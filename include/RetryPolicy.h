#ifndef MYRPCPROJECT_INCLUDE_RETRYPOLICY_H_
#define MYRPCPROJECT_INCLUDE_RETRYPOLICY_H_

#include "Protocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

// Client-side retry policy (see docs/v2-design-draft.md section 2.5):
//
// - only idempotent calls are retried (the caller marks them via
//   RpcController::setIdempotent);
// - deadline semantics: kDeadlineExceeded is never retried;
// - connection failures (kUnknown) are retried when idempotent;
// - a token budget caps retries cluster-wide to avoid retry storms
//   (AWS-style: consume on retry, refund on success);
// - backoff is exponential with full jitter: random * min(max, base * 2^n).

struct RetryOptions {
    uint32_t maxAttempts{3};      // total attempts including the first
    uint32_t budgetTokens{10};    // cluster-wide retry budget
    uint32_t baseBackoffMs{50};
    uint32_t maxBackoffMs{1000};
    // Hedging (backup request): when an idempotent call is still pending after
    // hedgeAfterMs, a duplicate is sent to another instance; the first response
    // wins and the other is dropped. 0 disables hedging (bRPC backup_request).
    uint32_t hedgeAfterMs{0};

    // Recovery rate limiting: when every instance is circuit-open, requests
    // are admitted with probability min(recoveryMinWorking / instances, 1),
    // so a recovering backend is not hit by the full burst at once (bRPC
    // q/min_working_instances). 0 disables the admission control.
    uint32_t recoveryMinWorking{1};
};

class RetryBudget {
public:
    explicit RetryBudget(uint32_t maxTokens) : maxTokens_(maxTokens), tokens_(maxTokens) {}

    void reset(uint32_t maxTokens) {
        maxTokens_ = maxTokens;
        tokens_.store(maxTokens, std::memory_order_relaxed);
    }

    bool tryConsume() {
        uint32_t t = tokens_.load(std::memory_order_relaxed);
        while (t > 0) {
            if (tokens_.compare_exchange_weak(t, t - 1, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void refund() {
        uint32_t t = tokens_.load(std::memory_order_relaxed);
        while (t < maxTokens_) {
            if (tokens_.compare_exchange_weak(t, t + 1, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    uint32_t available() const { return tokens_.load(std::memory_order_relaxed); }

private:
    uint32_t maxTokens_;
    std::atomic<uint32_t> tokens_;
};

namespace retry {

inline bool shouldRetry(proto::Status status, bool idempotent, uint32_t attempt,
                        uint32_t maxAttempts) {
    if (attempt >= maxAttempts) {
        return false;
    }
    switch (status) {
        case proto::kOk:
        case proto::kDeadlineExceeded:
            return false;  // deadline is final (bRPC: ERPCTIMEDOUT never retries)
        case proto::kUnknown:  // connection-class failure
            return idempotent;
        default:
            return false;  // conservative: explicit server errors are not retried
    }
}

inline std::chrono::milliseconds jitterBackoff(uint32_t attempt, uint32_t baseMs,
                                               uint32_t maxMs) {
    const uint64_t exp = static_cast<uint64_t>(baseMs) << (attempt > 8 ? 8 : attempt);
    const uint64_t cap = exp > maxMs ? maxMs : exp;
    const uint64_t r = static_cast<uint64_t>(
        std::rand() % 1000) * cap / 1000;  // random in [0, cap), full jitter
    return std::chrono::milliseconds(static_cast<uint32_t>(r));
}

}  // namespace retry

#endif  // MYRPCPROJECT_INCLUDE_RETRYPOLICY_H_
