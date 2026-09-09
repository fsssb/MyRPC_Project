// ucontext is deprecated on macOS but functional; suppress the deprecation
// warnings (no portable zero-dependency alternative without hand-written
// assembly). _XOPEN_SOURCE must be visible before any header exposes
// getcontext/makecontext/swapcontext on macOS.
#if defined(__APPLE__)
#define _XOPEN_SOURCE 700
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "Scheduler.h"

#include <algorithm>

namespace {
constexpr std::size_t kCoroutineStackSize = 64 * 1024;
}

thread_local Scheduler::Coroutine* Scheduler::tlsCurrentCoro_ = nullptr;
thread_local ucontext_t* Scheduler::tlsWorkerMain_ = nullptr;
thread_local Scheduler* Scheduler::tlsScheduler_ = nullptr;

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start(std::size_t workerCount) {
    if (running_.exchange(true)) {
        return;
    }
    const std::size_t n = workerCount > 0 ? workerCount : 1;
    workerMainCtx_.resize(n);
    timerThread_ = std::thread([this]() { timerLoop(); });
    workers_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this, i]() { workerLoop(i); });
    }
}

void Scheduler::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(readyMutex_);
        readyCv_.notify_all();
    }
    sleepCv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    if (timerThread_.joinable()) {
        timerThread_.join();
    }
    // Reap coroutines that were still queued or sleeping.
    for (auto* coro : readyQueue_) {
        delete coro;
    }
    readyQueue_.clear();
    {
        std::lock_guard<std::mutex> lock(sleepMutex_);
        for (const auto& entry : sleepHeap_) {
            delete entry.coro;
        }
        sleepHeap_.clear();
    }
}

void Scheduler::spawn(std::function<void()> fn) {
    auto* coro = new Coroutine;
    coro->fn = std::move(fn);
    coro->stack.resize(kCoroutineStackSize);
    getcontext(&coro->ctx);
    coro->ctx.uc_stack.ss_sp = coro->stack.data();
    coro->ctx.uc_stack.ss_size = coro->stack.size();
    coro->ctx.uc_link = nullptr;  // explicit yield back to the worker
    makecontext(&coro->ctx, reinterpret_cast<void (*)()>(&Scheduler::coroutineEntry), 0);

    {
        std::lock_guard<std::mutex> lock(readyMutex_);
        readyQueue_.push_back(coro);
    }
    readyCv_.notify_one();
}

void Scheduler::coSleep(std::chrono::milliseconds ms) {
    Coroutine* coro = tlsCurrentCoro_;
    ucontext_t* workerMain = tlsWorkerMain_;
    Scheduler* sched = tlsScheduler_;
    if (coro == nullptr || workerMain == nullptr || sched == nullptr) {
        std::this_thread::sleep_for(ms);  // called outside a coroutine
        return;
    }
    // Park the coroutine on the sleep heap and yield to the worker.
    {
        std::lock_guard<std::mutex> lock(sched->sleepMutex_);
        sched->sleepHeap_.push_back(
            {std::chrono::steady_clock::now() + ms, coro});
        std::push_heap(sched->sleepHeap_.begin(), sched->sleepHeap_.end(),
                       [](const SleepEntry& a, const SleepEntry& b) { return a.deadline > b.deadline; });
    }
    sched->sleepCv_.notify_one();
    swapcontext(&coro->ctx, workerMain);
}

void Scheduler::workerLoop(std::size_t workerIndex) {
    tlsScheduler_ = this;
    tlsWorkerMain_ = &workerMainCtx_[workerIndex];
    while (running_.load()) {
        Coroutine* coro = nullptr;
        {
            std::unique_lock<std::mutex> lock(readyMutex_);
            readyCv_.wait(lock, [this]() {
                return !running_.load() || !readyQueue_.empty();
            });
            if (!running_.load() && readyQueue_.empty()) {
                break;
            }
            coro = readyQueue_.front();
            readyQueue_.pop_front();
        }

        tlsCurrentCoro_ = coro;
        // Enter the coroutine; it yields back here on co_sleep or on finish.
        swapcontext(&workerMainCtx_[workerIndex], &coro->ctx);
        tlsCurrentCoro_ = nullptr;

        if (coro->done) {
            delete coro;  // finished; no queue holds it anymore
        }
    }
    tlsScheduler_ = nullptr;
    tlsWorkerMain_ = nullptr;
}

void Scheduler::coroutineEntry() {
    Coroutine* coro = tlsCurrentCoro_;
    if (coro != nullptr && coro->fn) {
        coro->fn();
    }
    if (coro != nullptr) {
        coro->done = true;
    }
    // Yield back to whichever worker resumed us; it reaps us on return.
    ucontext_t* workerMain = tlsWorkerMain_;
    if (coro != nullptr && workerMain != nullptr) {
        swapcontext(&coro->ctx, workerMain);
    }
}

void Scheduler::timerLoop() {
    while (running_.load() || !sleepHeap_.empty()) {
        SleepEntry entry;
        {
            std::unique_lock<std::mutex> lock(sleepMutex_);
            if (sleepHeap_.empty()) {
                if (!running_.load()) {
                    break;
                }
                sleepCv_.wait(lock);
                continue;
            }
            const auto frontDeadline = sleepHeap_.front().deadline;
            sleepCv_.wait_until(lock, frontDeadline);
            if (sleepHeap_.empty()) {
                continue;
            }
            entry = sleepHeap_.front();
            if (std::chrono::steady_clock::now() < entry.deadline) {
                continue;  // spurious wakeup or a closer entry arrived
            }
            std::pop_heap(sleepHeap_.begin(), sleepHeap_.end(),
                          [](const SleepEntry& a, const SleepEntry& b) {
                              return a.deadline > b.deadline;
                          });
            sleepHeap_.pop_back();
        }
        if (entry.coro != nullptr && !entry.coro->done) {
            std::lock_guard<std::mutex> lock(readyMutex_);
            readyQueue_.push_back(entry.coro);
            readyCv_.notify_one();
        }
    }
}
