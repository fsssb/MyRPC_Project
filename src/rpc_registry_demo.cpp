// Registry demo & acceptance (V2.1-E): one process hosts a LocalRegistry, two
// server instances and a discovering client, exercising:
//   1. discovery: the cluster picks up both registered instances;
//   2. graceful removal: unregister triggers a watch and the client converges;
//   3. lease expiry: an instance that stops renewing disappears after its
//      lease (ephemeral semantics, ZooKeeper style).
//
// Usage: ./rpc_registry_demo
#include "LoadBalancer.h"
#include "LocalRegistry.h"
#include "Logger.h"
#include "RpcClient.h"
#include "RpcClusterChannel.h"
#include "RpcServer.h"
#include "Serializer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

void echoHandler(const Value& request, Value* response, Router::Done done) {
    *response = request;
    done(proto::kOk);
}

Value makeReq(int n) {
    Value::Struct f;
    f.push_back({1, std::make_shared<Value>(Value::makeString("reg-" + std::to_string(n)))});
    return Value::makeStruct(std::move(f));
}

int countOk(std::shared_ptr<RpcClusterChannel> cluster, int n) {
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        RpcController ctrl;
        ctrl.setMethod("demo", "echo");
        ctrl.setTimeoutMs(3000);
        Value resp;
        if (cluster->call(ctrl, makeReq(i), &resp)) {
            ++ok;
        }
    }
    return ok;
}

}  // namespace

int main() {
    LocalRegistry registry;

    // Server instances run on their own loop thread (the loop must be
    // constructed on the thread that runs it).
    std::thread serverThread([]() {
        EventLoop loop;
        RpcServer a(&loop, "0.0.0.0", 12345, 2);
        RpcServer b(&loop, "0.0.0.0", 12346, 2);
        a.registerMethod("demo", "echo", echoHandler);
        b.registerMethod("demo", "echo", echoHandler);
        a.start();
        b.start();
        loop.loop();
    });

    // Register both instances with a 3s lease.
    registry.registerService({"demo", "127.0.0.1", 12345, 1}, 3000);
    registry.registerService({"demo", "127.0.0.1", 12346, 2}, 3000);

    std::atomic<bool> running{true};
    std::atomic<bool> renewA{true};
    std::thread renewer([&]() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (renewA.load()) {
                registry.renewLease("demo", 1);
            }
            registry.renewLease("demo", 2);  // no-op after unregister
        }
    });

    RpcClient client;
    auto cluster = client.createClusterChannel();
    cluster->setLoadBalancer(std::make_shared<P2cBalancer>());
    cluster->setDiscovery(&registry, "demo");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    const std::size_t discovered = cluster->instanceCount();
    std::printf("discovered=%zu (expect 2)\n", discovered);

    const int ok1 = countOk(cluster, 100);
    std::printf("phase1: ok=%d/100 (expect 100)\n", ok1);

    // Graceful removal: unregister instance 2 -> watch -> converge to A.
    registry.unregister("demo", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::size_t afterUnregister = cluster->instanceCount();
    std::printf("after_unregister=%zu (expect 1)\n", afterUnregister);
    const int ok2 = countOk(cluster, 100);
    std::printf("phase2: ok=%d/100 (expect 100)\n", ok2);

    // Lease expiry: instance 1 stops renewing -> disappears after its 3s lease.
    renewA.store(false);
    std::this_thread::sleep_for(std::chrono::seconds(4));  // lease surely expired
    registry.lookup("demo");  // trigger lazy sweep + watcher notification
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::size_t afterExpire = cluster->instanceCount();
    std::printf("after_lease_expiry=%zu (expect 0)\n", afterExpire);
    const int ok3 = countOk(cluster, 20);
    std::printf("phase3: ok=%d/20 (expect 0, all fail fast)\n", ok3);

    running.store(false);
    renewer.join();

    const bool pass = discovered == 2 && ok1 == 100 && afterUnregister == 1 && ok2 == 100 &&
                      afterExpire == 0 && ok3 == 0;
    std::printf(pass ? "REGISTRY DEMO PASSED\n" : "REGISTRY DEMO FAILED\n");
    client.stop();
    serverThread.detach();  // server loop runs until the process exits
    return pass ? 0 : 1;
}
