#ifndef MYRPCPROJECT_INCLUDE_LOADBALANCER_H_
#define MYRPCPROJECT_INCLUDE_LOADBALANCER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

// Load balancing strategies over the healthy instances of a cluster
// (see docs/v2-design-draft.md section 2.3).
struct InstanceState {
    bool healthy{false};
    std::size_t inflight{0};
    double latencyEmaMs{0.0};
    double weight{1.0};
};

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;

    // Pick an index into `states`; only healthy instances are considered.
    // requestKey is used by hash-based strategies (0 when unused).
    virtual std::size_t pick(const std::vector<InstanceState>& states, uint32_t requestKey) = 0;
};

// Power of two choices: sample two healthy instances at random and pick the
// one with the lower score = latencyEma * (inflight + 1) / weight. O(1) and
// independent of cluster size (bRPC p2c style).
class P2cBalancer : public LoadBalancer {
public:
    std::size_t pick(const std::vector<InstanceState>& states, uint32_t /*requestKey*/) override;
};

// Smooth weighted round-robin (guarantees instances are spread out).
class WeightedRoundRobinBalancer : public LoadBalancer {
public:
    std::size_t pick(const std::vector<InstanceState>& states, uint32_t /*requestKey*/) override;

private:
    std::vector<double> current_;  // persistent current weights (smooth WRR)
};

// Consistent hashing (160 virtual nodes per instance) for cache-like methods;
// callers should set a request key via RpcController::setRequestKey.
class ConsistentHashBalancer : public LoadBalancer {
public:
    explicit ConsistentHashBalancer(std::size_t virtualNodes = 160);

    std::size_t pick(const std::vector<InstanceState>& states, uint32_t requestKey) override;

private:
    void rebuild(const std::vector<InstanceState>& states);

    struct RingEntry {
        uint32_t hash;
        std::size_t instanceIndex;
        bool operator<(const RingEntry& other) const { return hash < other.hash; }
    };

    std::size_t virtualNodes_;
    std::vector<RingEntry> ring_;
    std::size_t ringVersion_{0};  // bump when the healthy set changes
};

#endif  // MYRPCPROJECT_INCLUDE_LOADBALANCER_H_
