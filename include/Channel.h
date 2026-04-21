#pragma once
#include <cstdint>
#include <functional>

class Epoll;
class Eventloop;

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

#ifdef __APPLE__
constexpr uint32_t POLLER_READ = 1;
constexpr uint32_t POLLER_WRITE = 4;
constexpr uint32_t POLLER_ET = 2;
constexpr uint32_t POLLER_PRI = 8;
#else
constexpr uint32_t POLLER_READ = EPOLLIN;
constexpr uint32_t POLLER_WRITE = EPOLLOUT;
constexpr uint32_t POLLER_ET = EPOLLET;
constexpr uint32_t POLLER_PRI = EPOLLPRI;
#endif

class Channel {
  private:
    Eventloop *loop;
    int fd;
    uint32_t events;
    uint32_t revents;
    bool inEpoll;
    std::function<void()> readCallback;
    std::function<void()> writeCallback;

  public:
    Channel(Eventloop *_ep, int _fd);
    ~Channel();

    void handleEvent();

    void enableET();
    void disableET();

    void enableReading();
    void disableReading();

    void enableWriting();
    void disableWriting();

    void disableAll();
    bool isWriting();

    int getFd();
    uint32_t getEvents();
    uint32_t getRevents();
    bool getInEpoll();
    void setInEpoll(bool _in = true);
    void setRevents(uint32_t _rev);

    void setReadCallback(std::function<void()> _cb);
    void setWriteCallback(std::function<void()> _cb);
};