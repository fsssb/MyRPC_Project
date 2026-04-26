#ifndef MYRPCPROJECT_INCLUDE_THREADPOOL_H_
#define MYRPCPROJECT_INCLUDE_THREADPOOL_H_

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

class EventLoop;

class ThreadPool {
public:
    using Task = std::function<void()>;

    ThreadPool(EventLoop* baseLoop, int numThreads);
    virtual ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void start();
    void stop();
    EventLoop* getNextLoop();
    void submit(Task task);
    std::size_t pendingTaskSize() const;

private:
    void threadFunc(int index);

private:
    EventLoop* baseLoop_;
    const int numThreads_;
    std::atomic<bool> started_{false};
    std::atomic<std::size_t> next_{0};
    std::atomic<int> readyCount_{0};

    std::vector<std::thread> threads_;
    std::vector<EventLoop*> loops_;
};

#endif  // MYRPCPROJECT_INCLUDE_THREADPOOL_H_
