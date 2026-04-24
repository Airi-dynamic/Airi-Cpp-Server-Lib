#include "Poller/Poller.h"
#include <memory>

#ifdef __linux__
#include "Poller/EpollPoller.h"
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include "Poller/KqueuePoller.h"
#else
#error "OS not supported!"
#endif

std::unique_ptr<Poller> Poller::newDefaultPoller(Eventloop *loop) {
#ifdef __linux__
    return std::make_unique<EpollPoller>(loop);
#else
    return std::make_unique<KqueuePoller>(loop);
#endif
}