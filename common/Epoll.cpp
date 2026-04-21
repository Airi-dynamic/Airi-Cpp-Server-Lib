#include "Epoll.h"
#include "Channel.h"
#include "util.h"
#include <cstring>
#include <unistd.h>

#define MAX_EVENTS 1024

#ifdef __APPLE__

Epoll::Epoll() : epfd(-1), events(nullptr) {
  epfd = kqueue();
  errif(epfd == -1, "kqueue create error");
  events = new struct kevent[MAX_EVENTS];
  memset(events, 0, sizeof(struct kevent) * MAX_EVENTS);
}

Epoll::~Epoll() {
  if (epfd != -1) { close(epfd); epfd = -1; }
  delete[] events;
}

void Epoll::updateChannel(Channel *channel) {
  int fd = channel->getFd();
  struct kevent changes[2];
  int nchanges = 0;

  uint32_t ev = channel->getEvents();

  if (ev & POLLER_READ) {
    EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void *)channel);
  }
  if (ev & POLLER_WRITE) {
    EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, (void *)channel);
  } else {
    EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  }

  for (int i = 0; i < nchanges; ++i) {
    kevent(epfd, &changes[i], 1, nullptr, 0, nullptr);
  }
  channel->setInEpoll();
}

std::vector<Channel *> Epoll::poll(int timeout) {
  struct timespec ts;
  struct timespec *tsp = nullptr;
  if (timeout >= 0) {
    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (timeout % 1000) * 1000000L;
    tsp = &ts;
  }
  int nfds = kevent(epfd, nullptr, 0, events, MAX_EVENTS, tsp);
  errif(nfds == -1, "kevent wait error");

  std::vector<Channel *> activeChannels;
  for (int i = 0; i < nfds; ++i) {
    Channel *ch = (Channel *)events[i].udata;
    uint32_t revt = 0;
    if (events[i].filter == EVFILT_READ)  revt |= POLLER_READ;
    if (events[i].filter == EVFILT_WRITE) revt |= POLLER_WRITE;
    ch->setRevents(ch->getRevents() | revt);
    bool found = false;
    for (auto *c : activeChannels) { if (c == ch) { found = true; break; } }
    if (!found) activeChannels.push_back(ch);
  }
  return activeChannels;
}

#else

Epoll::Epoll() : epfd(-1), events(nullptr) {
  epfd = epoll_create1(0);
  errif(epfd == -1, "epoll create error");
  events = new epoll_event[MAX_EVENTS];
  bzero(events, sizeof(epoll_event) * MAX_EVENTS);
}

Epoll::~Epoll() {
  if (epfd != -1) { close(epfd); epfd = -1; }
  delete[] events;
}

void Epoll::updateChannel(Channel *channel) {
  int fd = channel->getFd();
  struct epoll_event ev;
  ev.data.ptr = channel;
  ev.events = channel->getEvents();

  if (!channel->getInEpoll()) {
    errif(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1, "epoll add error");
    channel->setInEpoll();
  } else {
    errif(epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev), "epoll modify error");
  }
}

std::vector<Channel *> Epoll::poll(int timeout) {
  std::vector<Channel *> activeChannels;
  int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
  errif(nfds == -1, "epoll wait error");

  for (int i = 0; i < nfds; ++i) {
    Channel *ch = (Channel *)events[i].data.ptr;
    ch->setRevents(events[i].events);
    activeChannels.push_back(ch);
  }
  return activeChannels;
}

#endif
