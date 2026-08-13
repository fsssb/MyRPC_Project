#ifndef MYRPCPROJECT_INCLUDE_RPCCLUSTERCHANNEL_H_
#define MYRPCPROJECT_INCLUDE_RPCCLUSTERCHANNEL_H_

#include "CircuitBreaker.h"
#include "LoadBalancer.h"
#include "Registry.h"
#include "RetryPolicy.h"
#include "RpcChannel.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class EventLoop;

// A logical channel to a cluster of server instances: it holds one RpcChannel
// per instance and dispatches every call to an instance chosen by the load
// balancer. Instances whose connection has failed (isHealthy() == false) are
// skipped by the balancer; the caller observes the failure through the
// RpcController status instead of hanging.
//
// See docs/v2-design-draft.md section 2.3.
class RpcClusterChannel : public std::enable_shared_from_this<RpcClusterChannel> {
public:
    using Done = RpcChannel::Done;

    explicit RpcClusterChannel(EventLoop* loop);

    RpcClusterChannel(const RpcClusterChannel&) = delete;
    RpcClusterChannel& operator=(const RpcClusterChannel&) = delete;

    void addInstance(const std::string& host, uint16_t port);
    void removeInstance(const std::string& host, uint16_t port);
    std::size_t instanceCount() const;

    void setLoadBalancer(std::shared_ptr<LoadBalancer> lb);

    // Switch the instance list to service discovery: the initial set comes
    // from registry->lookup(service), then one-shot watches keep it in sync
    // (instances added/removed/re-expired are applied incrementally).
    void setDiscovery(Registry* registry, const std::string& service);

    // Retry configuration: max attempts, cluster-wide retry budget and
    // backoff limits. Defaults are RetryOptions{}.
    void setRetryOptions(const RetryOptions& options);

    // Number of retries performed (attempts beyond the first), for
    // observability / acceptance checks.
    uint32_t retryCount() const { return retries_.load(std::memory_order_relaxed); }

    // Async / sync calls, forwarded to one instance through the balancer.
    void callAsync(RpcController& controller, const Value& request,
                   Value* response, Done done);
    bool call(RpcController& controller, const Value& request, Value* response);

private:
    struct Instance {
        std::string host;
        uint16_t port;
        std::shared_ptr<RpcChannel> channel;
        std::shared_ptr<CircuitBreaker> breaker;
    };

    void attemptOnce(RpcController& controller, const Value& request,
                     Value* response, Done done, uint32_t attempt,
                     const std::shared_ptr<std::atomic<bool>>& completed, std::size_t excludeIndex,
                     bool hedge);
    std::size_t pickInstance(uint32_t requestKey);
    void failCall(RpcController* controller, proto::Status status,
                  const std::string& text, Done done);
    void applyInstances(const std::vector<InstanceInfo>& infos);
    void resubscribeWatch();

private:
    EventLoop* loop_;
    mutable std::mutex mutex_;
    std::vector<Instance> instances_;
    std::shared_ptr<LoadBalancer> lb_;
    RetryOptions retryOptions_;
    RetryBudget retryBudget_{10};
    std::atomic<uint32_t> retries_{0};
    Registry* discoveryRegistry_{nullptr};  // owned by the caller
    std::string discoveryService_;
};

#endif  // MYRPCPROJECT_INCLUDE_RPCCLUSTERCHANNEL_H_
