#include "Socket.h"
#include "InetAddress.h"
#include "log/Logger.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

Socket::Socket() : fd_(-1) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ == -1) {
        LOG_ERROR << "[Socket] socket 创建失败，错误=" << strerror(errno) << " errno=" << errno;
    }
}

Socket::Socket(int _fd) : fd_(_fd) {
    if (fd_ == -1) {
        LOG_ERROR << "[Socket] 使用无效 fd 初始化，错误=" << strerror(EBADF) << " errno=" << EBADF;
    }
}

Socket::~Socket() {
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
}

bool Socket::bind(InetAddress *addr) {
    if (fd_ == -1) {
        errno = EBADF;
        LOG_ERROR << "[Socket] bind 失败：fd 无效";
        return false;
    }
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr->addr), addr->addr_len) == -1) {
        LOG_ERROR << "[Socket] bind 失败，错误=" << strerror(errno) << " errno=" << errno;
        return false;
    }
    return true;
}

bool Socket::listen() {
    if (fd_ == -1) {
        errno = EBADF;
        LOG_ERROR << "[Socket] listen 失败：fd 无效";
        return false;
    }
    if (::listen(fd_, SOMAXCONN) == -1) {
        LOG_ERROR << "[Socket] listen 失败，错误=" << strerror(errno) << " errno=" << errno;
        return false;
    }
    return true;
}

int Socket::accept(InetAddress *addr) {
    if (fd_ == -1) {
        errno = EBADF;
        return -1;
    }
    int client_sockfd = ::accept(fd_, reinterpret_cast<sockaddr *>(&addr->addr), &addr->addr_len);
    return client_sockfd;
}

bool Socket::connect(InetAddress *addr) {
    if (fd_ == -1) {
        errno = EBADF;
        LOG_ERROR << "[Socket] connect 失败：fd 无效";
        return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr->addr), addr->addr_len) == -1) {
        LOG_ERROR << "[Socket] connect 失败，错误=" << strerror(errno) << " errno=" << errno;
        return false;
    }
    return true;
}

int Socket::getFd() { return fd_; }

bool Socket::setnonblocking() {
    if (fd_ == -1) {
        errno = EBADF;
        LOG_ERROR << "[Socket] 设置非阻塞失败：fd 无效";
        return false;
    }
    int oldoptions = fcntl(fd_, F_GETFL);
    if (oldoptions == -1) {
        LOG_ERROR << "[Socket] 获取 fd flags 失败，错误=" << strerror(errno) << " errno=" << errno;
        return false;
    }
    int new_option = oldoptions | O_NONBLOCK;
    if (fcntl(fd_, F_SETFL, new_option) == -1) {
        LOG_ERROR << "[Socket] 设置非阻塞失败，错误=" << strerror(errno) << " errno=" << errno;
        return false;
    }
    return true;
}

bool Socket::isNonBlocking() {
    if (fd_ == -1)
        return false;
    int flags = fcntl(fd_, F_GETFL);
    if (flags == -1)
        return false;
    return (flags & O_NONBLOCK) != 0;
}