#include "TcpServer.h"
#include "Acceptor.h"
#include "Connection.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "log/Logger.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#ifdef MCPP_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#ifdef MCPP_HAS_OPENSSL
void TcpServer::SslCtxDeleter::operator()(SSL_CTX *ctx) const {
    if (ctx)
        SSL_CTX_free(ctx);
}
#endif

int TcpServer::normalizeIoThreadCount(int configured, unsigned int hardwareCount) {
    if (configured > 0)
        return configured;
    if (hardwareCount == 0)
        return 1;
    return static_cast<int>(hardwareCount);
}

TcpServer::TcpServer() : TcpServer(Options{}) {}

TcpServer::TcpServer(const Options &options) : options_(options) {
    mainReactor_ = std::make_unique<Eventloop>();

    acceptor_ = std::make_unique<Acceptor>(mainReactor_.get(), options_.listenIp.c_str(),
                                           options_.listenPort);
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1));

    int threadNum = normalizeIoThreadCount(options_.ioThreads, std::thread::hardware_concurrency());
    threadPool_ = std::make_unique<EventLoopThreadPool>(mainReactor_.get());
    threadPool_->setThreadNums(threadNum);

    if (options_.maxConnections > 0)
        maxConnections_ = options_.maxConnections;

    if (!initTlsIfNeeded()) {
        throw std::runtime_error("tcp server tls init failed");
    }

    LOG_INFO << "[TcpServer] Main Reactor + " << threadNum
             << " Sub Reactors ready. listen=" << options_.listenIp << ":" << options_.listenPort
             << " maxConnections=" << maxConnections_ << " tls=" << (tlsEnabled_ ? "on" : "off");
}

