#ifndef MYRPCPROJECT_INCLUDE_POLLPOLLER_H_
#define MYRPCPROJECT_INCLUDE_POLLPOLLER_H_

#include "Poller.h"

#include <poll.h>
#include <unordered_map>
#include <vector>

class PollPoller : public Poller {
public:
    explicit PollPoller(EventLoop* loop);
    ~PollPoller() override = default;

    void poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;
    bool hasChannel(Channel* channel) const override;

private:
    std::vector<struct pollfd> pollFds_;
    std::unordered_map<int, Channel*> channels_;
};

#endif  // MYRPCPROJECT_INCLUDE_POLLPOLLER_H_
