#include "LocalRegistry.h"

#include <algorithm>
#include <chrono>

LocalRegistry::LocalRegistry() {
    sweeper_ = std::thread([this]() { sweeperLoop(); });
}

LocalRegistry::~LocalRegistry() {
    running_ = false;
    if (sweeper_.joinable()) {
        sweeper_.join();
    }
}

void LocalRegistry::registerService(const InstanceInfo& info, uint32_t leaseMs) {
    std::vector<WatchCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry;
        entry.info = info;
        entry.leaseMs = leaseMs > 0 ? leaseMs : 10000;
        entry.leaseDeadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(entry.leaseMs);
        auto& state = services_[info.service];
        state.instances[info.instanceId] = std::move(entry);
        state.version++;
        callbacks.swap(state.watchers);  // one-shot
    }
    for (auto& cb : callbacks) {
        if (cb) {
            cb();  // outside the registry lock; callers may re-enter lookup()
        }
    }
}

void LocalRegistry::unregister(const std::string& service, uint64_t instanceId) {
    std::vector<WatchCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(service);
        if (it == services_.end()) {
            return;
        }
        it->second.instances.erase(instanceId);
        it->second.version++;
        callbacks.swap(it->second.watchers);
    }
    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

void LocalRegistry::renewLease(const std::string& service, uint64_t instanceId) {
    std::vector<WatchCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(service);
        if (it != services_.end()) {
            auto& state = it->second;
            callbacks = sweepExpiredLocked(&state);
            auto inst = state.instances.find(instanceId);
            if (inst != state.instances.end()) {
                inst->second.leaseDeadline = std::chrono::steady_clock::now() +
                                             std::chrono::milliseconds(inst->second.leaseMs);
            }
            // Unknown instance ids are ignored, but expired-instance callbacks
            // from the sweep must still run (do not early-return past them).
        }
    }
    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

std::vector<InstanceInfo> LocalRegistry::lookup(const std::string& service) {
    std::vector<WatchCallback> callbacks;
    std::vector<InstanceInfo> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(service);
        if (it == services_.end()) {
            return {};
        }
        auto& state = it->second;
        callbacks = sweepExpiredLocked(&state);
        out.reserve(state.instances.size());
        for (const auto& kv : state.instances) {
            out.push_back(kv.second.info);
        }
    }
    for (auto& cb : callbacks) {
        if (cb) {
            cb();  // expired instances changed the set; notify watchers
        }
    }
    return out;
}

void LocalRegistry::watch(const std::string& service, WatchCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    services_[service].watchers.push_back(std::move(cb));
}

std::size_t LocalRegistry::instanceCount(const std::string& service) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(service);
    if (it == services_.end()) {
        return 0;
    }
    return it->second.instances.size();
}

// Sweeps expired leases; returns the callbacks to invoke after the lock is
// released. Assumes mutex_ is held.
std::vector<LocalRegistry::WatchCallback> LocalRegistry::sweepExpiredLocked(ServiceState* state) {
    std::vector<WatchCallback> callbacks;
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;
    for (auto it = state->instances.begin(); it != state->instances.end();) {
        if (now >= it->second.leaseDeadline) {
            it = state->instances.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        state->version++;
        callbacks.swap(state->watchers);
    }
    return callbacks;
}

void LocalRegistry::sweeperLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::vector<WatchCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& kv : services_) {
                auto cbs = sweepExpiredLocked(&kv.second);
                callbacks.insert(callbacks.end(), cbs.begin(), cbs.end());
            }
        }
        for (auto& cb : callbacks) {
            if (cb) {
                cb();
            }
        }
    }
}
