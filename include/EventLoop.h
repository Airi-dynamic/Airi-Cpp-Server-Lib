#pragma once
#include "Channel.h"
#include <functional>
#include <mutex>
#include <sys/types.h>
#include <thread>
#include <vector>
class Epoll;
class Channel;

class Eventloop {
  private:
    Epoll *ep;
    bool quit;

  public:
    Eventloop();
    ~Eventloop();

    void loop();
    void setQuit();
    void updateChannel(Channel *ch);
};