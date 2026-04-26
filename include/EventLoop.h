#ifndef MYRPCPROJECT_INCLUDE_EVENTLOOP_H_
#define MYRPCPROJECT_INCLUDE_EVENTLOOP_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Channel;
class Poller;

class EventLoop {
public:
    using Functor = std::function<void()>;
    using TimerId = uint64_t;

    EventLoop();
    virtual ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();
    void quit();

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);
    TimerId runAfter(std::chrono::milliseconds delay, Functor cb);
    TimerId runEvery(std::chrono::milliseconds interval, Functor cb);
    void cancelTimer(TimerId timerId);

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel) const;

    bool isInLoopThread() const;

private:
    void wakeup();
    void handleWakeupRead();
    void doPendingFunctors();
    void runTimers();
    int getPollTimeoutMs() const;

private:
    struct TimerTask {
        TimerId id {0};
        std::chrono::steady_clock::time_point expiresAt;
        std::chrono::milliseconds interval {0};
        Functor cb;
        bool repeat {false};
    };

    std::atomic<bool> looping_{false};
    std::atomic<bool> quit_{false};
    const std::thread::id threadId_;

    std::unique_ptr<Poller> poller_;
    std::vector<Channel*> activeChannels_;

    int wakeupReadFd_{-1};
    int wakeupWriteFd_{-1};
    std::shared_ptr<Channel> wakeupChannel_;

    mutable std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;
    bool callingPendingFunctors_{false};

    TimerId nextTimerId_{1};
    std::map<TimerId, TimerTask> timers_;
};

#endif  // MYRPCPROJECT_INCLUDE_EVENTLOOP_H_
