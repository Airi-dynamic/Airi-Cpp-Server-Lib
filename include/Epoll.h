#pragma once
#include "Channel.h"
#include <cstdint>
#include <vector>

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

class Epoll {
private:
  int epfd;
#ifdef __APPLE__
  struct kevent *events;
#else
  struct epoll_event *events;
#endif

public:
  Epoll();
  ~Epoll();
  void updateChannel(Channel *channel);
  std::vector<Channel *> poll(int timeout = -1);
};
