#ifndef MYRPCPROJECT_INCLUDE_REGISTRY_H_
#define MYRPCPROJECT_INCLUDE_REGISTRY_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Service registry abstraction (see docs/v2-design-draft.md section 2.2,
// modeled on ZooKeeper: ephemeral instances + one-shot watches + lease-based
// failure detection).
struct InstanceInfo {
    std::string service;
    std::string host;
    uint16_t port{0};
    uint64_t instanceId{0};  // incremented on every (re)start

    std::string addr() const { return host + ":" + std::to_string(port); }
};

class Registry {
public:
    using WatchCallback = std::function<void()>;

    virtual ~Registry() = default;

    // Register an instance with a lease; the instance must renewLease() before
    // the lease expires or it is removed (ephemeral semantics).
    virtual void registerService(const InstanceInfo& info, uint32_t leaseMs) = 0;

    // Explicit removal (graceful shutdown).
    virtual void unregister(const std::string& service, uint64_t instanceId) = 0;

    virtual void renewLease(const std::string& service, uint64_t instanceId) = 0;

    virtual std::vector<InstanceInfo> lookup(const std::string& service) = 0;

    // One-shot watch: cb is invoked once when the service set changes, then
    // the subscription expires; the caller must re-register (ZooKeeper
    // semantics). A connection-loss style refresh is the caller's duty.
    virtual void watch(const std::string& service, WatchCallback cb) = 0;
};

#endif  // MYRPCPROJECT_INCLUDE_REGISTRY_H_
