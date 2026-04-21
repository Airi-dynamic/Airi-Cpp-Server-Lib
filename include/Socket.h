#pragma once
#include "InetAddress.h"
#include "Macros.h"

class Socket {
    DISALLOW_COPY_AND_MOVE(Socket)
  private:
    int fd_;

  public:
    Socket();
    Socket(int fd);
    ~Socket();

    bool isValid() const { return fd_ != -1; }

    // 失败时返回 false，并由调用方决定降级或终止。
    bool bind(InetAddress *addr);
    bool listen();

    // 返回新连接 fd；失败时返回 -1（由上层根据 errno 决定是否忽略或记录）
    int accept(InetAddress *addr);

    // 在且仅在客户端 Socket 调用
    bool connect(InetAddress *addr);

    int getFd();

    bool setnonblocking();

    bool isNonBlocking();
};