#pragma once

// 不需要 InetAddress 和 Channel 的前置声明了，因为移交给 Acceptor 了

#include "InetAddress.h"
class Eventloop;
class Socket;
class Acceptor;

class Server {
  private:
    Eventloop *loop;
    Acceptor *acceptor;

  public:
    Server(Eventloop *_loop);
    ~Server();

    // 【新增】提供给 Acceptor 的回调函数，当有新连接时调用
    void newConnection(Socket *client_sock, InetAddress *client_addr);

    // 删除了 handleReadEvent，因为那个逻辑现在被拆分了
};