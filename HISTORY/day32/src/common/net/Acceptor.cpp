#include "Acceptor.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include "log/Logger.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/socket.h>

Acceptor::Acceptor(Eventloop *loop, const char *ip, uint16_t port) {
    sock_ = std::make_unique<Socket>();
    if (!sock_->isValid()) {
        LOG_FATAL << "[Acceptor] 监听 socket 创建失败，无法启动服务";
    }

    InetAddress addr(ip, port);
    int opt = 1;
    if (setsockopt(sock_->getFd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        // SO_REUSEADDR 失败通常不影响正确性，仅影响端口快速复用能力。
        LOG_WARN << "[Acceptor] setsockopt(SO_REUSEADDR) 失败，继续启动，错误=" << strerror(errno)
                 << " errno=" << errno;
    }

    if (!sock_->bind(&addr)) {
        LOG_FATAL << "[Acceptor] bind 失败，无法启动监听";
    }
    if (!sock_->listen()) {
        LOG_FATAL << "[Acceptor] listen 失败，无法启动监听";
    }
    if (!sock_->setnonblocking()) {
        LOG_FATAL << "[Acceptor] 设置监听 socket 非阻塞失败";
    }

    acceptChannel_ = std::make_unique<Channel>(loop, sock_->getFd());
    // 只要有新连接请求，Channel 就会调用我们注册的 acceptConnection
    acceptChannel_->setReadCallback(std::bind(&Acceptor::acceptConnection, this));
    // 注意：Acceptor 不使用 ET 模式，避免多个连接同时到达时只 accept 一次导致丢连接
    acceptChannel_->enableReading();
}

Acceptor::~Acceptor() {}

void Acceptor::acceptConnection() {
    InetAddress clientAddr;
    int clientFd = sock_->accept(&clientAddr);
    if (clientFd == -1) {
        // 非阻塞 accept 在高并发下会频繁出现以下“可恢复错误”，不应导致进程退出。
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED ||
            errno == EPROTO)
            return;

        LOG_ERROR << "[Acceptor] accept 失败，错误=" << strerror(errno) << " errno=" << errno;
        return;
    }

    // 直接 fcntl，不通过 Socket 包装（避免析构时 close）
    int oldFlags = fcntl(clientFd, F_GETFL);
    if (oldFlags != -1)
        fcntl(clientFd, F_SETFL, oldFlags | O_NONBLOCK);

    if (newConnectionCallback_)
        newConnectionCallback_(clientFd);
}

void Acceptor::setNewConnectionCallback(std::function<void(int)> cb) {
    newConnectionCallback_ = std::move(cb);
}