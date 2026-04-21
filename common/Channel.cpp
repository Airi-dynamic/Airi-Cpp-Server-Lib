#include "Channel.h"
#include "EventLoop.h"
#include <functional>

Channel::Channel(Eventloop *_loop, int _fd)
    : loop(_loop), fd(_fd), events(0), revents(0), inEpoll(false) {}

Channel::~Channel() {}

void Channel::enableReading() {
    events = POLLER_READ | POLLER_ET;
    loop->updateChannel(this);
}

void Channel::disableWriting() {
    events &= ~POLLER_WRITE;
    loop->updateChannel(this);
}

void Channel::enableWriting() {
    events |= POLLER_WRITE;
    loop->updateChannel(this);
}

bool Channel::isWriting() { return events & POLLER_WRITE; }
int Channel::getFd() { return fd; }

uint32_t Channel::getEvents() { return events; }

uint32_t Channel::getRevents() { return revents; }

bool Channel::getInEpoll() { return inEpoll; }

void Channel::setInEpoll(bool _in) { inEpoll = _in; }

void Channel::setRevents(uint32_t _rev) { revents = _rev; }

void Channel::handleEvent() {
    if (revents & (POLLER_READ | POLLER_PRI)) {
        if (readCallback)
            readCallback();
    }
    if (revents & POLLER_WRITE) {
        if (writeCallback)
            writeCallback();
    }
}

void Channel::setReadCallback(std::function<void()> _cb) { readCallback = _cb; }

void Channel::setWriteCallback(std::function<void()> _cb) { writeCallback = _cb; }
