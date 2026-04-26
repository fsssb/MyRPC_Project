#include "EpollPoller.h"

#include "Channel.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

constexpr int kMinEventBatch = 32;

}  // namespace

int EpollPoller::epollWaitLoop(int epollFd, struct epoll_event* events, int maxEvents, int timeoutMs) noexcept {
    for (;;) {
        const int n = ::epoll_wait(epollFd, events, maxEvents, timeoutMs);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return n;
        }
        return n;
    }
}

std::uint32_t EpollPoller::channelMaskToEpoll(int channelMask) noexcept {
    // 与 Channel 的 POLLIN / POLLOUT 位一致，并采用边缘触发
    std::uint32_t ev = EPOLLET;
    if (channelMask & POLLIN) {
        ev |= static_cast<std::uint32_t>(EPOLLIN) | static_cast<std::uint32_t>(EPOLLRDHUP);
    }
    if (channelMask & POLLOUT) {
        ev |= static_cast<std::uint32_t>(EPOLLOUT);
    }
    return ev;
}

std::uint32_t EpollPoller::reventsToPollMask(std::uint32_t epollRevents) noexcept {
    int r = 0;
    if (epollRevents & static_cast<std::uint32_t>(EPOLLIN)) {
        r |= POLLIN;
    }
    if (epollRevents & static_cast<std::uint32_t>(EPOLLOUT)) {
        r |= POLLOUT;
    }
    if (epollRevents & static_cast<std::uint32_t>(EPOLLERR)) {
        r |= POLLERR;
    }
    if (epollRevents & static_cast<std::uint32_t>(EPOLLHUP)) {
        r |= POLLHUP;
    }
    if (epollRevents & static_cast<std::uint32_t>(EPOLLRDHUP)) {
        r |= POLLHUP;
    }
    return static_cast<std::uint32_t>(r);
}

EpollPoller::EpollPoller(EventLoop* loop) : Poller(loop) {
    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        throw std::runtime_error(std::string("epoll_create1 failed: ") + std::strerror(errno));
    }
    eventBuffer_.assign(static_cast<std::size_t>(kMinEventBatch), epoll_event{});
}

EpollPoller::~EpollPoller() {
    if (epollFd_ >= 0) {
        (void)::close(epollFd_);
        epollFd_ = -1;
    }
}

void EpollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    if (epollFd_ < 0) {
        return;
    }

    int n = epollWaitLoop(
        epollFd_,
        eventBuffer_.data(),
        static_cast<int>(eventBuffer_.size()),
        timeoutMs);
    if (n <= 0) {
        return;
    }

    if (n == static_cast<int>(eventBuffer_.size())) {
        const int newSize = static_cast<int>(std::max<std::size_t>(eventBuffer_.size() * 2, static_cast<std::size_t>(kMinEventBatch)));
        eventBuffer_.resize(static_cast<std::size_t>(newSize), epoll_event{});
    }

    for (int i = 0; i < n; ++i) {
        const struct epoll_event& ee = eventBuffer_[static_cast<std::size_t>(i)];
        const int fd = ee.data.fd;
        const std::uint32_t asPoll = reventsToPollMask(ee.events);
        const auto it = channels_.find(fd);
        if (it == channels_.end() || it->second == nullptr) {
            continue;
        }
        Channel* channel = it->second;
        channel->setRevents(static_cast<short>(asPoll));
        activeChannels->push_back(channel);
    }
}

void EpollPoller::updateChannel(Channel* channel) {
    if (channel == nullptr) {
        return;
    }
    const int fd = channel->fd();
    if (fd < 0) {
        return;
    }

    if (channel->isNoneEvent()) {
        if (hasChannel(channel)) {
            (void)::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
            channels_.erase(fd);
        }
        return;
    }

    const int chMask = static_cast<int>(static_cast<unsigned short>(channel->events()));
    struct epoll_event event {};
    event.events = channelMaskToEpoll(chMask);
    event.data.fd = fd;

    if (!hasChannel(channel)) {
        if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &event) < 0) {
            return;
        }
        channels_[fd] = channel;
        return;
    }

    (void)::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &event);
}

void EpollPoller::removeChannel(Channel* channel) {
    if (channel == nullptr) {
        return;
    }
    const int fd = channel->fd();
    if (fd < 0) {
        return;
    }
    if (!hasChannel(channel)) {
        return;
    }
    (void)::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    channels_.erase(fd);
}

bool EpollPoller::hasChannel(Channel* channel) const {
    if (channel == nullptr) {
        return false;
    }
    const auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}
