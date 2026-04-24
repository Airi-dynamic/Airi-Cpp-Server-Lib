#pragma once
#ifdef __linux__

#include "Poller.h"
#include <sys/epoll.h>
#include <vector>

class EpollPoller : public Poller {
  private:
    int epollFd_;
    std::vector<struct epoll_event> events_;

  public:
    explicit EpollPoller(Eventloop *loop);
    ~EpollPoller() override; // 释放 events_

    // 纯策略函数：用于判断 epoll_ctl 失败后的重试/忽略策略，便于单元测试。
    static bool shouldRetryWithMod(int op, int err);
    static bool shouldRetryWithAdd(int op, int err);
    static bool shouldIgnoreCtlError(int op, int err);

    // 加上 override 关键字
    void updateChannel(Channel *channel) override;
    void deleteChannel(Channel *channel) override;
    std::vector<Channel *> poll(int timeout = -1) override;
};

#endif