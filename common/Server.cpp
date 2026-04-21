#include "Server.h"
#include "Acceptor.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <unistd.h>

#define READ_BUFFER 1024

Server::Server(Eventloop *_loop) : loop(_loop) {
    // 现在资源初始化交由 Acceptor
    acceptor = new Acceptor(loop);
    std::function<void(Socket *, InetAddress *)> cb =
        std::bind(&Server::newConnection, this, std::placeholders::_1, std::placeholders::_2);
    acceptor->setNewConnectionCallback(cb);
}

Server::~Server() { delete acceptor; }

// 这个函数就是以前 accept 之后的那部分逻辑
void Server::newConnection(Socket *client_sock, InetAddress *client_addr) {
    // 注意：server_sock 是 Acceptor new 出来传给我们的
    // 在 Day 7 阶段，为了防止内存泄漏，Server 应该接管这个 Socket 的生命周期
    // 但因为还没写 TCPConnection 类，这里暂且还是只能看着它泄漏或者简单 delete

    int client_sockfd = client_sock->getFd();

    // 下面的逻辑和 Day 6 的 handleReadEvent 后半部分一样
    // 为客户端 socket 建立 Channel，设置读回调

    std::cout << "[server] new client fd" << client_sockfd
              << "! IP:" << inet_ntoa(client_addr->addr.sin_addr) << " PortL "
              << ntohs(client_addr->addr.sin_port) << std::endl;

    // 为新的客户端连接创建 Socket 和 Channel
    // 还是有内存泄漏，以后处理

    Channel *clientChannel = new Channel(loop, client_sockfd);

    // 数据处理的lambda函数
    std::function<void()> cb = [=] {
        char buf[READ_BUFFER];
        while (true) {
            bzero(&buf, sizeof(buf));
            ssize_t bytes_read = read(client_sockfd, buf, sizeof(buf));
            if (bytes_read > 0) {
                std::cout << "[server] message from client fd " << client_sockfd << ": " << buf
                          << std::endl;
                write(client_sockfd, buf, bytes_read);
            } else if (bytes_read == -1 && errno == EINTR) {
                continue;
            } else if (bytes_read == -1 && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
                break;
            } else if (bytes_read == 0) {
                std::cout << "[server] client fd " << client_sockfd << " disconnected."
                          << std::endl;
                close(client_sockfd);
                // 这里需要 delete clntChannel，但 lambda 里很难删自己
                // 所以我们暂且不管内存泄漏
                break;
            }
        }
    };

    clientChannel->setCallback(cb);
    clientChannel->enableReading();
}