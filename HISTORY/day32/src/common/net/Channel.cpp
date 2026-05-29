#include "Channel.h"
#include "EventLoop.h"
#include <functional>

// 使用我们自定义的等价标志位
const int Channel::READ_EVENT = 1;
const int Channel::WRITE_EVENT = 2;
const int Channel::ET = 4;

Channel::Channel(Eventloop *_loop, int _fd)
    : loop_(_loop), fd_(_fd), listen_events_(0), ready_events_(0), inEpoll_(false) {}

Channel::~Channel() {}

void Channel::enableReading() {
    listen_events_ |= READ_EVENT;
    loop_->updateChannel(this);
}

void Channel::disableReading() {
    listen_events_ &= ~READ_EVENT;
    loop_->updateChannel(this);
}

void Channel::enableET() {
    listen_events_ |= ET;
    loop_->updateChannel(this);
}

void Channel::disableET() { listen_events_ &= ~ET; }

void Channel::enableWriting() {
    listen_events_ |= WRITE_EVENT;
    loop_->updateChannel(this);
}

void Channel::disableWriting() {
    listen_events_ &= ~WRITE_EVENT;
    loop_->updateChannel(this);
}

void Channel::disableAll() {
    listen_events_ = 0;
    loop_->updateChannel(this);
}

bool Channel::isWriting() { return listen_events_ & WRITE_EVENT; }
int Channel::getFd() { return fd_; }
int Channel::getListenEvents() { return listen_events_; }
int Channel::getReadyEvents() { return ready_events_; }
bool Channel::getInEpoll() { return inEpoll_; }
void Channel::setInEpoll(bool _in) { inEpoll_ = _in; }
void Channel::setReadyEvents(int ev) { ready_events_ = ev; }

void Channel::handleEvent() {
    if (ready_events_ & READ_EVENT) {
        if (readCallback)
            readCallback();
    }
    if (ready_events_ & WRITE_EVENT) {
        if (writeCallback)
            writeCallback();
    }
}

void Channel::setReadCallback(std::function<void()> _cb) { readCallback = _cb; }
void Channel::setWriteCallback(std::function<void()> _cb) { writeCallback = _cb; }