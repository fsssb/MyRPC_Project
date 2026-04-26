#ifndef MYRPCPROJECT_INCLUDE_METRICS_H_
#define MYRPCPROJECT_INCLUDE_METRICS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class Metrics {
public:
    static Metrics& instance();
    virtual ~Metrics() = default;

    void markRequest(std::size_t n = 1);
    void setActiveConnections(std::size_t n);
    void setTaskQueueDepth(std::size_t n);

    std::string renderPrometheusAndRotate();

private:
    Metrics() = default;

private:
    std::atomic<uint64_t> totalRequests_{0};
    std::atomic<uint64_t> requestsInCurrentSecond_{0};
    std::atomic<std::size_t> activeConnections_{0};
    std::atomic<std::size_t> taskQueueDepth_{0};
};

#endif  // MYRPCPROJECT_INCLUDE_METRICS_H_
