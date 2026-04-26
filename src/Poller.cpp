#include "Poller.h"

#if defined(__linux__)
#include "EpollPoller.h"
#elif defined(__APPLE__)
#include "PollPoller.h"
#else
#include "PollPoller.h"
#endif

Poller::Poller(EventLoop* loop) : ownerLoop_(loop) {}

std::unique_ptr<Poller> Poller::newDefaultPoller(EventLoop* loop) {
#if defined(__linux__)
    return std::make_unique<EpollPoller>(loop);
#else
    return std::make_unique<PollPoller>(loop);
#endif
}
