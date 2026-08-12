#ifndef MYRPCPROJECT_INCLUDE_CONCURRENCYLIMITER_H_
#define MYRPCPROJECT_INCLUDE_CONCURRENCYLIMITER_H_

#include <atomic>
#include <cstddef>

// Server-side overload protection: a lightweight atomic semaphore limiting the
// number of in-flight requests (see docs/v2-design-draft.md section 2.6,
// "limit concurrency, not QPS" -- concurrency = peak QPS x low-load latency).
// When the limit is hit the server replies CONCURRENCY_LIMITED immediately
// instead of queueing, so clients can retry another instance.
//
// max == 0 means unlimited.
class ConcurrencyLimiter {
public:
    explicit ConcurrencyLimiter(std::size_t max) : max_(max) {}

    bool tryAcquire() {
        if (max_ == 0) {
            return true;
        }
        std::size_t current = current_.load(std::memory_order_relaxed);
        while (current < max_) {
            if (current_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release() {
        current_.fetch_sub(1, std::memory_order_relaxed);
    }

    std::size_t current() const {
        return current_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t max_;
    std::atomic<std::size_t> current_{0};
};

#endif  // MYRPCPROJECT_INCLUDE_CONCURRENCYLIMITER_H_
