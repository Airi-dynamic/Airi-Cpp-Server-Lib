#include "Server.h"
#include "EventLoop.h"
#include <atomic>
#include <csignal>
#include <iostream>

#define READ_BUFFER 1024

Eventloop *g_loop = nullptr;

void signalHandler(int signum) {
    std::cout << "[server] Caught signal " << signum << " , shutting down server." << std::endl;
    if (g_loop) {
        g_loop->setQuit();
    }
}

int main() {
    signal(SIGINT, signalHandler);
    Eventloop *loop = new Eventloop();
    g_loop = loop;
    std::cout << "[server] Main Loop Created" << std::endl;
    Server *server = new Server(loop);
    std::cout << "[server] Server Created" << std::endl;

    loop->loop();

    delete server;
    delete loop;
    return 0;
}