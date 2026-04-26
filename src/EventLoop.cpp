#include "EventLoop.h"

#include "Channel.h"
#include "Logger.h"
#include "Poller.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <future>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr int kDefaultPollTimeoutMs = 10000;
}

EventLoop::EventLoop() : threadId_(std::this_thread::get_id()), poller_(Poller::newDefaultPoller(this)) {
    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        throw std::runtime_error("failed to create wakeup pipe");
    }

    wakeupReadFd_ = pipeFds[0];
    wakeupWriteFd_ = pipeFds[1];
#if defined(__linux__)
    for (int fd : pipeFds) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
#endif

    wakeupChannel_ = std::make_shared<Channel>(this, wakeupReadFd_);
    wakeupChannel_->setReadCallback([this]() {
        handleWakeupRead();
    });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    if (wakeupChannel_) {
        wakeupChannel_->disableAll();
        removeChannel(wakeupChannel_.get());
    }

    if (wakeupReadFd_ >= 0) {
        ::close(wakeupReadFd_);
        wakeupReadFd_ = -1;
    }
    if (wakeupWriteFd_ >= 0) {
        ::close(wakeupWriteFd_);
        wakeupWriteFd_ = -1;
    }
}

void EventLoop::loop() {
    looping_.store(true);
    quit_.store(false);

    while (!quit_.load()) {
        activeChannels_.clear();
        poller_->poll(getPollTimeoutMs(), &activeChannels_);

        for (Channel* channel : activeChannels_) {
            channel->handleEvent();
        }

        doPendingFunctors();
        runTimers();
    }

    looping_.store(false);
}

void EventLoop::quit() {
    quit_.store(true);
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();
        return;
    }
    queueInLoop(std::move(cb));
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }

    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

EventLoop::TimerId EventLoop::runAfter(std::chrono::milliseconds delay, Functor cb) {
    if (isInLoopThread()) {
        TimerId id = nextTimerId_++;
        TimerTask task;
        task.id = id;
        task.expiresAt = std::chrono::steady_clock::now() + delay;
        task.interval = delay;
        task.cb = std::move(cb);
        task.repeat = false;
        timers_[id] = std::move(task);
        return id;
    }

    auto promiseId = std::make_shared<std::promise<TimerId>>();
    auto future = promiseId->get_future();
    queueInLoop([this, delay, cb = std::move(cb), promiseId]() mutable {
        TimerId id = nextTimerId_++;
        TimerTask task;
        task.id = id;
        task.expiresAt = std::chrono::steady_clock::now() + delay;
        task.interval = delay;
        task.cb = std::move(cb);
        task.repeat = false;
        timers_[id] = std::move(task);
        promiseId->set_value(id);
    });
    return future.get();
}

EventLoop::TimerId EventLoop::runEvery(std::chrono::milliseconds interval, Functor cb) {
    if (isInLoopThread()) {
        TimerId id = nextTimerId_++;
        TimerTask task;
        task.id = id;
        task.expiresAt = std::chrono::steady_clock::now() + interval;
        task.interval = interval;
        task.cb = std::move(cb);
        task.repeat = true;
        timers_[id] = std::move(task);
        return id;
    }

    auto promiseId = std::make_shared<std::promise<TimerId>>();
    auto future = promiseId->get_future();
    queueInLoop([this, interval, cb = std::move(cb), promiseId]() mutable {
        TimerId id = nextTimerId_++;
        TimerTask task;
        task.id = id;
        task.expiresAt = std::chrono::steady_clock::now() + interval;
        task.interval = interval;
        task.cb = std::move(cb);
        task.repeat = true;
        timers_[id] = std::move(task);
        promiseId->set_value(id);
    });
    return future.get();
}

void EventLoop::cancelTimer(TimerId timerId) {
    runInLoop([this, timerId]() {
        timers_.erase(timerId);
    });
}

void EventLoop::updateChannel(Channel* channel) {
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel) const {
    return poller_->hasChannel(channel);
}

bool EventLoop::isInLoopThread() const {
    return threadId_ == std::this_thread::get_id();
}

void EventLoop::wakeup() {
    const uint8_t one = 1;
    const ssize_t n = ::write(wakeupWriteFd_, &one, sizeof(one));
    if (n < 0) {
        LOG_ERROR("EventLoop wakeup write failed");
    }
}

void EventLoop::handleWakeupRead() {
    uint8_t buf[64];
#if defined(__linux__)
    for (;;) {
        const ssize_t n = ::read(wakeupReadFd_, buf, sizeof(buf));
        if (n > 0) {
            continue;
        }
        if (n == 0) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        LOG_ERROR("EventLoop wakeup read failed");
        return;
    }
#else
    const ssize_t n = ::read(wakeupReadFd_, buf, sizeof(buf));
    if (n < 0) {
        LOG_ERROR("EventLoop wakeup read failed");
    }
#endif
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (const auto& functor : functors) {
        functor();
    }
    callingPendingFunctors_ = false;
}

void EventLoop::runTimers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<TimerTask> expired;

    for (const auto& timerEntry : timers_) {
        if (timerEntry.second.expiresAt <= now) {
            expired.push_back(timerEntry.second);
        }
    }

    for (const auto& task : expired) {
        const auto it = timers_.find(task.id);
        if (it == timers_.end()) {
            continue;
        }

        if (it->second.cb) {
            it->second.cb();
        }

        if (it->second.repeat) {
            it->second.expiresAt = std::chrono::steady_clock::now() + it->second.interval;
        } else {
            timers_.erase(it);
        }
    }
}

int EventLoop::getPollTimeoutMs() const {
    if (timers_.empty()) {
        return kDefaultPollTimeoutMs;
    }

    auto nearest = timers_.begin()->second.expiresAt;
    for (const auto& timerEntry : timers_) {
        if (timerEntry.second.expiresAt < nearest) {
            nearest = timerEntry.second.expiresAt;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (nearest <= now) {
        return 0;
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(nearest - now).count();
    return static_cast<int>(ms > kDefaultPollTimeoutMs ? kDefaultPollTimeoutMs : ms);
}
