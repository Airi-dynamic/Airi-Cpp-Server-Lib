#include "Acceptor.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include <cstdio>
#include <functional>

Acceptor::Acceptor(Eventloop *_loop)
    : loop(_loop), sock(nullptr), addr(nullptr), acceptChannel(nullptr) {
    sock = new Socket();
    addr = new InetAddress("127.0.0.1", 8888);
    sock->bind(addr);
    sock->listen();
    sock->setnonblocking();

    acceptChannel = new Channel(loop, sock->getFd());

    // 只要有新连接请求，Channel 就会调用我们注册的 acceptConnection
    std::function<void()> cb = std::bind(&Acceptor::acceptConnection, this);
    acceptChannel->setReadCallback(cb);
    acceptChannel->setWriteCallback(nullptr);
    acceptChannel->enableReading();
}

Acceptor::~Acceptor() {
    delete sock;
    delete addr;
    delete acceptChannel;
}

void Acceptor::acceptConnection() {
    InetAddress *client_addr = new InetAddress();
    int client_fd = sock->accept(client_addr);

    if (client_fd == -1) {
        delete client_addr;
        return;
    }

    Socket *client_sock = new Socket(client_fd);
    client_sock->setnonblocking();

    if (newConnectionCallback) {
        newConnectionCallback(client_sock, client_addr);
    } else {
        delete client_addr;
        delete client_sock;
    }
}

void Acceptor::setNewConnectionCallback(std::function<void(Socket *, InetAddress *)> _cb) {
    newConnectionCallback = _cb;
}