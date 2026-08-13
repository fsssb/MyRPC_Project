#include "RpcClusterChannel.h"

#include <condition_variable>
#include <mutex>
#include <thread>

RpcClusterChannel::RpcClusterChannel(EventLoop* loop) : loop_(loop) {}

void RpcClusterChannel::addInstance(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& inst : instances_) {
        if (inst.host == host && inst.port == port) {
            return;  // already present
        }
    }
    Instance inst;
    inst.host = host;
    inst.port = port;
    inst.channel = std::make_shared<RpcChannel>(loop_, host, port);
    inst.channel->setHeartbeatIntervalMs(5000);
    inst.channel->start();
    inst.breaker = std::make_shared<CircuitBreaker>();
    instances_.push_back(std::move(inst));
}

void RpcClusterChannel::removeInstance(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    instances_.erase(
        std::remove_if(instances_.begin(), instances_.end(),
                       [&](const Instance& inst) {
                           return inst.host == host && inst.port == port;
                       }),
        instances_.end());
}

std::size_t RpcClusterChannel::instanceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instances_.size();
}

void RpcClusterChannel::setLoadBalancer(std::shared_ptr<LoadBalancer> lb) {
    std::lock_guard<std::mutex> lock(mutex_);
    lb_ = std::move(lb);
}

void RpcClusterChannel::setRetryOptions(const RetryOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    retryOptions_ = options;
    retryBudget_.reset(options.budgetTokens);
}

void RpcClusterChannel::setDiscovery(Registry* registry, const std::string& service) {
    discoveryRegistry_ = registry;
    discoveryService_ = service;
    applyInstances(registry->lookup(service));
    resubscribeWatch();
}

void RpcClusterChannel::resubscribeWatch() {
    if (discoveryRegistry_ == nullptr) {
        return;
    }
    auto self = shared_from_this();
    discoveryRegistry_->watch(discoveryService_, [self]() {
        self->applyInstances(self->discoveryRegistry_->lookup(self->discoveryService_));
        self->resubscribeWatch();  // one-shot watch: subscribe again
    });
}

void RpcClusterChannel::applyInstances(const std::vector<InstanceInfo>& infos) {
    std::vector<std::shared_ptr<RpcChannel>> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Instance> next;
        next.reserve(infos.size());
        for (const auto& info : infos) {
            auto it = std::find_if(instances_.begin(), instances_.end(),
                                   [&](const Instance& inst) {
                                       return inst.host == info.host && inst.port == info.port;
                                   });
            if (it != instances_.end()) {
                next.push_back(*it);  // reuse channel + breaker (copy: shared_ptr)
            } else {
                Instance inst;
                inst.host = info.host;
                inst.port = info.port;
                inst.channel = std::make_shared<RpcChannel>(loop_, info.host, info.port);
                inst.channel->setHeartbeatIntervalMs(5000);
                inst.channel->start();
                inst.breaker = std::make_shared<CircuitBreaker>();
                next.push_back(std::move(inst));
            }
        }
        // Instances that disappeared are stopped outside the lock.
        for (auto& inst : instances_) {
            const bool keep = std::any_of(infos.begin(), infos.end(),
                                          [&](const InstanceInfo& info) {
                                              return info.host == inst.host && info.port == inst.port;
                                          });
            if (!keep) {
                removed.push_back(inst.channel);
            }
        }
        instances_.swap(next);
    }
    for (auto& ch : removed) {
        ch->stop();
    }
}

void RpcClusterChannel::callAsync(RpcController& controller, const Value& request,
                                  Value* response, Done done) {
    // A call may fan out (retries, hedged backups); the completion flag makes
    // the first finisher win and drops the others.
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto sharedDone = [completed, done = std::move(done)]() mutable {
        if (!completed->exchange(true, std::memory_order_relaxed)) {
            done();
        }
    };
    attemptOnce(controller, request, response, std::move(sharedDone), 1, completed,
                static_cast<std::size_t>(-1), true);
}

