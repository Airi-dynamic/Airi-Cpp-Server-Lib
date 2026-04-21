#pragma once
#include <cstdint>
#include <sys/types.h>

class Epoll;

class Channel {
private:
  Epoll *ep;
  int fd;
  uint32_t events;
  uint32_t revents;
  bool inEpoll;
public:
  Channel(Epoll *_ep, int _fd);
  ~Channel();

  void enableReading();
  int getFd();
  uint32_t getEvents();
  uint32_t getRevents();

  bool getInEpoll();
  void setInEpoll(bool _in = true);

  void setRevents(uint32_t _rev);
};
