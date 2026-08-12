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

void RpcClusterChannel::callAsync(RpcController& controller, const Value& request,
                                  Value* response, Done done) {
    attemptOnce(controller, request, response, std::move(done), 1);
}

void RpcClusterChannel::attemptOnce(RpcController& controller, const Value& request,
                                    Value* response, Done done, uint32_t attempt) {
    std::vector<InstanceState> states;
    std::vector<std::shared_ptr<RpcChannel>> channels;
    std::vector<std::shared_ptr<CircuitBreaker>> breakers;
    std::shared_ptr<LoadBalancer> lb;
    RetryOptions options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& inst : instances_) {
            InstanceState st;
            st.healthy = inst.channel->isHealthy() && inst.breaker->isAvailable();
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

    auto maybeRetry = [this, &controller, &request, response, done = std::move(done),
                       attempt, options](bool success) mutable {
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
                         attempt, backoff]() mutable {
                std::this_thread::sleep_for(backoff);
                self->attemptOnce(controller, request, response, std::move(done), attempt + 1);
            }).detach();
            return;  // done is deferred until the retry resolves
        }
        done();
    };

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
