#include "PollPoller.h"

#include "Channel.h"

#include <algorithm>

PollPoller::PollPoller(EventLoop* loop) : Poller(loop) {}

void PollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    const int eventCount = ::poll(pollFds_.data(), static_cast<nfds_t>(pollFds_.size()), timeoutMs);
    if (eventCount <= 0) {
        return;
    }

    int remaining = eventCount;
    for (auto& pollFd : pollFds_) {
        if (pollFd.revents > 0) {
            const auto it = channels_.find(pollFd.fd);
            if (it != channels_.end()) {
                Channel* channel = it->second;
                channel->setRevents(pollFd.revents);
                activeChannels->push_back(channel);
            }
            --remaining;
            if (remaining == 0) {
                break;
            }
        }
    }
}

void PollPoller::updateChannel(Channel* channel) {
    const int fd = channel->fd();
    if (!hasChannel(channel)) {
        struct pollfd pollFd {};
        pollFd.fd = fd;
        pollFd.events = channel->events();
        pollFd.revents = 0;
        pollFds_.push_back(pollFd);
        channels_[fd] = channel;
        return;
    }

    for (auto& pollFd : pollFds_) {
        if (pollFd.fd == fd) {
            pollFd.events = channel->events();
            pollFd.revents = 0;
            break;
        }
    }
}

void PollPoller::removeChannel(Channel* channel) {
    const int fd = channel->fd();
    channels_.erase(fd);

    pollFds_.erase(
        std::remove_if(pollFds_.begin(), pollFds_.end(), [fd](const struct pollfd& pollFd) {
            return pollFd.fd == fd;
        }),
        pollFds_.end());
}

bool PollPoller::hasChannel(Channel* channel) const {
    const auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}
