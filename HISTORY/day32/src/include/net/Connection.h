#pragma once
#include "Buffer.h"
#include "Channel.h"
#include "Macros.h"
#include "Socket.h"
#include "timer/TimeStamp.h"
#include <any>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#ifdef MCPP_HAS_OPENSSL
struct ssl_st;
using SSL = ssl_st;
struct ssl_ctx_st;
using SSL_CTX = ssl_ctx_st;
#endif

class Eventloop;
class Buffer;

class Connection {
    DISALLOW_COPY_AND_MOVE(Connection)
  public:
    enum class State {
        kInvalid = 1,
        kConnected,
        kClosed,
        kFailed,
    };

    // ── 回压配置（Phase 1-2）──────────────────────────────────────────────
    // low < high < hardLimit
    // - buffered > high      -> 暂停读事件，优先排空写缓冲
    // - buffered <= low      -> 恢复读事件
    // - buffered > hardLimit -> 触发保护性断连，防止内存失控
    struct BackpressureConfig {
        size_t lowWatermarkBytes{4 * 1024 * 1024};
        size_t highWatermarkBytes{16 * 1024 * 1024};
        size_t hardLimitBytes{64 * 1024 * 1024};
    };

    struct BackpressureDecision {
        bool shouldPauseRead{false};
        bool shouldResumeRead{false};
        bool shouldCloseConnection{false};
    };

    // 构造参数改为 int fd，Socket 在内部创建
    Connection(int fd, Eventloop *loop);
    ~Connection();

    // 回压策略配置：传入非法配置会被忽略并打印告警。
    void setBackpressureConfig(const BackpressureConfig &cfg);
    const BackpressureConfig &backpressureConfig() const { return backpressureConfig_; }
    bool isReadPausedByBackpressure() const { return readPausedByBackpressure_; }

    static bool isValidBackpressureConfig(const BackpressureConfig &cfg);
    static BackpressureDecision evaluateBackpressure(size_t bufferedBytes, bool readPaused,
                                                     const BackpressureConfig &cfg);

#ifdef MCPP_HAS_OPENSSL
    // 启用 TLS 服务端模式，后续读写自动经 SSL_* 接口收发。
    bool enableTlsServer(SSL_CTX *ctx);
#endif
    bool tlsEnabled() const;

    void send(const std::string &msg);
    // 移动重载：当调用方持有临时字符串（如 resp.serialize() 的返回值）时，
    // 避免一次额外的字符串拷贝。热路径：HttpServer::onRequest() → conn->send(resp.serialize())
    void send(std::string &&msg);

    // 文件正文发送快路径：优先尝试 sendfile，不可用时降级为分块读取后发送。
    bool sendFile(const std::string &path, size_t offset, size_t count);

    void setOnMessageCallback(std::function<void(Connection *)> const &cb);
    // deleteCallback 改为 void(int fd)
    void setDeleteConnectionCallback(std::function<void(int)> _cb);
    void setOnConnectCallback(std::function<void(Connection *)> const &_cb);

    void close();

    // 对外接口，调用 doRead() 或 doWrite()
    void Read();
    void Write();
    void Business(); // Read() 后调用 on_message_callback_

    Socket *getSocket();
    State getState() const;
    Buffer *getInputBuffer();
    Buffer *getOutputBuffer();
    Eventloop *getLoop() const;

    // 在所有回调设置完成后，通过 queueInLoop 调用此方法来启用 Channel
    void enableInLoop();

    // 通用上下文槽：供上层协议（如 HttpContext）附挂每连接状态数据。
    // TCP 层对类型一无所知，上层用 std::any_cast<T> 取出。
    void setContext(std::any ctx) { context_ = std::move(ctx); }
    std::any &getContext() { return context_; }
    template <typename T> T *getContextAs() { return std::any_cast<T>(&context_); }

    // ── 空闲超时支持 ─────────────────────────────────────────────────────────
    // alive_：shared_ptr<bool> 作为"存活标志"，Connection 析构时置 false。
    // 定时器回调持有 weak_ptr<bool>，通过 lock() 判断连接是否仍在生命周期内，
    // 规避 raw pointer 悬空 UB（EventLoop 单线程执行保证时序安全）。
    std::weak_ptr<bool> aliveFlag() const { return alive_; }
    void touchLastActive() { lastActive_ = TimeStamp::now(); }
    TimeStamp lastActive() const { return lastActive_; }

  private:
    State state_{State::kInvalid};
    Eventloop *loop_;
    std::unique_ptr<Socket> sock_;
    std::unique_ptr<Channel> channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;

    std::any context_; // 上层协议附挂的每连接状态（如 HttpContext）

    // 空闲超时安全机制：析构时将 *alive_ 置 false，
    // 使持有 weak_ptr 的定时器回调能安全判断连接是否已销毁
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    TimeStamp lastActive_;

    BackpressureConfig backpressureConfig_{};
    bool readPausedByBackpressure_{false};

    std::function<void(int)> deleteConnectionCallback_;
    // 业务处理回调，当 Buffer 有数据时被调用
    std::function<void(Connection *)> onConnectCallback_;
    std::function<void(Connection *)> onMessageCallback_;

    // 原本调用 callback 的逻辑从这里分离，现在这两个函数只管IO
    void doRead();
    void doWrite();

    void applyBackpressureAfterAppend();
    void tryResumeReadAfterDrain();

#ifdef MCPP_HAS_OPENSSL
    enum class TlsState {
        kDisabled = 0,
        kHandshaking,
        kEstablished,
        kFailed,
    };

    ssize_t readFromTransport(char *buf, size_t len, int *savedErrno);
    ssize_t writeToTransport(const char *buf, size_t len, int *savedErrno);
    bool driveTlsHandshake();

    SSL *ssl_{nullptr};
    TlsState tlsState_{TlsState::kDisabled};
#endif
};