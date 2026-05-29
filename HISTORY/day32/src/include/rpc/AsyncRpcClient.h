#pragma once
//
// AsyncRpcClient —— 基于 EventLoop 的全异步 RPC 客户端
//
// 设计动机：
//   旧版 RpcClient 是"每次调用 = 新建 fd + 阻塞 connect + 阻塞 read"的同步短连接，
//   必须配合 worker 线程池才能不卡 loop_。线程数固定 → 网络抖动时被超时占满，
//   后续 RPC 全部排队。
//
//   AsyncRpcClient 把"发送 + 等待 + 超时"完全嫁接到 EventLoop 的 IO 循环上，
//   消灭 worker 池：
//     · 每个目标 peer 保持一条长连接（断了自动重连）
//     · 请求按 reqId 复用同一连接，回包到达时通过 reqId 路由到等待的 callback
//     · 超时由 EventLoop 的定时器实现（不阻塞任何线程）
//
// 线程模型：
//   - 构造时绑定一个 Eventloop*；所有内部状态只在该 loop 线程读写。
//   - callAsync() 可被任意线程调用——内部 runInLoop 切回 loop_ 后再操作 pending_。
//   - callback 永远在 loop_ 线程触发。调用方若要把结果带回别的线程，自行 queueInLoop。
//
// 用法：
//   AsyncRpcClient client(&loop, "127.0.0.1", 18901);
//   client.callAsync("echo", "{\"hi\":1}",
//                    [](bool ok, std::string resp) {
//                        if (ok) LOG_INFO << "got " << resp;
//                    },
//                    /*timeoutMs=*/200);
//
#include "net/Connection.h"
#include "rpc/RpcMessage.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class Eventloop;
class Channel;

class AsyncRpcClient {
  public:
    using Callback = std::function<void(bool ok, std::string responseJson)>;

    AsyncRpcClient(Eventloop *loop, std::string ip, uint16_t port);
    ~AsyncRpcClient();

    // 任意线程调用；callback 在 loop_ 线程触发。
    void callAsync(const std::string &method, const std::string &requestJson, Callback cb,
                   int timeoutMs = 200);

    // 主动关闭：取消所有 pending 并关闭长连接。stop 后 callAsync 立刻以 ok=false 回调。
    void stop();

  private:
    enum class State { kIdle, kConnecting, kConnected, kStopped };

    struct PendingCall {
        Callback cb;
        uint64_t timerEpoch{0};
    };

    // —— 以下函数全部只在 loop_ 线程执行 ——
    void doCall(const std::string &method, const std::string &requestJson, Callback cb,
                int timeoutMs);
    void startConnectLocked();
    void onConnectWritable(int fd, uint64_t connEpoch);
    void onConnected(int fd);
    void onResponse(Connection *conn);
    void handleConnectionClosed();
    void cleanupConnectChannel();
    void flushPendingFrames();
    void failAllPending(const char *reason);
    void completeWithTimeout(uint32_t reqId, uint64_t timerEpoch);

    // —— 不可变配置 ——
    Eventloop  *loop_;
    std::string ip_;
    uint16_t    port_;

    // 指数退避（初始 500ms，每次翻倍，上限 30s；仅第一次连续失败打 WARN）
    struct ConnectBackoff {
        static constexpr int64_t kInitMs = 500;
        static constexpr int64_t kMaxMs  = 30'000;
        int64_t untilMs_   {0};
        int64_t durationMs_{0};
        int     failures_  {0};
        bool inBackoff(int64_t nowMs) const { return nowMs < untilMs_; }
        // 记录一次失败，指数增长退避时长；首次失败返回 true（建议打 WARN）
        bool recordFailure(int64_t nowMs) {
            durationMs_ = (durationMs_ == 0) ? kInitMs
                        : std::min(durationMs_ * 2, kMaxMs);
            untilMs_    = nowMs + durationMs_;
            return (failures_++ == 0);
        }
        void reset() { untilMs_ = 0; durationMs_ = 0; failures_ = 0; }
    };

    // —— loop_ 线程独占的状态 ——
    State                              state_{State::kIdle};
    std::unique_ptr<Connection>        conn_;
    std::unique_ptr<Channel>           connectChannel_;
    int                                connectFd_{-1};
    uint64_t                           connectEpoch_{0};  // 用于 cancel 超时 timer
    ConnectBackoff                     backoff_;          // 指数退避状态
    std::deque<std::string>            pendingFrames_;
    std::unordered_map<uint32_t, PendingCall> pending_;
    std::string                        recvBuf_;
    uint32_t                           nextReqId_{0};
};
