#ifndef MYRPCPROJECT_INCLUDE_LOCALREGISTRY_H_
#define MYRPCPROJECT_INCLUDE_LOCALREGISTRY_H_

#include "Registry.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// In-process registry implementation for development and acceptance (see
// docs/v2-design-draft.md section 2.2):
//
// - instances are ephemeral: a lease must be renewed, expired leases are
//   dropped (scanned lazily on lookup/renew and by a background sweeper);
// - watches are one-shot: a change invokes the callback once, then the
//   subscription expires (ZooKeeper semantics; the caller re-subscribes);
// - a per-service version counter lets watchers detect missed changes.
//
// Cross-process service discovery would use a real registry (etcd/ZooKeeper);
// LocalRegistry keeps the interface and semantics testable in one process.
class LocalRegistry : public Registry {
public:
    LocalRegistry();
    ~LocalRegistry() override;

    LocalRegistry(const LocalRegistry&) = delete;
    LocalRegistry& operator=(const LocalRegistry&) = delete;

    void registerService(const InstanceInfo& info, uint32_t leaseMs) override;
    void unregister(const std::string& service, uint64_t instanceId) override;
    void renewLease(const std::string& service, uint64_t instanceId) override;
    std::vector<InstanceInfo> lookup(const std::string& service) override;
    void watch(const std::string& service, WatchCallback cb) override;

    // Visible for acceptance checks.
    std::size_t instanceCount(const std::string& service) const;

private:
    struct Entry {
        InstanceInfo info;
        std::chrono::steady_clock::time_point leaseDeadline;
        uint32_t leaseMs{10000};
    };
    struct ServiceState {
        std::map<uint64_t, Entry> instances;
        std::vector<WatchCallback> watchers;
        uint64_t version{0};
    };

    // Sweeps expired leases; returns callbacks to run outside the lock.
    // Assumes mutex_ is held.
    std::vector<WatchCallback> sweepExpiredLocked(ServiceState* state);
    void sweeperLoop();

    mutable std::mutex mutex_;
    std::map<std::string, ServiceState> services_;
    uint64_t nextInstanceId_{1};
    std::thread sweeper_;
    bool running_{true};
};

#endif  // MYRPCPROJECT_INCLUDE_LOCALREGISTRY_H_