bool TcpServer::initTlsIfNeeded() {
    if (!options_.tls.enabled) {
        tlsEnabled_ = false;
        return true;
    }

#ifndef MCPP_HAS_OPENSSL
    LOG_ERROR << "[TcpServer] TLS 已启用但当前构建未启用 OpenSSL（MCPP_HAS_OPENSSL）";
    return false;
#else
    if (options_.tls.certFile.empty() || options_.tls.keyFile.empty()) {
        LOG_ERROR << "[TcpServer] TLS 已启用但证书/私钥路径为空";
        return false;
    }

    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    SSL_CTX *rawCtx = SSL_CTX_new(TLS_server_method());
    if (!rawCtx) {
        LOG_ERROR << "[TcpServer] SSL_CTX_new 失败";
        return false;
    }

    SSL_CTX_set_mode(rawCtx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_min_proto_version(rawCtx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(rawCtx, options_.tls.certFile.c_str(), SSL_FILETYPE_PEM) !=
        1) {
        unsigned long err = ERR_get_error();
        LOG_ERROR << "[TcpServer] 加载 TLS 证书失败: " << options_.tls.certFile
                  << " err=" << ERR_error_string(err, nullptr);
        SSL_CTX_free(rawCtx);
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(rawCtx, options_.tls.keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
        unsigned long err = ERR_get_error();
        LOG_ERROR << "[TcpServer] 加载 TLS 私钥失败: " << options_.tls.keyFile
                  << " err=" << ERR_error_string(err, nullptr);
        SSL_CTX_free(rawCtx);
        return false;
    }

    if (SSL_CTX_check_private_key(rawCtx) != 1) {
        unsigned long err = ERR_get_error();
        LOG_ERROR << "[TcpServer] TLS 私钥与证书不匹配"
                  << " err=" << ERR_error_string(err, nullptr);
        SSL_CTX_free(rawCtx);
        return false;
    }

    tlsCtx_.reset(rawCtx);
    tlsEnabled_ = true;
    LOG_INFO << "[TcpServer] TLS 已启用 cert=" << options_.tls.certFile;
    return true;
#endif
}

void TcpServer::setMaxConnections(size_t maxConnections) {
    if (maxConnections == 0) {
        LOG_WARN << "[TcpServer] 最大连接数不能为0，忽略本次设置";
        return;
    }
    maxConnections_ = maxConnections;
    LOG_INFO << "[TcpServer] 最大连接数已更新: " << maxConnections_;
}

bool TcpServer::shouldRejectNewConnection(size_t currentCount, size_t maxConnections) {
    return currentCount >= maxConnections;
}

void TcpServer::Start() {
    // 1. 启动所有 IO 线程：每个线程在内部构造 EventLoop 并进入 loop()
    //    start() 内部同步等待每个 EventLoop 就绪后才返回，
    //    此后 nextLoop() 可安全使用
    threadPool_->start();

    // 2. 主线程进入 main-reactor 循环（阻塞直到 stop()）
    mainReactor_->loop();
}

void TcpServer::newConnection(int fd) {
    if (fd == -1) {
        LOG_WARN << "[TcpServer] 收到无效连接 fd=-1，已忽略";
        return;
    }

    if (shouldRejectNewConnection(connections_.size(), maxConnections_)) {
        LOG_WARN << "[TcpServer] 连接数达到上限，拒绝新连接 fd=" << fd
                 << " current=" << connections_.size() << " max=" << maxConnections_;
        ::close(fd);
        return;
    }

    // 轮询选择一个 sub-reactor 归属这条连接
    Eventloop *subLoop = threadPool_->nextLoop();
    auto conn = std::make_unique<Connection>(fd, subLoop);

#ifdef MCPP_HAS_OPENSSL
    if (tlsEnabled_) {
        if (!conn->enableTlsServer(tlsCtx_.get())) {
            LOG_ERROR << "[TcpServer] 连接 TLS 初始化失败，关闭 fd=" << fd;
            return;
        }
    }
#endif

    // 先注入所有回调，再通过 queueInLoop 在归属 sub-reactor 线程启用 Channel，
    // 避免 Channel 就绪触发事件时回调尚未设置
    conn->setOnMessageCallback(onMessageCallback_);
    conn->setDeleteConnectionCallback(
        std::bind(&TcpServer::deleteConnection, this, std::placeholders::_1));

    Connection *rawConn = conn.get();
    connections_[fd] = std::move(conn);

    if (newConnectCallback_)
        newConnectCallback_(rawConn);

    // queueInLoop 跨线程投递：在 sub-reactor 线程启用读事件（enableInLoop），
    // 此时 connections_[fd] 已插入 map，回调均已就绪
    subLoop->queueInLoop([rawConn]() { rawConn->enableInLoop(); });

    LOG_INFO << "[TcpServer] new connection fd=" << fd;
}

void TcpServer::deleteConnection(int fd) {
    // 本函数由 sub-reactor 线程经 Connection::close() 回调链调用，
    // 需切换到 main-reactor 线程操作 connections_ map（逻辑隔离，无需 mutex）
    mainReactor_->queueInLoop([this, fd]() {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            Eventloop *ioLoop = it->second->getLoop();
            std::unique_ptr<Connection> conn = std::move(it->second);
            connections_.erase(it);
            LOG_INFO << "[TcpServer] connection fd=" << fd << " deleted.";

            // 将 Connection 析构投递到其归属 sub-reactor 线程执行：
            // 保证 Connection::~Connection()（调用 loop_->deleteChannel）
            // 发生在该 sub-reactor 本次 poll() 结束、events_ 遍历完毕之后，
            // 消除 Channel* 悬空野指针风险。
            // shared_ptr 包装 unique_ptr：满足 std::function 对可复制性的要求，
            // 引用计数归零时自动析构 Connection，无需 release()+delete。
            std::shared_ptr<Connection> guard(std::move(conn));
            ioLoop->queueInLoop([guard]() {
                // guard 在此作用域结束时析构，Connection 被安全释放
            });
        }
    });
}

void TcpServer::onMessage(std::function<void(Connection *)> fn) {
    onMessageCallback_ = std::move(fn);
}
void TcpServer::newConnect(std::function<void(Connection *)> fn) {
    newConnectCallback_ = std::move(fn);
}

void TcpServer::stop() {
    // 通知所有 sub-reactor 退出 loop()
    threadPool_->stopAll();
    // 通知 main-reactor 退出 loop()，使 Start() 函数得以返回
    mainReactor_->setQuit();
    mainReactor_->wakeup();
}

TcpServer::~TcpServer() {
    // 1. 令所有 reactor 退出 loop()（幂等，重复调用无害）
    stop();
    // 2. 显式等待所有 IO 线程实际退出（join），
    //    之后 connections_ 析构时调用 loop->deleteChannel() 才没有竞态风险
    threadPool_->joinAll();
    // 3. 成员按声明逆序自动析构（详见 TcpServer.h 注释）
}
