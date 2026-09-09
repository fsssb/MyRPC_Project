#include "RegistryClient.h"

#include "Logger.h"
#include "RegistryProtocol.h"
#include "RpcChannel.h"
#include "RpcClient.h"
#include "RpcController.h"
#include "Serializer.h"

#include <chrono>
#include <memory>

namespace {

Value makeRequest(uint32_t field, const std::string& value) {
    Value::Struct f;
    f.push_back({field, std::make_shared<Value>(Value::makeString(value))});
    return Value::makeStruct(std::move(f));
}

}  // namespace

RegistryClient::RegistryClient(std::string host, uint16_t port)
    : rpc_(std::make_unique<RpcClient>()) {
    channel_ = rpc_->connect(std::move(host), port);
    channel_->setConnectTimeoutMs(1000);
}

RegistryClient::~RegistryClient() {
    running_.store(false);
    if (channel_) {
        channel_->stop();
    }
    if (rpc_) {
        rpc_->stop();
    }
}

void RegistryClient::registerService(const InstanceInfo& info, uint32_t leaseMs) {
    Value::Struct f;
    f.push_back({registry_proto::kFService,
                 std::make_shared<Value>(Value::makeString(info.service))});
    f.push_back({registry_proto::kFHost, std::make_shared<Value>(Value::makeString(info.host))});
    f.push_back({registry_proto::kFPort, std::make_shared<Value>(Value::makeUint(info.port))});
    f.push_back({registry_proto::kFInstanceId,
                 std::make_shared<Value>(Value::makeUint(info.instanceId))});
    f.push_back({registry_proto::kFLeaseMs, std::make_shared<Value>(Value::makeUint(leaseMs))});
    RpcController ctrl;
    ctrl.setMethodKey(registry_proto::kRegister);
    ctrl.setTimeoutMs(3000);
    Value resp;
    channel_->call(ctrl, Value::makeStruct(std::move(f)), &resp);
}

void RegistryClient::unregister(const std::string& service, uint64_t instanceId) {
    Value::Struct f;
    f.push_back({registry_proto::kFService, std::make_shared<Value>(Value::makeString(service))});
    f.push_back({registry_proto::kFInstanceId, std::make_shared<Value>(Value::makeUint(instanceId))});
    RpcController ctrl;
    ctrl.setMethodKey(registry_proto::kUnregister);
    ctrl.setTimeoutMs(3000);
    Value resp;
    channel_->call(ctrl, Value::makeStruct(std::move(f)), &resp);
}

void RegistryClient::renewLease(const std::string& service, uint64_t instanceId) {
    Value::Struct f;
    f.push_back({registry_proto::kFService, std::make_shared<Value>(Value::makeString(service))});
    f.push_back({registry_proto::kFInstanceId, std::make_shared<Value>(Value::makeUint(instanceId))});
    RpcController ctrl;
    ctrl.setMethodKey(registry_proto::kRenew);
    ctrl.setTimeoutMs(2000);
    Value resp;
    channel_->call(ctrl, Value::makeStruct(std::move(f)), &resp);
}

std::vector<InstanceInfo> RegistryClient::lookup(const std::string& service) {
    RpcController ctrl;
    ctrl.setMethodKey(registry_proto::kLookup);
    ctrl.setTimeoutMs(3000);
    Value resp;
    channel_->call(ctrl, makeRequest(registry_proto::kFService, service), &resp);
    std::vector<InstanceInfo> out;
    const Value* arr = resp.find(registry_proto::kFInstances);
    if (arr == nullptr || arr->type() != Value::Type::Array) {
        return out;
    }
    for (const auto& elem : arr->asArray()) {
        const Value* s = elem.find(registry_proto::kFService);
        const Value* h = elem.find(registry_proto::kFHost);
        const Value* p = elem.find(registry_proto::kFPort);
        const Value* id = elem.find(registry_proto::kFInstanceId);
        if (s == nullptr || h == nullptr || p == nullptr || id == nullptr) {
            continue;
        }
        out.push_back({s->asString(), h->asString(),
                       static_cast<uint16_t>(p->asUint64()), id->asUint64()});
    }
    return out;
}

void RegistryClient::watch(const std::string& service, WatchCallback cb) {
    std::thread([this, service, cb = std::move(cb)]() { watchLoop(service, cb); }).detach();
}

void RegistryClient::watchLoop(const std::string& service, WatchCallback cb) {
    while (running_.load()) {
        RpcController ctrl;
        ctrl.setMethodKey(registry_proto::kWatch);
        ctrl.setTimeoutMs(60000);  // long-poll: the server holds the request
        Value resp;
        channel_->call(ctrl, makeRequest(registry_proto::kFService, service), &resp);
        if (ctrl.status() == proto::kOk && cb) {
            cb();  // service set changed
        }
        // Timeout (60s without change) or error: re-poll.
    }
}
