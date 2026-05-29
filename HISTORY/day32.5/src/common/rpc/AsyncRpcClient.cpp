// AsyncRpcClient.cpp —— 基于 EventLoop 的全异步 RPC 客户端实现
//
// 阅读顺序：
//   §1 构造 / 析构 / stop
//   §2 callAsync（跨线程入口）→ doCall（loop_ 线程内核）
//   §3 非阻塞 connect 状态机：startConnect → onConnectWritable → onConnected
//   §4 收包路由：onResponse 解帧 → 查 pending_ map → 触发 callback
//   §5 错误路径：connection close / timeout / stop 时的 pending 清理
//
#include "rpc/AsyncRpcClient.h"

#include "net/Channel.h"
#include "net/Connection.h"
#include "net/EventLoop.h"
#include "log/Logger.h"

#include <chrono>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
// 与 RpcServer 保持一致的帧上限：防止对端响应声明天价 length。
constexpr uint32_t kMaxRpcPayloadBytes = 16u * 1024u * 1024u;

// 返回当前单调时钟毫秒数（用于连接失败退避计时）
inline int64_t nowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

// ════════════════════════════════════════════════════════════════════════════
// §1 构造 / 析构 / stop
// ════════════════════════════════════════════════════════════════════════════

AsyncRpcClient::AsyncRpcClient(Eventloop *loop, std::string ip, uint16_t port)
    : loop_(loop), ip_(std::move(ip)), port_(port) {}

AsyncRpcClient::~AsyncRpcClient() { stop(); }

void AsyncRpcClient::stop() {
    // 必须切回 loop_ 线程销毁 conn_ / channel_（poller 操作要求归属线程）。
    // 使用 promise/atomic 等待的话又会引入跨线程同步——
    // 这里假设 stop() 由 loop_ 所在线程的拥有者（如 RaftNode::stop()）在 join loop 前调用，
    // 投递任务后调用方再 quit + join loop_，loop_ 退出前会 doPendingFunctors() 把这些跑完。
    loop_->queueInLoop([this] {
        if (state_ == State::kStopped) return;
        state_ = State::kStopped;
        failAllPending("client stopped");
        cleanupConnectChannel();
        // conn_ 析构会走 ~Connection → loop_->deleteChannel，
        // 当前已经在 loop_ 线程，安全。
        conn_.reset();
    });
}

// ════════════════════════════════════════════════════════════════════════════
// §2 callAsync —— 任意线程入口；真正逻辑在 doCall（loop_ 线程）
// ════════════════════════════════════════════════════════════════════════════

void AsyncRpcClient::callAsync(const std::string &method, const std::string &requestJson,
                               Callback cb, int timeoutMs) {
    // 注意：method/requestJson 在 lambda 中按值拷贝，确保跨线程生命周期安全。
    loop_->runInLoop(
        [this, method, requestJson, cb = std::move(cb), timeoutMs]() mutable {
            doCall(method, requestJson, std::move(cb), timeoutMs);
        });
}

void AsyncRpcClient::doCall(const std::string &method, const std::string &requestJson, Callback cb,
                            int timeoutMs) {
    if (state_ == State::kStopped) {
        cb(false, "");
        return;
    }

    // 1) 分配 reqId 并登记 pending
    const uint32_t reqId = ++nextReqId_;
    PendingCall    pc;
    pc.cb         = std::move(cb);
    pc.timerEpoch = reqId; // 每个 reqId 一个 epoch（够用，因为 reqId 单调递增）
    pending_.emplace(reqId, std::move(pc));

    // 2) 超时定时器：用 reqId 自身作为 epoch 标识
    loop_->runAfter(timeoutMs / 1000.0,
                    [this, reqId] { completeWithTimeout(reqId, reqId); });

    // 3) 编码消息帧
    RpcMessage msg;
    msg.type    = RpcMessage::Type::kRequest;
    msg.reqId   = reqId;
    msg.method  = method;
    msg.payload = requestJson;
    std::string frame = msg.encode();

    // 4) 投递
    if (state_ == State::kConnected && conn_) {
        conn_->send(std::move(frame));
        return;
    }
    pendingFrames_.emplace_back(std::move(frame));
    if (state_ == State::kIdle) startConnectLocked();
}

// ════════════════════════════════════════════════════════════════════════════
// §3 非阻塞 connect 状态机
// ════════════════════════════════════════════════════════════════════════════

