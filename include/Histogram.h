#ifndef MYRPCPROJECT_INCLUDE_HISTOGRAM_H_
#define MYRPCPROJECT_INCLUDE_HISTOGRAM_H_

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// A fixed-bound latency histogram with Prometheus-style bucket rendering.
// Buckets are cumulative: count of observations <= each bound. Thread-safe
// (latencies arrive from handler completion threads).
class Histogram {
public:
    // bounds: upper edges in milliseconds, strictly increasing.
    // The trailing +Inf bucket is implicit.
    explicit Histogram(std::vector<double> bounds);

    void observe(double valueMs);

    std::uint64_t count() const;
    double sumMs() const;

    // Prometheus text: name_bucket{le=".."} n ... name_sum / name_count.
    void renderPrometheus(std::ostringstream* out, const std::string& name) const;

private:
    const std::vector<double> bounds_;
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> counts_;  // size = bounds_.size() + 1
    std::uint64_t count_{0};
    double sumMs_{0.0};
};

// Default RPC latency histogram bounds in milliseconds.
inline std::vector<double> defaultLatencyBoundsMs() {
    return {0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0};
}

#endif  // MYRPCPROJECT_INCLUDE_HISTOGRAM_H_
