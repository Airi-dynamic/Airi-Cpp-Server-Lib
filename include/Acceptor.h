#pragma once
#include <functional>

class Eventloop;
class Socket;
class InetAddress;
class Channel;

class Acceptor {
  private:
    Eventloop *loop;
    Socket *sock;
    InetAddress *addr;
    Channel *acceptChannel;

    // Acceptor 获取到新连接后，需要回调 Server 的函数
    // 参数是新客户端的文件描述符
    std::function<void(Socket *, InetAddress *)> newConnectionCallback;

  public:
    Acceptor(Eventloop *_loop);
    ~Acceptor();

    void acceptConnection(); // 以前 Server::handleReadEvent 的创建新连接的逻辑
    void setNewConnectionCallback(std::function<void(Socket *, InetAddress *)> _cb);
};