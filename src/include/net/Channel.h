#pragma once
#include "Macros.h"
#include <functional>

class Eventloop;

class Channel {
    DISALLOW_COPY_AND_MOVE(Channel)
  public:
    static const int READ_EVENT;  // = 1
    static const int WRITE_EVENT; // = 2
    static const int ET;          // = 4
  private:
    Eventloop *loop_;
    int fd_;
    int listen_events_{0};
    int ready_events_{0};
    bool inEpoll_{false}; // 标记当前 Channel 是否已经在 Epoll 树上
    std::function<void()> readCallback;
    std::function<void()> writeCallback;

  public:
    Channel(Eventloop *_ep, int _fd);
    ~Channel();

    void handleEvent(); // 处理事件

    void enableET();
    void disableET();

    void enableReading();
    void disableReading();

    void enableWriting();
    void disableWriting();

    void disableAll();
    bool isWriting();

    int getFd();
    int getListenEvents(); // 原 getEvents()，返回我们的平台中立标志
    int getReadyEvents();  // 原 getRevents()
    bool getInEpoll();
    void setInEpoll(bool _in = true);
    void setReadyEvents(int ev); // 原 setRevents()，由 Poller 调用

    void setReadCallback(std::function<void()> _cb);
    void setWriteCallback(std::function<void()> _cb);
};