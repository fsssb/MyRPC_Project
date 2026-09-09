// Coroutine scheduler demo & acceptance (V2.2-B): N concurrent coroutines each
// co_sleep for 100ms on W workers. Serial execution would take N*100ms; with
// M:N scheduling the elapsed time is ~100ms + scheduling overhead because a
// sleeping coroutine does not occupy a worker.
//
// Usage: ./rpc_coroutine_demo [workers] [count]
#include "Scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char* argv[]) {
    const int workers = argc > 1 ? std::atoi(argv[1]) : 4;
    const int count = argc > 2 ? std::atoi(argv[2]) : 100;

    Scheduler sched;
    sched.start(static_cast<std::size_t>(workers));

    std::atomic<int> done{0};
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        sched.spawn([&done]() {
            Scheduler::coSleep(std::chrono::milliseconds(100));
            done.fetch_add(1);
        });
    }
    while (done.load() < count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
    std::printf("workers=%d coroutines=%d co_sleep(100ms) elapsed=%lldms (serial ~%dms)\n",
                workers, count, elapsedMs, count * 100);
    sched.stop();
    const bool ok = elapsedMs < static_cast<long long>(count) * 50;  // clearly parallel
    std::printf(ok ? "COROUTINE DEMO PASSED\n" : "COROUTINE DEMO FAILED\n");
    return ok ? 0 : 1;
}
