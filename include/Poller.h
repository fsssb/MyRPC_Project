#ifndef MYRPCPROJECT_INCLUDE_POLLER_H_
#define MYRPCPROJECT_INCLUDE_POLLER_H_

#include <memory>
#include <vector>

class Channel;
class EventLoop;

class Poller {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop);
    virtual ~Poller() = default;

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    virtual void poll(int timeoutMs, ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;
    virtual bool hasChannel(Channel* channel) const = 0;

    static std::unique_ptr<Poller> newDefaultPoller(EventLoop* loop);

protected:
    EventLoop* ownerLoop_;
};

#endif  // MYRPCPROJECT_INCLUDE_POLLER_H_
