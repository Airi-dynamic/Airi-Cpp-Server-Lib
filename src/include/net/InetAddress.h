#pragma once
#include "Macros.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

class InetAddress {
    DISALLOW_COPY(InetAddress)
  public:
    struct sockaddr_in addr;
    socklen_t addr_len;

    InetAddress();
    InetAddress(const char *ip, uint16_t port);
    ~InetAddress();
};