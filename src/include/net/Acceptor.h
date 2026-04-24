#pragma once
#include "Macros.h"
#include <cstdint>
#include <functional>
#include <memory>

class Eventloop;
class Socket;
class InetAddress;
class Channel;

class Acceptor {
    DISALLOW_COPY(Acceptor)
  private:
    std::unique_ptr<Socket> sock_;
    std::unique_ptr<Channel> acceptChannel_;
    // Acceptor 获取到新连接后，需要回调 Server 的函数
    // 参数是新客户端的文件描述符
    std::function<void(int)> newConnectionCallback_; // 只传 fd

  public:
    explicit Acceptor(Eventloop *_loop, const char *ip = "127.0.0.1", uint16_t port = 8888);
    ~Acceptor(); // unique_ptr 自动析构

    void acceptConnection(); // 以前 Server::handleReadEvent 的创建新连接的逻辑
    void setNewConnectionCallback(std::function<void(int)> cb);
};