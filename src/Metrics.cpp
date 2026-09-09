#include "Metrics.h"

#include <sstream>

Metrics& Metrics::instance() {
    static Metrics metrics;
    return metrics;
}

void Metrics::markRequest(std::size_t n) {
    totalRequests_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    requestsInCurrentSecond_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
}

void Metrics::observeLatency(double valueMs) {
    latency_.observe(valueMs);
}

void Metrics::observeStatus(std::uint16_t status) {
    if (status < 16) {
        statusCounts_[status].fetch_add(1, std::memory_order_relaxed);
    }
}

void Metrics::setActiveConnections(std::size_t n) {
    activeConnections_.store(n, std::memory_order_relaxed);
}

void Metrics::setTaskQueueDepth(std::size_t n) {
    taskQueueDepth_.store(n, std::memory_order_relaxed);
}

std::string Metrics::renderPrometheusAndRotate() {
    const uint64_t qps = requestsInCurrentSecond_.exchange(0, std::memory_order_relaxed);
    const uint64_t total = totalRequests_.load(std::memory_order_relaxed);
    const std::size_t active = activeConnections_.load(std::memory_order_relaxed);
    const std::size_t queue = taskQueueDepth_.load(std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "# TYPE myrpc_qps gauge\n";
    oss << "myrpc_qps " << qps << "\n";
    oss << "# TYPE myrpc_total_requests counter\n";
    oss << "myrpc_total_requests " << total << "\n";
    oss << "# TYPE myrpc_active_connections gauge\n";
    oss << "myrpc_active_connections " << active << "\n";
    oss << "# TYPE myrpc_task_queue_depth gauge\n";
    oss << "myrpc_task_queue_depth " << queue;
    return oss.str();
}

std::string Metrics::renderSnapshot() {
    const uint64_t total = totalRequests_.load(std::memory_order_relaxed);
    const std::size_t active = activeConnections_.load(std::memory_order_relaxed);
    const std::size_t queue = taskQueueDepth_.load(std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "# TYPE myrpc_total_requests counter\n";
    oss << "myrpc_total_requests " << total << "\n";
    oss << "# TYPE myrpc_latency_ms histogram\n";
    latency_.renderPrometheus(&oss, "myrpc_latency_ms");
    oss << "# TYPE myrpc_status counter\n";
    for (int i = 0; i < 16; ++i) {
        const uint64_t n = statusCounts_[i].load(std::memory_order_relaxed);
        if (n > 0) {
            oss << "myrpc_status{code=\"" << i << "\"} " << n << "\n";
        }
    }
    oss << "# TYPE myrpc_active_connections gauge\n";
    oss << "myrpc_active_connections " << active << "\n";
    oss << "# TYPE myrpc_task_queue_depth gauge\n";
    oss << "myrpc_task_queue_depth " << queue;
    return oss.str();
}
