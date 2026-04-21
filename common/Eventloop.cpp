#include "EventLoop.h"
#include "Channel.h"
#include "Epoll.h"
#include <unistd.h>
#include <vector>

Eventloop::Eventloop() : ep(nullptr), quit(false) {
    ep = new Epoll(); // 这个是我们自定义的epoll，不是OS给的
}

Eventloop::~Eventloop() { delete ep; }

void Eventloop::loop() {
    while (!quit) {
        std::vector<Channel *> channels;
        channels = ep->poll(); // 这里会返回
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            (*it)->handleEvent();
        }
    }
}

void Eventloop::updateChannel(Channel *ch) {
    ep->updateChannel(ch);
    // 以后要更新channel走eventloop中转
}

void Eventloop::setQuit() { this->quit = true; }