void AsyncRpcClient::startConnectLocked() {
    // 连接失败退避：若距上次失败时刻尚未过退避期，直接失败所有待处理请求而不再尝试 TCP 连接
    if (backoff_.inBackoff(nowSteadyMs())) {
        failAllPending("connect backoff");
        return;
    }
    state_ = State::kConnecting;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR << "[AsyncRpcClient] socket() 失败: " << strerror(errno);
        state_ = State::kIdle;
        failAllPending("socket() failed");
        return;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    ::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (rc == 0) {
        // 极少见：loopback 立即完成
        onConnected(fd);
        return;
    }
    if (errno != EINPROGRESS) {
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] connect 立即失败 " << ip_ << ":" << port_ << " err="
                     << strerror(errno);
        ::close(fd);
        state_ = State::kIdle;
        failAllPending("connect failed");
        return;
    }

    // EINPROGRESS：等可写
    connectFd_                = fd;
    const uint64_t connEpoch  = ++connectEpoch_;
    connectChannel_           = std::make_unique<Channel>(loop_, fd);
    connectChannel_->setWriteCallback([this, fd, connEpoch] {
        if (connEpoch != connectEpoch_) return; // 已被新一轮 connect 顶替
        onConnectWritable(fd, connEpoch);
    });
    // 错误信号也会让 write 就绪 / read HUP，统一在 write 回调里通过 SO_ERROR 判定。
    connectChannel_->enableWriting();

    // 连接整体超时（独立于业务超时）：3 秒兜底
    loop_->runAfter(3.0, [this, connEpoch] {
        if (connEpoch != connectEpoch_) return;
        if (state_ != State::kConnecting) return;
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] connect 超时 " << ip_ << ":" << port_;
        cleanupConnectChannel();
        state_ = State::kIdle;
        failAllPending("connect timeout");
    });
}

void AsyncRpcClient::onConnectWritable(int fd, uint64_t /*connEpoch*/) {
    int       err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    cleanupConnectChannel();
    if (err != 0) {
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] async connect 失败 " << ip_ << ":" << port_ << " err="
                     << strerror(err);
        ::close(fd);
        state_ = State::kIdle;
        failAllPending("async connect failed");
        return;
    }
    onConnected(fd);
}

void AsyncRpcClient::onConnected(int fd) {
    ++connectEpoch_; // 让任何遗留的 connect-timer 立刻失效
    backoff_.reset(); // 连接成功，重置退避状态
    state_ = State::kConnected;

    conn_ = std::make_unique<Connection>(fd, loop_);
    conn_->setDeleteConnectionCallback([this](int /*fd*/) {
        // 由 Connection::close() 同步调入；当下仍在 Business() 调用栈上，
        // 不能直接 reset(conn_)（会 UAF）。延迟到 doPendingFunctors() 处理。
        loop_->queueInLoop([this] { handleConnectionClosed(); });
    });
    conn_->setOnMessageCallback([this](Connection *c) { onResponse(c); });
    conn_->enableMessageMode(); // 显式切换到 Business() 读模式
    conn_->enableInLoop();

    flushPendingFrames();
}

void AsyncRpcClient::cleanupConnectChannel() {
    if (connectChannel_) {
        connectChannel_->disableAll();
        loop_->deleteChannel(connectChannel_.get());
        connectChannel_.reset();
    }
    connectFd_ = -1;
}

