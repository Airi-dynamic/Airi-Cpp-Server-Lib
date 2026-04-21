#pragma once
#include "Buffer.h"
#include <functional>

class Eventloop;
class Socket;
class Channel;
class Buffer;

class Connection {
  private:
    Eventloop *loop;
    Socket *sock;
    Channel *channel;
    Buffer inputBuffer_;
    Buffer outputBuffer_;

    std::function<void(Socket *)> deleteConnectionCallback;

  public:
    Connection(Eventloop *_loop, Socket *_sock);
    ~Connection();

    void handleRead();
    void handleWrite();
    void send(const std::string &msg);

    void setDeleteConnectionCallback(std::function<void(Socket *)> _cb);
};