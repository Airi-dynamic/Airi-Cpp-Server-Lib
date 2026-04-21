#include "Connection.h"
#include "Buffer.h"
#include "Channel.h"
#include "Socket.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <sys/types.h>
#include <unistd.h>

#define READ_BUFFER 1024

Connection::Connection(Eventloop *_loop, Socket *_sock)
    : loop(_loop), sock(_sock), channel(nullptr) {
    channel = new Channel(loop, sock->getFd());
    channel->setReadCallback(std::bind(&Connection::handleRead, this));
    channel->setWriteCallback(std::bind(&Connection::handleWrite, this));
    channel->enableReading();
}

Connection::~Connection() {
    delete channel;
    delete sock;
}

void Connection::handleRead() {
    int sockfd = sock->getFd();
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);

    if (n > 0) {
        // 读到了数据，业务逻辑处理
        // 现在的业务是 Echo，所以直接把 buf 里的拿出来发回去
        // 实际应该是: onMessageCallback(this, &inputBuffer_);

        std::string msg = inputBuffer_.retrieveAllAsString();
        std::cout << "[server] message from client fd " << sockfd << ": " << msg << std::endl;
        // 这一行就是业务逻辑，要更复杂的业务逻辑就换掉这里
        send(msg);
    } else if (n == 0) {
        std::cout << "[server] client fd " << sockfd << " disconnected." << std::endl;
        // 关键：连接断开时，通过回调通知 Server 移除自己
        if (deleteConnectionCallback)
            deleteConnectionCallback(sock);
    } else {
        std::cerr << "[server] read error on fd " << sockfd << ": " << strerror(savedErrno)
                  << std::endl;
        if (deleteConnectionCallback)
            deleteConnectionCallback(sock);
    }
}

void Connection::handleWrite() {
    if (channel->isWriting()) {
        ssize_t n = ::write(sock->getFd(), outputBuffer_.peek(), outputBuffer_.readableBytes());
        if (n > 0) {
            outputBuffer_.retrieve(n);
            if (outputBuffer_.readableBytes() == 0) {
                channel->disableWriting();
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
    if (!channel->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(sock->getFd(), msg.data(), msg.size());
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
        if (!channel->isWriting()) {
            channel->enableWriting();
        }
    }
}

void Connection::setDeleteConnectionCallback(std::function<void(Socket *)> _cb) {
    deleteConnectionCallback = _cb;
}