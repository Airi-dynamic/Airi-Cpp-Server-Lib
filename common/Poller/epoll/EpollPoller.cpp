#ifdef __linux__
#include "Poller/EpollPoller.h"
#include "Channel.h"
#include "Poller/Poller.h"
#include "log/Logger.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <unistd.h>
#include <vector>

#include <sys/epoll.h>

namespace {

constexpr int kInitialEvents = 1024;

const char *epollOpName(int op) {
    switch (op) {
    case EPOLL_CTL_ADD:
        return "ADD";
    case EPOLL_CTL_MOD:
        return "MOD";
    case EPOLL_CTL_DEL:
        return "DEL";
    default:
        return "UNKNOWN";
    }
}

} // namespace

EpollPoller::EpollPoller(Eventloop *loop) : Poller(loop), epollFd_(-1), events_(kInitialEvents) {
    epollFd_ = epoll_create1(0);
    if (epollFd_ == -1) {
        LOG_FATAL << "epoll create error, errno=" << errno << " 错误=" << strerror(errno);
    }
}

EpollPoller::~EpollPoller() {
    if (epollFd_ != -1) {
        close(epollFd_);
        epollFd_ = -1;
    }
    // events_ 是 std::vector，析构时自动释放
}

bool EpollPoller::shouldRetryWithMod(int op, int err) {
    return op == EPOLL_CTL_ADD && err == EEXIST;
}

bool EpollPoller::shouldRetryWithAdd(int op, int err) {
    return op == EPOLL_CTL_MOD && err == ENOENT;
}

bool EpollPoller::shouldIgnoreCtlError(int op, int err) {
    return op == EPOLL_CTL_DEL && (err == ENOENT || err == EBADF);
}

void EpollPoller::updateChannel(Channel *channel) {
    int fd = channel->getFd();

    // 未注册且不监听任何事件时，直接返回，避免向 epoll 提交无效 ADD。
    if (!channel->getInEpoll() && channel->getListenEvents() == 0)
        return;

    struct epoll_event ev {};
    ev.data.ptr = channel;

    // 原本的 ev.events = channel->getEvents(); 需要在 channel 类全局引入 linux 上的 sys/epoll.h
    if (channel->getListenEvents() & Channel::READ_EVENT)
        ev.events |= (EPOLLIN | EPOLLPRI);
    if (channel->getListenEvents() & Channel::WRITE_EVENT)
        ev.events |= EPOLLOUT;
    if (channel->getListenEvents() & Channel::ET)
        ev.events |= EPOLLET;

    int op = channel->getInEpoll() ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (channel->getInEpoll() && channel->getListenEvents() == 0)
        op = EPOLL_CTL_DEL;

    auto runCtl = [&](int ctlOp) {
        return epoll_ctl(epollFd_, ctlOp, fd, ctlOp == EPOLL_CTL_DEL ? nullptr : &ev);
    };

    if (runCtl(op) == 0) {
        channel->setInEpoll(op != EPOLL_CTL_DEL);
        return;
    }

    const int firstErr = errno;

    if (shouldRetryWithMod(op, firstErr)) {
        if (runCtl(EPOLL_CTL_MOD) == 0) {
            channel->setInEpoll(true);
            LOG_WARN << "[EpollPoller] ADD 命中 EEXIST，已自动切换 MOD，fd=" << fd;
            return;
        }
        const int retryErr = errno;
        LOG_ERROR << "[EpollPoller] epoll_ctl ADD->MOD 重试失败，fd=" << fd
                  << " 首错=" << strerror(firstErr) << "(" << firstErr << ")"
                  << " 重试错=" << strerror(retryErr) << "(" << retryErr << ")";
        return;
    }

    if (shouldRetryWithAdd(op, firstErr)) {
        if (runCtl(EPOLL_CTL_ADD) == 0) {
            channel->setInEpoll(true);
            LOG_WARN << "[EpollPoller] MOD 命中 ENOENT，已自动切换 ADD，fd=" << fd;
            return;
        }
        const int retryErr = errno;
        LOG_ERROR << "[EpollPoller] epoll_ctl MOD->ADD 重试失败，fd=" << fd
                  << " 首错=" << strerror(firstErr) << "(" << firstErr << ")"
                  << " 重试错=" << strerror(retryErr) << "(" << retryErr << ")";
        return;
    }

    if (shouldIgnoreCtlError(op, firstErr)) {
        channel->setInEpoll(false);
        LOG_WARN << "[EpollPoller] 忽略可恢复错误 op=" << epollOpName(op) << " fd=" << fd
                 << " 错误=" << strerror(firstErr) << "(" << firstErr << ")";
        return;
    }

    LOG_ERROR << "[EpollPoller] epoll_ctl " << epollOpName(op) << " 失败，fd=" << fd
              << " 错误=" << strerror(firstErr) << "(" << firstErr << ")";
    if (op == EPOLL_CTL_DEL)
        channel->setInEpoll(false);
}

void EpollPoller::deleteChannel(Channel *channel) {
    int fd = channel->getFd();

    if (!channel->getInEpoll())
        return;

    if (epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        const int err = errno;
        if (!shouldIgnoreCtlError(EPOLL_CTL_DEL, err)) {
            LOG_ERROR << "[EpollPoller] deleteChannel 失败，fd=" << fd << " 错误=" << strerror(err)
                      << "(" << err << ")";
        } else {
            LOG_WARN << "[EpollPoller] deleteChannel 忽略可恢复错误，fd=" << fd
                     << " 错误=" << strerror(err) << "(" << err << ")";
        }
    }
    channel->setInEpoll(false);
}

std::vector<Channel *> EpollPoller::poll(int timeout) {
    std::vector<Channel *> activeChannels;
    int nfds = epoll_wait(epollFd_, events_.data(), static_cast<int>(events_.size()), timeout);
    if (nfds == -1) {
        if (errno != EINTR) {
            LOG_ERROR << "[EpollPoller] epoll_wait 失败，错误=" << strerror(errno) << "(" << errno
                      << ")";
        }
        return activeChannels;
    }

    // 命中容量上限时扩容，降低高峰期事件截断概率。
    if (nfds == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);
    }

    for (int i = 0; i < nfds; ++i) {
        Channel *ch = static_cast<Channel *>(events_[i].data.ptr);
        int rawEv = events_[i].events;
        // epoll 事件
        int readyEv = 0;
        if (rawEv & (EPOLLERR | EPOLLHUP))
            readyEv |= (Channel::READ_EVENT | Channel::WRITE_EVENT);
        if (rawEv & (EPOLLIN | EPOLLPRI))
            readyEv |= Channel::READ_EVENT;
        if (rawEv & EPOLLOUT)
            readyEv |= Channel::WRITE_EVENT;
        if (rawEv & EPOLLET)
            readyEv |= Channel::ET;
        ch->setReadyEvents(readyEv);
        activeChannels.push_back(ch);
    }
    return activeChannels;
}

#endif