void AsyncRpcClient::flushPendingFrames() {
    if (!conn_) return;
    while (!pendingFrames_.empty()) {
        conn_->send(std::move(pendingFrames_.front()));
        pendingFrames_.pop_front();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §4 收包路由：reqId → callback
// ════════════════════════════════════════════════════════════════════════════

void AsyncRpcClient::onResponse(Connection *conn) {
    Buffer *buf = conn->getInputBuffer();
    size_t  n   = buf->readableBytes();
    recvBuf_.append(buf->peek(), n);
    buf->retrieve(n);

    while (true) {
        // 防御性：peek 头 4 字节 length，过大直接关连接（对端可能恶意/损坏）
        if (recvBuf_.size() >= 4) {
            uint32_t netLen = 0;
            std::memcpy(&netLen, recvBuf_.data(), 4);
            uint32_t payloadLen = ntohl(netLen);
            if (payloadLen > kMaxRpcPayloadBytes) {
                LOG_WARN << "[AsyncRpcClient] 对端响应 length 超限，关闭连接 " << ip_ << ":"
                         << port_ << " claimed=" << payloadLen
                         << " limit=" << kMaxRpcPayloadBytes;
                conn->close();
                return;
            }
        }

        RpcMessage resp;
        int        consumed = 0;
        bool       ok = RpcMessage::decode(recvBuf_.data(), static_cast<int>(recvBuf_.size()),
                                           &resp, &consumed);
        if (!ok) break;
        recvBuf_.erase(0, consumed);

        if (resp.type != RpcMessage::Type::kResponse) continue;
        auto it = pending_.find(resp.reqId);
        if (it == pending_.end()) {
            // 已超时被清理，丢弃即可
            continue;
        }
        Callback cb = std::move(it->second.cb);
        pending_.erase(it);
        if (cb) cb(true, std::move(resp.payload));
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §5 错误 / 超时清理
// ════════════════════════════════════════════════════════════════════════════

void AsyncRpcClient::handleConnectionClosed() {
    LOG_INFO << "[AsyncRpcClient] 与 " << ip_ << ":" << port_ << " 的连接已断开";
    conn_.reset();
    recvBuf_.clear();
    if (state_ == State::kStopped) {
        failAllPending("client stopped during close");
        return;
    }
    state_ = State::kIdle;
    // 失败所有已发出的 pending，让上层重试。下次 callAsync 会自动重新 connect。
    failAllPending("connection lost");
}

void AsyncRpcClient::failAllPending(const char *reason) {
    // 业务回调可能在内部又触发 callAsync，因此先 move 出来再清空。
    std::unordered_map<uint32_t, PendingCall> snapshot;
    snapshot.swap(pending_);
    pendingFrames_.clear();
    for (auto &kv : snapshot) {
        if (kv.second.cb) kv.second.cb(false, "");
    }
    if (reason && *reason) {
        LOG_DEBUG << "[AsyncRpcClient] 待处理请求已清空：" << reason;
    }
}

void AsyncRpcClient::completeWithTimeout(uint32_t reqId, uint64_t /*timerEpoch*/) {
    auto it = pending_.find(reqId);
    if (it == pending_.end()) return; // 已收到响应，timer 自然失效
    Callback cb = std::move(it->second.cb);
    pending_.erase(it);
    if (cb) cb(false, "");
}

// ════════════════════════════════════════════════════════════════════════════
// §6 协程出口：RpcCallAwaiter + callAsyncCo
// ════════════════════════════════════════════════════════════════════════════

// callAsyncCo — 构造 RpcCallAwaiter 并返回（工厂函数）。
// 不需要在 loop_ 线程调用（callAsync 内部已 runInLoop）。
RpcCallAwaiter AsyncRpcClient::callAsyncCo(const std::string &method,
                                            const std::string &requestJson,
                                            int timeoutMs) {
    return RpcCallAwaiter{this, method, requestJson, timeoutMs};
}

// RpcCallAwaiter::await_suspend — co_await 的挂起入口。
//
// 调用时序：
//   1. 协程在 co_await 处被编译器挂起（帧已保存），await_suspend(h) 被调用。
//   2. 向 callAsync 注册 callback lambda，lambda 捕获：
//      - this：RpcCallAwaiter* （存储在协程帧上，帧存活时安全）
//      - h   ：std::coroutine_handle<> （拷贝廉价，句柄是个指针）
//   3. callAsync 立刻返回，await_suspend 返回 void（协程继续挂起）。
//   4. 当 RPC 响应到达（或超时），callback 在 loop_ 线程触发：
//      先写 ok_/resp_（帧存活），再 h.resume()。
//   5. h.resume() 内部调用 await_resume()，取出 {ok_, resp_} 作为 co_await 结果。
//
void RpcCallAwaiter::await_suspend(std::coroutine_handle<> h) {
    client_->callAsync(
        method_, json_,
        [this, h](bool ok, std::string resp) mutable {
            // callback 在 loop_ 线程触发（callAsync 约定）。
            // 写入结果到协程帧（写在 resume 之前，保证 await_resume 可见）。
            ok_   = ok;
            resp_ = std::move(resp);
            // 恢复协程：也在 loop_ 线程，维持 single-thread invariant。
            h.resume();
        },
        timeoutMs_);
}
