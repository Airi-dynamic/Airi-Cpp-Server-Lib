// 整个文件仅在 macOS / BSD 上编译，guard 使 clangd 在 Linux 上看到空文件
#ifdef __APPLE__

#include "KqueuePoller.h"
#include "Channel.h"
#include "log/Logger.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/event.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {
constexpr int kInitialEvents = 1024;
} // namespace

KqueuePoller::KqueuePoller(Eventloop *loop) : Poller(loop), kqueueFd_(-1), events_(kInitialEvents) {
    kqueueFd_ = kqueue();
    if (kqueueFd_ == -1) {
        LOG_ERROR << "[KqueuePoller] kqueue 创建失败，错误=" << strerror(errno)
                  << " errno=" << errno;
        throw std::runtime_error("kqueue create failed");
    }
}

KqueuePoller::~KqueuePoller() {
    if (kqueueFd_ != -1) {
        close(kqueueFd_);
        kqueueFd_ = -1;
    }
    // events_ 是 std::vector，析构时自动释放
}

void KqueuePoller::updateChannel(Channel *channel) {
    int fd = channel->getFd();
    int listenEvents = channel->getListenEvents();

    // 若 Channel 此前已在 kqueue 中，先移除旧事件
    // （分开调用，容忍某个 filter 未注册而返回 ENOENT）
    if (channel->getInEpoll()) {
        struct kevent delEv;
        EV_SET(&delEv, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        kevent(kqueueFd_, &delEv, 1, nullptr, 0, nullptr);
        EV_SET(&delEv, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(kqueueFd_, &delEv, 1, nullptr, 0, nullptr);
    }

    // 添加当前需要的事件
    struct kevent ev[2];
    int n = 0;
    if (listenEvents & Channel::READ_EVENT) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, channel);
    }
    if (listenEvents & Channel::WRITE_EVENT) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, channel);
    }
    if (n > 0) {
        if (kevent(kqueueFd_, ev, n, nullptr, 0, nullptr) == -1) {
            LOG_ERROR << "[KqueuePoller] updateChannel 失败，fd=" << fd
                      << " 错误=" << strerror(errno) << " errno=" << errno;
            return;
        }
    }
    channel->setInEpoll(listenEvents != 0);
}

void KqueuePoller::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    // 分开两次调用：kevent 批量操作中若首条 EV_DELETE 失败（ENOENT），
    // 整个调用返回 -1，第二条也不会被执行
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr); // 忽略错误
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr); // 忽略错误
    channel->setInEpoll(false);
}

std::vector<Channel *> KqueuePoller::poll(int timeout) {
    std::vector<Channel *> activeChannels;
    struct timespec ts;
    struct timespec *pts = nullptr;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = static_cast<long>(timeout % 1000) * 1000000;
        pts = &ts;
    }
    int nfds = kevent(kqueueFd_, nullptr, 0, events_.data(), static_cast<int>(events_.size()), pts);
    if (nfds == -1) {
        if (errno != EINTR) {
            LOG_ERROR << "[KqueuePoller] kevent wait 失败，错误=" << strerror(errno)
                      << " errno=" << errno;
        }
        return activeChannels;
    }

    if (nfds == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);
    }

    // 合并同一 Channel 上的多个事件（kqueue 对同一 fd 可能分别返回
    // EVFILT_READ 和 EVFILT_WRITE 两条记录）
    std::unordered_map<Channel *, int> channelEvents;
    for (int i = 0; i < nfds; ++i) {
        Channel *ch = static_cast<Channel *>(events_[i].udata);
        int readyEv = 0;
        if (events_[i].filter == EVFILT_READ)
            readyEv |= Channel::READ_EVENT;
        if (events_[i].filter == EVFILT_WRITE)
            readyEv |= Channel::WRITE_EVENT;
        channelEvents[ch] |= readyEv;
    }
    for (auto &[ch, ev] : channelEvents) {
        ch->setReadyEvents(ev);
        activeChannels.push_back(ch);
    }
    return activeChannels;
}

#endif // __APPLE__