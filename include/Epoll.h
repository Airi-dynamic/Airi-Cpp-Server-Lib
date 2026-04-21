#pragma once
#include "Channel.h"
#include <cstdint>
#include <vector>

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

// 平台标志常量
#ifdef __APPLE__
constexpr uint32_t POLLER_READ = 1;
constexpr uint32_t POLLER_ET   = 2;
#else
constexpr uint32_t POLLER_READ = EPOLLIN;
constexpr uint32_t POLLER_ET   = EPOLLET;
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
