#include "Histogram.h"

#include <algorithm>
#include <cstdio>

Histogram::Histogram(std::vector<double> bounds)
    : bounds_(std::move(bounds)), counts_(bounds_.size() + 1, 0) {}

void Histogram::observe(double valueMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::upper_bound(bounds_.begin(), bounds_.end(), valueMs);
    counts_[static_cast<std::size_t>(it - bounds_.begin())]++;
    count_++;
    sumMs_ += valueMs;
}

std::uint64_t Histogram::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

double Histogram::sumMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sumMs_;
}

void Histogram::renderPrometheus(std::ostringstream* out, const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        cumulative += counts_[i];
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3g", bounds_[i]);
        *out << name << "_bucket{le=\"" << buf << "\"} " << cumulative << "\n";
    }
    cumulative += counts_[bounds_.size()];
    *out << name << "_bucket{le=\"+Inf\"} " << cumulative << "\n";
    *out << name << "_sum " << sumMs_ << "\n";
    *out << name << "_count " << count_ << "\n";
}
