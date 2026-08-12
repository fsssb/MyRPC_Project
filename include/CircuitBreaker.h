#ifndef MYRPCPROJECT_INCLUDE_CIRCUITBREAKER_H_
#define MYRPCPROJECT_INCLUDE_CIRCUITBREAKER_H_

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>

// Per-instance circuit breaker with a sliding-window error rate, half-open
// probe recovery and a minimum request amount to avoid tripping on low traffic
// (see docs/v2-design-draft.md section 2.4, modeled on Hystrix / bRPC EMA).
//
// State machine:
//   CLOSED  -> (window error rate > threshold AND requests >= minAmount) -> OPEN
//   OPEN    -> (isolation elapsed) -> HALF_OPEN (one probe request is allowed)
//   HALF_OPEN -> probe success -> CLOSED (window reset); probe failure -> OPEN
//
// The isolation period starts at 100ms and doubles on every trip, capped at
// 30s, so a recovering backend is not hammered right after it comes back.
class CircuitBreaker {
public:
    explicit CircuitBreaker(std::size_t minRequestAmount = 5,
                            double errorRateThreshold = 0.5,
                            std::chrono::milliseconds windowMs = std::chrono::seconds(10));

    // Whether a request is allowed right now; a HALF_OPEN probe consumes the
    // single probe slot.
    bool allowRequest();

    void onSuccess();
    void onError();

    bool isOpen() const;

    // Whether the instance may take part in load balancing: CLOSED/HALF_OPEN,
    // or OPEN whose isolation period has elapsed (picked instances then get a
    // single half-open probe via allowRequest). Without this an open circuit
    // would be skipped forever and never recover.
    bool isAvailable() const;

private:
    struct Bucket {
        std::chrono::steady_clock::time_point start;
        std::size_t requests{0};
        std::size_t errors{0};
    };

    void record(bool error);
    void tally(std::size_t* total, std::size_t* errors) const;
    void rollWindowLocked();
    std::chrono::milliseconds isolation() const;

    mutable std::mutex mutex_;
    std::deque<Bucket> buckets_;
    const std::chrono::milliseconds windowMs_;

    enum class State { Closed, Open, HalfOpen };
    State state_{State::Closed};
    std::chrono::steady_clock::time_point openedAt_{};
    std::chrono::milliseconds lastIsolation_{100};

    const std::size_t minRequestAmount_;
    const double errorRateThreshold_;
};

#endif  // MYRPCPROJECT_INCLUDE_CIRCUITBREAKER_H_
