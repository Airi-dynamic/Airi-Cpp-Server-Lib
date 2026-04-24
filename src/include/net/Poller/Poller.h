#pragma once
#include "Macros.h"
#include <memory>
#include <vector>

class Channel;
class Eventloop;

class Poller {
    DISALLOW_COPY_AND_MOVE(Poller)
  protected:
    Eventloop *ownerLoop_;

  public:
    explicit Poller(Eventloop *loop) : ownerLoop_(loop) {}

    virtual ~Poller() = default;

    virtual void updateChannel(Channel *channel) = 0;
    virtual void deleteChannel(Channel *channel) = 0;
    virtual std::vector<Channel *> poll(int timeout = -1) = 0;

    // 静态工厂方法，返回 unique_ptr 以明确所有权
    static std::unique_ptr<Poller> newDefaultPoller(Eventloop *loop);
};