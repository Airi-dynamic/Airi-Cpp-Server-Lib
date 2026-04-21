#include "Server.h"
#include "Acceptor.h"
#include "Connection.h"
#include "InetAddress.h"
#include "Socket.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>

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
    // 注意：clnt_addr 在这里打印完就可以删了，或者保存到 Connection 里（Day 8 先不保存）
    std::cout << "[server] new client fd" << client_sock->getFd()
              << "! IP:" << inet_ntoa(client_addr->addr.sin_addr) << " PortL "
              << ntohs(client_addr->addr.sin_port) << std::endl;

    Connection *conn = new Connection(loop, client_sock);

    std::function<void(Socket *)> cb =
        std::bind(&Server::deleteConnection, this, std::placeholders::_1);
    conn->setDeleteConnectionCallback(cb);

    connection[client_sock->getFd()] = conn;

    delete client_addr;
}

void Server::deleteConnection(Socket *sock) {
    int sockfd = sock->getFd();
    if (connection.find(sockfd) != connection.end()) {
        Connection *conn = connection[sockfd];
        connection.erase(sockfd);
        delete conn;
        std::cout << "[server] client fd " << sockfd
                  << " closed, memory resources of the connection is deleted" << std::endl;
    }
}