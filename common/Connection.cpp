#include "Connection.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include "log/Logger.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sys/types.h>
#include <unistd.h>

#define READ_BUFFER 1024

Connection::Connection(int fd, Eventloop *loop)
    : loop_(loop), sock_(std::make_unique<Socket>(fd)), channel_(nullptr) {
    channel_ = std::make_unique<Channel>(loop_, sock_->getFd());
    channel_->setReadCallback(std::bind(&Connection::doRead, this));
    channel_->setWriteCallback(std::bind(&Connection::doWrite, this));
    // 不在构造函数中 enableReading / enableET，
    // 由 TcpServer::newConnection 设置好所有回调后，通过 enableInLoop() 启用
    state_ = State::kConnected;
}

Connection::~Connection() {
    // alive_ 置 false：通知所有持有 weak_ptr<bool> 的定时器回调此连接已销毁，
    // 避免回调在 Connection 内存释放后通过 raw pointer 访问悬空对象
    *alive_ = false;
    // 再从 Poller 中注销 Channel，防止 kqueue/epoll 保留悬空 udata 指针
    loop_->deleteChannel(channel_.get());
}

void Connection::doRead() {
    int sockfd = sock_->getFd();
    // ET 模式必须循环读直到 EAGAIN，否则状态只变化一次，内核不再通知，
    // 导致缓冲区中残留数据却永远不被读取，客户端卡死在 read 等待 echo
    while (true) {
        int savedErrno = 0;
        ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);
        if (n > 0) {
            // 本次读到数据，继续循环尝试读取更多（ET 模式可能有大量数据）
            continue;
        } else if (n == 0) {
            state_ = State::kClosed;
            LOG_INFO << "[server] client fd " << sockfd << " disconnected.";
            // 不在此处调用 close()，由 Business() 在所有回调完成后统一触发
            // 防止 close() → queueInLoop(删除) 后 Main Reactor 立刻析构 Connection
            // 而 Business() 中 onMessageCallback_(this) 尚未执行完毕，导致 use-after-free
            break;
        } else {
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
                // 内核缓冲区已读空，ET 下这是正常退出条件
                break;
            }
            state_ = State::kFailed;
            LOG_ERROR << "[server] read error on fd " << sockfd << ": " << strerror(savedErrno);
            // 同上，由 Business() 统一触发删除
            break;
        }
    }
    // 执行到此处：数据已尽数读入 inputBuffer（或连接异常）
    // 指令流转移到 Business() 继续处理
}

void Connection::doWrite() {
    if (state_ != State::kConnected)
        return;
    if (channel_->isWriting()) {
        ssize_t n = ::write(sock_->getFd(), outputBuffer_.peek(), outputBuffer_.readableBytes());
        // 不一定能一次全发出去
        if (n > 0) {
            // 一部分数据已被输出，对应 Buffer 区域被划入废弃区域
            outputBuffer_.retrieve(n);
            // 如果全发完了，通知 epoll 本 channel 不再需要发数据
            if (outputBuffer_.readableBytes() == 0) {
                channel_->disableWriting();
            }
        }
    }
}

void Connection::send(const std::string &msg) {
    if (msg.empty())
        return;

    ssize_t nwrote = 0;
    size_t remaining = msg.size();
    bool faultError = false;

    // 1. 如果当前没有正在等待写的（OutputBuffer 为空），且 socket 可写
    // 尝试直接 write，减少一次 buffer copy
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(sock_->getFd(), msg.data(), msg.size());
        if (nwrote >= 0) {
            remaining = msg.size() - nwrote;
        } else {
            nwrote = 0;
            //
            if ((errno != EWOULDBLOCK) && (errno == EPIPE || errno == ECONNRESET)) {
                faultError = true;
            }
        }
    }

    // 2. 如果刚才没写完（或者 OutputBuffer 本来就有积压），把剩下的追加到 OutputBuffer
    // 并注册写事件，等待内核通知
    if (!faultError && remaining > 0) {
        outputBuffer_.append(msg.data() + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void Connection::Business() {
    // 状态守卫：kqueue/epoll 的同一批次事件中，同一 fd 可能出现多次
    // （如 EVFILT_READ + EVFILT_WRITE 同帧返回）。
    // 若第一个事件已经探测到 EOF/错误并触发 close()，Connection 尚未被删除
    // （删除是异步的，通过 queueInLoop 投递到本线程的 doPendingFunctors() 延迟执行），
    // 第二个事件不应再进入 doRead() 对一个关闭中的 fd 做读写。
    if (state_ != State::kConnected)
        return;

    doRead();
    if (state_ == State::kConnected) {
        // 连接正常：执行业务回调
        if (onMessageCallback_)
            onMessageCallback_(this);
    } else {
        // 连接已关闭或出错：close() 作为最后一步执行
        // 此时所有对 this 的访问已结束，close() 触发 queueInLoop(删除) 后
        // Business() 立刻返回，不再访问任何成员，Main Reactor 可安全析构 Connection
        close();
    }
}

void Connection::Read() { doRead(); }

void Connection::Write() { doWrite(); }

void Connection::close() {
    if (deleteConnectionCallback_)
        deleteConnectionCallback_(sock_->getFd()); // 传 fd 不传指针
}

Connection::State Connection::getState() const { return state_; }

Socket *Connection::getSocket() { return sock_.get(); }

Buffer *Connection::getInputBuffer() { return &inputBuffer_; }

Buffer *Connection::getOutputBuffer() { return &outputBuffer_; };

void Connection::setDeleteConnectionCallback(std::function<void(int)> cb) {
    deleteConnectionCallback_ = std::move(cb);
}

void Connection::setOnConnectCallback(std::function<void(Connection *)> const &_cb) {
    onConnectCallback_ = _cb;
}

void Connection::setOnMessageCallback(std::function<void(Connection *)> const &cb) {
    onMessageCallback_ = cb;
    channel_->setReadCallback(std::bind(&Connection::Business, this));
}

void Connection::enableInLoop() {
    channel_->enableReading();
    channel_->enableET();
}

Eventloop *Connection::getLoop() const { return loop_; }