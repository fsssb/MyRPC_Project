// Cross-process discovery demo (V2.1 extension): a cluster that learns its
// instances from a rpc_registry_server through RegistryClient, then keeps
// calling them while the harness kills/restores instances.
//
// Usage: ./rpc_registry_client_demo <registry_host> <registry_port> [service]
// The demo prints READY once discovery settles, then loops echo calls; the
// acceptance harness kills an instance and expects zero failures afterwards.
#include "LoadBalancer.h"
#include "Logger.h"
#include "RegistryClient.h"
#include "RpcClient.h"
#include "RpcClusterChannel.h"
#include "Serializer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

Value makeEchoRequest(int n) {
    Value::Struct f;
    f.push_back({1, std::make_shared<Value>(Value::makeString("rpc-" + std::to_string(n)))});
    return Value::makeStruct(std::move(f));
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::printf("usage: %s <registry_host> <registry_port> [service]\n", argv[0]);
        return 2;
    }
    const std::string registryHost = argv[1];
    const uint16_t registryPort = static_cast<uint16_t>(std::atoi(argv[2]));
    const std::string service = argc > 3 ? argv[3] : "demo";

    RegistryClient registry(registryHost, registryPort);
    RpcClient rpc;
    auto cluster = rpc.createClusterChannel();
    cluster->setLoadBalancer(std::make_shared<P2cBalancer>());
    cluster->setDiscovery(&registry, service);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::printf("discovered=%zu\n", cluster->instanceCount());
    std::printf("READY\n");
    std::fflush(stdout);

    // Keep calling; the harness kills / restores instances while we run.
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    int round = 0;
    while (true) {
        RpcController ctrl;
        ctrl.setMethod("demo", "echo");
        ctrl.setTimeoutMs(3000);
        Value resp;
        if (cluster->call(ctrl, makeEchoRequest(round++), &resp)) {
            ok.fetch_add(1);
        } else {
            fail.fetch_add(1);
        }
        if (round % 50 == 0) {
            std::printf("round=%d ok=%d fail=%d instances=%zu\n", round, ok.load(),
                        fail.load(), cluster->instanceCount());
            std::fflush(stdout);
        }
        if (round % 200 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));  // keep the run alive for the harness
        }
        if (round > 4000) {
            break;  // bounded demo run
        }
    }
    std::printf("FINAL ok=%d fail=%d\n", ok.load(), fail.load());
    rpc.stop();
    return fail.load() == 0 ? 0 : 1;
}
