#ifndef MYRPCPROJECT_INCLUDE_EPOLLPOLLER_H_
#define MYRPCPROJECT_INCLUDE_EPOLLPOLLER_H_

#include "Poller.h"

#include <cstdint>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>

class EpollPoller : public Poller {
public:
    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller() override;

    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    void poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;
    bool hasChannel(Channel* channel) const override;

private:
    static int epollWaitLoop(int epollFd, struct epoll_event* events, int maxEvents, int timeoutMs) noexcept;
    static std::uint32_t channelMaskToEpoll(int channelMask) noexcept;
    static std::uint32_t reventsToPollMask(std::uint32_t epollRevents) noexcept;

private:
    int epollFd_{-1};
    std::vector<struct epoll_event> eventBuffer_;
    std::unordered_map<int, Channel*> channels_;
};

#endif  // MYRPCPROJECT_INCLUDE_EPOLLPOLLER_H_
