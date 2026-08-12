// Governance demo & acceptance harness (V2.1):
//   1. cluster channel over N server instances with p2c load balancing;
//   2. instance failure: killing one server must route traffic to the
//      survivors without error storms.
//
// Usage: ./rpc_governance_demo host1 port1 [host2 port2 ...]
//   e.g. ./rpc_governance_demo 127.0.0.1 12345 127.0.0.1 12346
#include "LoadBalancer.h"
#include "Logger.h"
#include "RpcClient.h"
#include "RpcClusterChannel.h"
#include "Serializer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

Value makeEchoRequest(int n) {
    Value::Struct fields;
    fields.push_back({1, std::make_shared<Value>(Value::makeString("gov-" + std::to_string(n)))});
    return Value::makeStruct(std::move(fields));
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || (argc - 1) % 2 != 0) {
        std::printf("usage: %s host1 port1 [host2 port2 ...]\n", argv[0]);
        return 2;
    }

    const int total = (argc - 1) / 2;
    RpcClient client;
    auto cluster = client.createClusterChannel();
    for (int i = 0; i < total; ++i) {
        cluster->addInstance(argv[1 + i * 2], static_cast<uint16_t>(std::atoi(argv[2 + i * 2])));
        std::printf("instance: %s:%s\n", argv[1 + i * 2], argv[2 + i * 2]);
    }
    cluster->setLoadBalancer(std::make_shared<P2cBalancer>());

    // Phase 1: 400 concurrent echo calls distributed over the instances.
    const int requests = 400;
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    {
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(requests));
        for (int t = 0; t < requests; ++t) {
            workers.emplace_back([&, t]() {
                RpcController ctrl;
                ctrl.setMethod("demo", "echo");
                ctrl.setTimeoutMs(3000);
                Value resp;
                if (cluster->call(ctrl, makeEchoRequest(t), &resp)) {
                    ok.fetch_add(1);
                } else {
                    fail.fetch_add(1);
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
    }
    std::printf("phase1: requests=%d ok=%d fail=%d\n", requests, ok.load(), fail.load());
    if (ok.load() != requests) {
        std::printf("PHASE1 FAILED\n");
        return 1;
    }

    // Phase 2: after killing the first instance, all traffic must go to the
    // survivors; the caller waits out the cluster's reconnect attempts (the
    // demo prints this so the harness can kill an instance here).
    std::printf("READY_FOR_KILL\n");
    std::fflush(stdout);

    std::this_thread::sleep_for(std::chrono::seconds(2));  // harness kills an instance now

    ok.store(0);
    fail.store(0);
    for (int t = 0; t < 100; ++t) {
        RpcController ctrl;
        ctrl.setMethod("demo", "echo");
        ctrl.setTimeoutMs(3000);
        Value resp;
        if (cluster->call(ctrl, makeEchoRequest(t), &resp)) {
            ok.fetch_add(1);
        } else {
            fail.fetch_add(1);
        }
    }
    std::printf("phase2: ok=%d fail=%d\n", ok.load(), fail.load());
    client.stop();
    if (ok.load() == 100 && fail.load() == 0) {
        std::printf("GOVERNANCE DEMO PASSED\n");
        return 0;
    }
    std::printf("GOVERNANCE DEMO FAILED\n");
    return 1;
}
