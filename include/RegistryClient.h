#ifndef MYRPCPROJECT_INCLUDE_REGISTRYCLIENT_H_
#define MYRPCPROJECT_INCLUDE_REGISTRYCLIENT_H_

#include "Registry.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class RpcClient;
class RpcChannel;

// RPC-backed implementation of the Registry interface: talks to a
// rpc_registry_server over the RPC stack, so service instances in separate
// processes can share one registry (see docs/v2-design-draft.md section 2.2).
//
// The server keeps the LocalRegistry semantics (ephemeral leases, one-shot
// watches); this client adapts them to the wire:
//   registry.register / unregister / renew / lookup  -> normal RPC calls
//   registry.watch                                   -> long-poll: the call
//   stays pending on the server until the service set changes, then returns
//   and the client re-issues it (watch loop runs on an internal thread).
class RegistryClient : public Registry {
public:
    // Spins up its own RPC client (event loop thread + channel) to the
    // registry server.
    RegistryClient(std::string host, uint16_t port);
    ~RegistryClient() override;

    RegistryClient(const RegistryClient&) = delete;
    RegistryClient& operator=(const RegistryClient&) = delete;

    void registerService(const InstanceInfo& info, uint32_t leaseMs) override;
    void unregister(const std::string& service, uint64_t instanceId) override;
    void renewLease(const std::string& service, uint64_t instanceId) override;
    std::vector<InstanceInfo> lookup(const std::string& service) override;
    void watch(const std::string& service, WatchCallback cb) override;

private:
    void watchLoop(const std::string& service, WatchCallback cb);

    std::unique_ptr<RpcClient> rpc_;
    std::shared_ptr<RpcChannel> channel_;
    std::atomic<bool> running_{true};
};

#endif  // MYRPCPROJECT_INCLUDE_REGISTRYCLIENT_H_
