#include "LoadBalancer.h"

#include <algorithm>
#include <random>

namespace {

std::uint32_t fnv1a32(const char* data, std::size_t len) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

std::size_t P2cBalancer::pick(const std::vector<InstanceState>& states, uint32_t) {
    std::vector<std::size_t> healthy;
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (states[i].healthy) {
            healthy.push_back(i);
        }
    }
    if (healthy.empty()) {
        return states.size();  // caller treats it as "no instance available"
    }
    if (healthy.size() == 1) {
        return healthy[0];
    }

    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> dist(0, healthy.size() - 1);
    const std::size_t a = healthy[dist(rng)];
    const std::size_t b = healthy[dist(rng)];

    const auto score = [&](std::size_t idx) {
        const auto& s = states[idx];
        const double w = s.weight > 0.0 ? s.weight : 1.0;
        return s.latencyEmaMs * (static_cast<double>(s.inflight) + 1.0) / w;
    };
    return score(a) <= score(b) ? a : b;
}

std::size_t WeightedRoundRobinBalancer::pick(const std::vector<InstanceState>& states, uint32_t) {
    std::vector<std::size_t> healthy;
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (states[i].healthy) {
            healthy.push_back(i);
        }
    }
    if (healthy.empty()) {
        return states.size();
    }
    const std::size_t n = healthy.size();
    // Reset the persistent weights when the healthy set size changes, so a
    // dead/recovered instance restarts the smooth sequence cleanly.
    if (current_.size() != n) {
        current_.assign(n, 0.0);
    }
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        total += states[healthy[i]].weight;
    }
    std::size_t best = 0;
    for (std::size_t i = 0; i < n; ++i) {
        current_[i] += states[healthy[i]].weight;
        if (current_[i] > current_[best]) {
            best = i;
        }
    }
    current_[best] -= total;
    return healthy[best];
}

ConsistentHashBalancer::ConsistentHashBalancer(std::size_t virtualNodes)
    : virtualNodes_(virtualNodes > 0 ? virtualNodes : 160) {}

void ConsistentHashBalancer::rebuild(const std::vector<InstanceState>& states) {
    ring_.clear();
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (!states[i].healthy) {
            continue;
        }
        for (std::size_t v = 0; v < virtualNodes_; ++v) {
            const std::string key = std::to_string(i) + "#" + std::to_string(v);
            ring_.push_back(RingEntry{fnv1a32(key.data(), key.size()), i});
        }
    }
    std::sort(ring_.begin(), ring_.end());
}

std::size_t ConsistentHashBalancer::pick(const std::vector<InstanceState>& states, uint32_t requestKey) {
    if (ringVersion_ != states.size() + 1) {  // simple invalidation on size change
        rebuild(states);
        ringVersion_ = states.size() + 1;
    }
    if (ring_.empty()) {
        return states.size();
    }
    const uint32_t key = requestKey != 0 ? requestKey : fnv1a32("default", 7);
    auto it = std::lower_bound(ring_.begin(), ring_.end(), RingEntry{key, 0});
    if (it == ring_.end()) {
        it = ring_.begin();
    }
    return it->instanceIndex;
}
