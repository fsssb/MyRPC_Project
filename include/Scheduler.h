#ifndef MYRPCPROJECT_INCLUDE_SCHEDULER_H_
#define MYRPCPROJECT_INCLUDE_SCHEDULER_H_

// macOS keeps ucontext behind _XOPEN_SOURCE (deprecated but functional); the
// define must precede any system header in every translation unit that
// includes this header.
#if defined(__APPLE__)
#define _XOPEN_SOURCE 700
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <ucontext.h>
#include <vector>

// M:N user-space coroutine scheduler (a simplified bthread; see
// docs/v2-design-draft.md section 3.2).
//
// - M coroutines run on N worker threads; a coroutine blocks only itself, so
//   a 2s co_sleep does not occupy a worker thread;
// - scheduling is a single global ready queue guarded by a condition variable
//   (no work-stealing; documented simplification over bRPC);
// - sleep is timer-based: a coroutine that calls co_sleep yields its worker
//   and is woken by a dedicated timer thread when its deadline arrives;
// - a worker that finds no ready coroutine blocks on the condition variable
//   instead of spinning.
//
// Coroutines must only call scheduler blocking primitives (co_sleep); blocking
// system calls inside a coroutine would still block a worker (same NonGoal as
// bRPC: we do not hook glibc calls).
class Scheduler {
public:
    Scheduler() = default;
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start(std::size_t workerCount);
    void stop();

    // Enqueue a function to run on a new coroutine.
    void spawn(std::function<void()> fn);

    // Suspend the current coroutine for ms (from any thread: falls back to
    // std::this_thread::sleep_for when not running inside a coroutine).
    static void coSleep(std::chrono::milliseconds ms);

private:
    struct Coroutine {
        std::function<void()> fn;
        std::vector<char> stack;
        // makecontext/swapcontext; deprecated on macOS but functional. The
        // context lives inside the coroutine and resumes on whatever worker
        // picks it up, so it must never run on two threads at once.
        ucontext_t ctx{};
        bool done{false};
    };

    struct SleepEntry {
        std::chrono::steady_clock::time_point deadline;
        Coroutine* coro;
        bool operator>(const SleepEntry& other) const { return deadline > other.deadline; }
    };

    void workerLoop(std::size_t workerIndex);
    void timerLoop();
    void wakeExpired();

    static void coroutineEntry();

    // Per-thread coroutine context (only valid while a coroutine runs).
    static thread_local Coroutine* tlsCurrentCoro_;
    static thread_local ucontext_t* tlsWorkerMain_;
    static thread_local Scheduler* tlsScheduler_;

private:
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    std::thread timerThread_;

    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    std::deque<Coroutine*> readyQueue_;

    std::mutex sleepMutex_;
    std::vector<SleepEntry> sleepHeap_;  // min-heap by deadline (std::push_heap)
    std::condition_variable sleepCv_;

    std::vector<ucontext_t> workerMainCtx_;  // one per worker
};

#endif  // MYRPCPROJECT_INCLUDE_SCHEDULER_H_
