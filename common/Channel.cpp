#include "Channel.h"
#include "Epoll.h"

Channel::Channel(Epoll *_ep, int _fd)
    : ep(_ep), fd(_fd), events(0), revents(0), inEpoll(false) {}

Channel::~Channel() {}

void Channel::enableReading() {
  events = POLLER_READ | POLLER_ET;
  ep->updateChannel(this);
}
int Channel::getFd() { return fd; }

uint32_t Channel::getEvents() { return events; }

uint32_t Channel::getRevents() { return revents; }

bool Channel::getInEpoll() { return inEpoll; }

void Channel::setInEpoll(bool _in) { inEpoll = _in; }

void Channel::setRevents(uint32_t _rev) { revents = _rev; }
