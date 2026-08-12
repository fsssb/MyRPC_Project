#include "CircuitBreaker.h"

#include <algorithm>

namespace {
constexpr std::chrono::milliseconds kMaxIsolation{30000};
}  // namespace

CircuitBreaker::CircuitBreaker(std::size_t minRequestAmount, double errorRateThreshold,
                               std::chrono::milliseconds windowMs)
    : windowMs_(windowMs),
      minRequestAmount_(minRequestAmount),
      errorRateThreshold_(errorRateThreshold) {}

bool CircuitBreaker::allowRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    rollWindowLocked();
    switch (state_) {
        case State::Closed: {
            std::size_t total = 0;
            std::size_t errors = 0;
            tally(&total, &errors);
            if (total >= minRequestAmount_ &&
                static_cast<double>(errors) / static_cast<double>(total) > errorRateThreshold_) {
                state_ = State::Open;
                openedAt_ = std::chrono::steady_clock::now();
            }
            return true;
        }
        case State::Open: {
            if (std::chrono::steady_clock::now() - openedAt_ >= lastIsolation_) {
                state_ = State::HalfOpen;  // grant a single probe below
                return true;
            }
            return false;
        }
        case State::HalfOpen:
            return false;  // the single probe slot is taken until it resolves
    }
    return false;
}

void CircuitBreaker::onSuccess() {
    std::lock_guard<std::mutex> lock(mutex_);
    rollWindowLocked();
    if (state_ == State::HalfOpen) {
        // Probe succeeded: close the circuit and reset the window/isolation.
        state_ = State::Closed;
        buckets_.clear();
        lastIsolation_ = std::chrono::milliseconds(100);
        return;
    }
    record(false);
}

void CircuitBreaker::onError() {
    std::lock_guard<std::mutex> lock(mutex_);
    rollWindowLocked();
    if (state_ == State::HalfOpen) {
        // Probe failed: re-open with a doubled isolation period.
        state_ = State::Open;
        openedAt_ = std::chrono::steady_clock::now();
        lastIsolation_ = std::min(lastIsolation_ * 2, kMaxIsolation);
        return;
    }
    record(true);
}

bool CircuitBreaker::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Open;
}

bool CircuitBreaker::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Closed || state_ == State::HalfOpen) {
        return true;
    }
    // OPEN: usable again once the isolation period has elapsed (a picked
    // request then flips the circuit to HALF_OPEN as its single probe).
    return std::chrono::steady_clock::now() - openedAt_ >= lastIsolation_;
}

void CircuitBreaker::record(bool error) {
    if (buckets_.empty() ||
        std::chrono::steady_clock::now() - buckets_.back().start >= windowMs_) {
        rollWindowLocked();
    }
    buckets_.back().requests++;
    if (error) {
        buckets_.back().errors++;
    }
}

void CircuitBreaker::tally(std::size_t* total, std::size_t* errors) const {
    *total = 0;
    *errors = 0;
    for (const auto& b : buckets_) {
        *total += b.requests;
        *errors += b.errors;
    }
}

void CircuitBreaker::rollWindowLocked() {
    const auto now = std::chrono::steady_clock::now();
    // Drop buckets older than the window (buckets are 1s slices).
    while (!buckets_.empty() && now - buckets_.front().start >= windowMs_) {
        buckets_.pop_front();
    }
    if (buckets_.empty() || now - buckets_.back().start >= std::chrono::seconds(1)) {
        Bucket b;
        b.start = now;
        buckets_.push_back(b);
    }
}
