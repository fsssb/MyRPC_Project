#include "RpcClusterChannel.h"

#include <condition_variable>
#include <mutex>

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

void RpcClusterChannel::callAsync(RpcController& controller, const Value& request,
                                  Value* response, Done done) {
    std::vector<InstanceState> states;
    std::vector<std::shared_ptr<RpcChannel>> channels;
    std::shared_ptr<LoadBalancer> lb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& inst : instances_) {
            InstanceState st;
            st.healthy = inst.channel->isHealthy();
            st.inflight = inst.channel->inflightCount();
            st.latencyEmaMs = inst.channel->latencyEmaMs();
            states.push_back(st);
            channels.push_back(inst.channel);
        }
        lb = lb_;
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

    if (idx >= channels.size() || !states[idx].healthy) {
        failCall(&controller, proto::kUnknown, "no healthy instance available", std::move(done));
        return;
    }
    channels[idx]->callAsync(controller, request, response, std::move(done));
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
    const uint32_t waitMs = controller.timeoutMs() > 0 ? controller.timeoutMs() + 500 : 10000;
    cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&]() { return doneCalled; });
    if (!doneCalled) {
        // The selected channel's deadline timer eventually completes the call;
        // wait a second window so the done callback (and its stack writes)
        // always run before we return.
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
