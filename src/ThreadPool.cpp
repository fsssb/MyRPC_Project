#include "ThreadPool.h"

#include "EventLoop.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

ThreadPool::ThreadPool(EventLoop* baseLoop, int numThreads) : baseLoop_(baseLoop), numThreads_(numThreads) {
    if (numThreads_ < 0) {
        throw std::invalid_argument("numThreads must be >= 0");
    }
    if (baseLoop_ == nullptr) {
        throw std::invalid_argument("baseLoop must not be null");
    }
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::start() {
    if (started_.exchange(true)) {
        return;
    }

    if (numThreads_ == 0) {
        return;
    }

    loops_.assign(static_cast<std::size_t>(numThreads_), nullptr);
    threads_.reserve(static_cast<std::size_t>(numThreads_));
    for (int i = 0; i < numThreads_; ++i) {
        threads_.emplace_back([this, i]() { threadFunc(i); });
    }

    while (readyCount_.load(std::memory_order_acquire) < numThreads_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ThreadPool::stop() {
    if (!started_.exchange(false)) {
        return;
    }

    for (EventLoop* loop : loops_) {
        if (loop != nullptr) {
            loop->runInLoop([loop]() { loop->quit(); });
        }
    }

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    loops_.clear();
    threads_.clear();
    next_.store(0, std::memory_order_release);
    readyCount_.store(0, std::memory_order_release);
}

EventLoop* ThreadPool::getNextLoop() {
    if (numThreads_ == 0) {
        return baseLoop_;
    }

    if (loops_.empty()) {
        return baseLoop_;
    }

    const std::size_t index = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    EventLoop* loop = loops_[index];
    return loop;
}

void ThreadPool::submit(Task task) {
    if (!task) {
        return;
    }
    // 经主循环投递，与 Muduo/常见 Reactor 模式一致，避免 MPSC 无锁队列实现错误导致任务丢失。
    baseLoop_->queueInLoop([t = std::move(task)]() mutable {
        if (t) {
            t();
        }
    });
}

std::size_t ThreadPool::pendingTaskSize() const { return 0; }

void ThreadPool::threadFunc(int index) {
    auto loop = std::make_unique<EventLoop>();
    loops_[static_cast<std::size_t>(index)] = loop.get();
    readyCount_.fetch_add(1, std::memory_order_release);

    loop->loop();
}
