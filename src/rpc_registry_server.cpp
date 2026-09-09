// Cross-process registry service (V2.1 extension): exposes the LocalRegistry
// semantics over RPC so service instances in separate processes can share one
// registry.
//
// Methods (see include/RegistryProtocol.h):
//   registry.register   {service,host,port,instanceId,leaseMs} -> ok
//   registry.unregister {service,instanceId}
//   registry.renew      {service,instanceId}
//   registry.lookup     {service} -> {instances:[{service,host,port,instanceId}]}
//   registry.watch      {service} -> ok  (long-poll: held until the set changes)
//
// Usage: ./rpc_registry_server [port]   (default 18000)
#include "EventLoop.h"
#include "LocalRegistry.h"
#include "Logger.h"
#include "RegistryProtocol.h"
#include "Router.h"
#include "RpcServer.h"
#include "Serializer.h"

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>

namespace {

const Value* str(const Value& req, uint32_t field) {
    const Value* v = req.find(field);
    return (v != nullptr && v->type() == Value::Type::String) ? v : nullptr;
}
const Value* num(const Value& req, uint32_t field) {
    const Value* v = req.find(field);
    return (v != nullptr && v->type() == Value::Type::Uint64) ? v : nullptr;
}

Value instanceValue(const InstanceInfo& info) {
    Value::Struct f;
    f.push_back({registry_proto::kFService,
                 std::make_shared<Value>(Value::makeString(info.service))});
    f.push_back({registry_proto::kFHost, std::make_shared<Value>(Value::makeString(info.host))});
    f.push_back({registry_proto::kFPort, std::make_shared<Value>(Value::makeUint(info.port))});
    f.push_back({registry_proto::kFInstanceId,
                 std::make_shared<Value>(Value::makeUint(info.instanceId))});
    return Value::makeStruct(std::move(f));
}

}  // namespace

int main(int argc, char* argv[]) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 18000;
    LocalRegistry registry;  // in-memory store; leases + one-shot watches
    EventLoop loop;
    RpcServer server(&loop, "0.0.0.0", port, 2);

    server.registerMethod("registry", "register",
                          [&registry](const Value& req, Value* resp, Router::Done done) {
        const Value* service = str(req, registry_proto::kFService);
        const Value* host = str(req, registry_proto::kFHost);
        const Value* p = num(req, registry_proto::kFPort);
        const Value* id = num(req, registry_proto::kFInstanceId);
        const Value* lease = num(req, registry_proto::kFLeaseMs);
        if (service == nullptr || host == nullptr || p == nullptr || id == nullptr) {
            done(proto::kInvalidArgument);
            return;
        }
        registry.registerService({service->asString(), host->asString(),
                                  static_cast<uint16_t>(p->asUint64()), id->asUint64()},
                                 static_cast<uint32_t>(lease ? lease->asUint64() : 10000));
        done(proto::kOk);
    });

    server.registerMethod("registry", "unregister",
                          [&registry](const Value& req, Value*, Router::Done done) {
        const Value* service = str(req, registry_proto::kFService);
        const Value* id = num(req, registry_proto::kFInstanceId);
        if (service == nullptr || id == nullptr) {
            done(proto::kInvalidArgument);
            return;
        }
        registry.unregister(service->asString(), id->asUint64());
        done(proto::kOk);
    });

    server.registerMethod("registry", "renew",
                          [&registry](const Value& req, Value*, Router::Done done) {
        const Value* service = str(req, registry_proto::kFService);
        const Value* id = num(req, registry_proto::kFInstanceId);
        if (service == nullptr || id == nullptr) {
            done(proto::kInvalidArgument);
            return;
        }
        registry.renewLease(service->asString(), id->asUint64());
        done(proto::kOk);
    });

    server.registerMethod("registry", "lookup",
                          [&registry](const Value& req, Value* resp, Router::Done done) {
        const Value* service = str(req, registry_proto::kFService);
        if (service == nullptr) {
            done(proto::kInvalidArgument);
            return;
        }
        Value::Array arr;
        for (const auto& info : registry.lookup(service->asString())) {
            arr.push_back(instanceValue(info));
        }
        Value::Struct f;
        f.push_back({registry_proto::kFInstances,
                     std::make_shared<Value>(Value::makeArray(std::move(arr)))});
        *resp = Value::makeStruct(std::move(f));
        done(proto::kOk);
    });

    server.registerMethod("registry", "watch",
                          [&registry](const Value& req, Value*, Router::Done done) {
        const Value* service = str(req, registry_proto::kFService);
        if (service == nullptr) {
            done(proto::kInvalidArgument);
            return;
        }
        // Long-poll: hold the request until the service set changes (one-shot
        // watch semantics from LocalRegistry), then complete it.
        registry.watch(service->asString(), [done]() { done(proto::kOk); });
    });

    server.start();
    LOG_INFO("registry server on port " + std::to_string(port));
    loop.loop();
    return 0;
}
