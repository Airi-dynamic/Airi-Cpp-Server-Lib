#pragma once
#include "Buffer.h"
#include "Channel.h"
#include "Macros.h"
#include "Socket.h"
#include "timer/TimeStamp.h"
#include <any>
#include <functional>
#include <memory>

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

    // 构造参数改为 int fd，Socket 在内部创建
    Connection(int fd, Eventloop *loop);
    ~Connection();

    void send(const std::string &msg);

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
    template <typename T>
    T *getContextAs() { return std::any_cast<T>(&context_); }

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

    std::function<void(int)> deleteConnectionCallback_;
    // 业务处理回调，当 Buffer 有数据时被调用
    std::function<void(Connection *)> onConnectCallback_;
    std::function<void(Connection *)> onMessageCallback_;

    // 原本调用 callback 的逻辑从这里分离，现在这两个函数只管IO
    void doRead();
    void doWrite();
};