#pragma once
#include "Macros.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef MCPP_HAS_OPENSSL
struct ssl_ctx_st;
using SSL_CTX = ssl_ctx_st;
#endif

class Connection;
class Eventloop;
class Acceptor;
class EventLoopThreadPool;

class TcpServer {
    DISALLOW_COPY_AND_MOVE(TcpServer)
  public:
    struct Options {
        std::string listenIp{"127.0.0.1"};
        uint16_t listenPort{8888};
        int ioThreads{0}; // <=0 表示按 hardware_concurrency 自动选择
        size_t maxConnections{10000};

        struct TlsOptions {
            bool enabled{false};
            std::string certFile;
            std::string keyFile;
        };

        TlsOptions tls;
    };

  private:
    Options options_{};
    std::unique_ptr<Eventloop> mainReactor_;
    std::unique_ptr<Acceptor> acceptor_;

    // ── 析构顺序说明（C++ 成员按声明逆序析构）──────────────────────────────
    // 声明顺序：mainReactor_(1) → acceptor_(2) → threadPool_(3) → connections_(4)
    // 析构顺序：connections_(1st) → threadPool_(2nd) → acceptor_(3rd) → mainReactor_(4th)
    //
    // 但 connections_ 析构时 Connection::~Connection() 会调 loop->deleteChannel()，
    // 此时要求：① IO 线程已退出（无竞态）；② EventLoop 对象仍然存活（不是野指针）。
    //
    // 满足条件的方式：
    //   ~TcpServer() 先显式调 threadPool_->joinAll()（join 线程但不销毁 EventLoop），
    //   然后成员按逆序自动析构：
    //     connections_ 析构：IO 线程已 join，EventLoop 存活于 threadPool_ 中 ✓
    //     threadPool_  析构：EventLoopThread 析构 → join(no-op) → EventLoop 最终销毁 ✓
    // ────────────────────────────────────────────────────────────────────────
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    size_t maxConnections_{10000};
    bool tlsEnabled_{false};

#ifdef MCPP_HAS_OPENSSL
    struct SslCtxDeleter {
        void operator()(SSL_CTX *ctx) const;
    };

    std::unique_ptr<SSL_CTX, SslCtxDeleter> tlsCtx_{nullptr};
#endif

    std::function<void(Connection *)> onMessageCallback_;
    std::function<void(Connection *)> newConnectCallback_;

    bool initTlsIfNeeded();

  public:
    TcpServer();
    explicit TcpServer(const Options &options);
    ~TcpServer();

    // 最大连接数保护（防止连接洪峰导致资源耗尽）
    void setMaxConnections(size_t maxConnections);
    size_t maxConnections() const { return maxConnections_; }
    size_t connectionCount() const { return connections_.size(); }
    bool tlsEnabled() const { return tlsEnabled_; }

    // 纯策略函数：便于单元测试，不依赖网络环境。
    static bool shouldRejectNewConnection(size_t currentCount, size_t maxConnections);
    static int normalizeIoThreadCount(int configured, unsigned int hardwareCount);

    void Start(); // 启动所有 sub-reactor 线程和 main-reactor 循环
    void stop();  // 令所有 Reactor 退出循环

    void newConnection(int fd);
    void deleteConnection(int fd);

    void onMessage(std::function<void(Connection *)> fn);
    void newConnect(std::function<void(Connection *)> fn);
};
