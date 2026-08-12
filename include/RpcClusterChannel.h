#ifndef MYRPCPROJECT_INCLUDE_RPCCLUSTERCHANNEL_H_
#define MYRPCPROJECT_INCLUDE_RPCCLUSTERCHANNEL_H_

#include "CircuitBreaker.h"
#include "LoadBalancer.h"
#include "RpcChannel.h"

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

    std::size_t pickInstance(uint32_t requestKey);
    void failCall(RpcController* controller, proto::Status status,
                  const std::string& text, Done done);

private:
    EventLoop* loop_;
    mutable std::mutex mutex_;
    std::vector<Instance> instances_;
    std::shared_ptr<LoadBalancer> lb_;
};

#endif  // MYRPCPROJECT_INCLUDE_RPCCLUSTERCHANNEL_H_
