#ifndef MYRPCPROJECT_INCLUDE_METRICS_H_
#define MYRPCPROJECT_INCLUDE_METRICS_H_

#include "Histogram.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// Process-wide observability (V2.2): request counters, a latency histogram,
// status-code distribution and Prometheus text rendering. Renderers are cheap
// and callable from any thread.
class Metrics {
public:
    static Metrics& instance();
    virtual ~Metrics() = default;

    void markRequest(std::size_t n = 1);
    void observeLatency(double valueMs);
    void observeStatus(std::uint16_t status);
    void setActiveConnections(std::size_t n);
    void setTaskQueueDepth(std::size_t n);

    // Rotates the per-second counter (used by the periodic log line).
    std::string renderPrometheusAndRotate();
    // Non-destructive snapshot for the /metrics endpoint.
    std::string renderSnapshot();

private:
    Metrics() = default;

private:
    std::atomic<uint64_t> totalRequests_{0};
    std::atomic<uint64_t> requestsInCurrentSecond_{0};
    std::atomic<std::size_t> activeConnections_{0};
    std::atomic<std::size_t> taskQueueDepth_{0};
    Histogram latency_{defaultLatencyBoundsMs()};
    std::atomic<uint64_t> statusCounts_[16]{};
};

#endif  // MYRPCPROJECT_INCLUDE_METRICS_H_