void RpcClusterChannel::attemptOnce(RpcController& controller, const Value& request,
                                    Value* response, Done done, uint32_t attempt,
                                    const std::shared_ptr<std::atomic<bool>>& completed,
                                    std::size_t excludeIndex, bool hedge) {
    std::vector<InstanceState> states;
    std::vector<std::shared_ptr<RpcChannel>> channels;
    std::vector<std::shared_ptr<CircuitBreaker>> breakers;
    std::shared_ptr<LoadBalancer> lb;
    RetryOptions options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < instances_.size(); ++i) {
            const auto& inst = instances_[i];
            InstanceState st;
            const bool excluded = (i == excludeIndex);
            st.healthy = !excluded && inst.channel->isHealthy() && inst.breaker->isAvailable();
            st.inflight = inst.channel->inflightCount();
            st.latencyEmaMs = inst.channel->latencyEmaMs();
            states.push_back(st);
            channels.push_back(inst.channel);
            breakers.push_back(inst.breaker);
        }
        lb = lb_;
        options = retryOptions_;
    }

    std::size_t idx = channels.size();
    if (lb && !states.empty()) {
        idx = lb->pick(states, controller.requestKey());
    } else if (!states.empty()) {
        idx = 0;
        for (std::size_t i = 0; i < states.size(); ++i) {
            if (states[i].healthy) {
                idx = i;
                break;
            }
        }
    }

    // A copy of the completion callback for the hedge timer (maybeRetry below
    // moves the parameter, but the backup must be able to finish the call too).
    Done hedgeDone = done;

    auto maybeRetry = [this, &controller, &request, response, done = std::move(done),
                       attempt, options, completed](bool success) mutable {
        if (success) {
            retryBudget_.refund();
            done();
            return;
        }
        const auto st = controller.status();
        if (retry::shouldRetry(st, controller.idempotent(), attempt, options.maxAttempts) &&
            retryBudget_.tryConsume()) {
            retries_.fetch_add(1, std::memory_order_relaxed);
            const auto backoff = retry::jitterBackoff(attempt - 1, options.baseBackoffMs,
                                                      options.maxBackoffMs);
            auto self = shared_from_this();
            std::thread([self, &controller, &request, response, done = std::move(done),
                         attempt, backoff, completed]() mutable {
                std::this_thread::sleep_for(backoff);
                self->attemptOnce(controller, request, response, std::move(done), attempt + 1,
                                  completed, static_cast<std::size_t>(-1), false);
            }).detach();
            return;  // done is deferred until the retry resolves
        }
        done();
    };

    // Hedging: after hedgeAfterMs the first attempt is still pending, send a
    // backup to another instance; the first finisher wins (completed flag).
    // Backups never hedge again, and only the caller's first attempt may hedge.
    if (hedge && options.hedgeAfterMs > 0 && attempt == 1 && controller.idempotent() &&
        idx < channels.size()) {
        const std::size_t firstIdx = idx;
        auto self = shared_from_this();
        loop_->runAfter(std::chrono::milliseconds(options.hedgeAfterMs),
                        [self, &controller, &request, response, hedgeDone, completed,
                         firstIdx]() mutable {
                            if (completed->load(std::memory_order_relaxed)) {
                                return;  // primary already finished
                            }
                            self->attemptOnce(controller, request, response,
                                              std::move(hedgeDone), 1, completed, firstIdx, false);
                        });
    }

    if (idx >= channels.size() || !states[idx].healthy) {
        // All instances are unavailable: fail fast (retry may kick in).
        failCall(&controller, proto::kUnknown, "no available instance",
                 [this, maybeRetry]() mutable { maybeRetry(false); });
        return;
    }

    auto breaker = breakers[idx];
    if (!breaker->allowRequest()) {
        failCall(&controller, proto::kConcurrencyLimited, "circuit probe slot busy",
                 [this, maybeRetry]() mutable { maybeRetry(false); });
        return;
    }

    auto wrappedDone = [this, &controller, breaker, maybeRetry]() mutable {
        const auto st = controller.status();
        if (st == proto::kOk) {
            breaker->onSuccess();
            maybeRetry(true);
        } else if (st != proto::kUnknown) {
            breaker->onError();
            maybeRetry(false);
        } else {
            // connection-class failure; breaker is skipped (isHealthy covers it)
            maybeRetry(false);
        }
    };
    channels[idx]->callAsync(controller, request, response, std::move(wrappedDone));
}

bool RpcClusterChannel::call(RpcController& controller, const Value& request, Value* response) {
    std::mutex m;
    std::condition_variable cv;
    bool doneCalled = false;
    const auto notify = [&]() {
        std::lock_guard<std::mutex> lk(m);
        doneCalled = true;
        cv.notify_all();
    };
    callAsync(controller, request, response, notify);

    std::unique_lock<std::mutex> lk(m);
    const uint32_t waitMs = controller.timeoutMs() > 0 ? controller.timeoutMs() + 2000 : 15000;
    cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&]() { return doneCalled; });
    if (!doneCalled) {
        // The selected channel's deadline timer eventually completes the call;
        // wait another window (retries with backoff may extend the duration).
        cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&]() { return doneCalled; });
    }
    return controller.status() == proto::kOk;
}

void RpcClusterChannel::failCall(RpcController* controller, proto::Status status,
                                 const std::string& text, Done done) {
    controller->setStatus(status, text);
    if (done) {
        done();
    }
}
