// Client demo and acceptance harness (stage 3):
//   1. N concurrent echo calls multiplexed over one connection, responses are
//      matched by request_id (may arrive out of order);
//   2. client-side deadline: the ai handler takes ~250ms, a 100ms timeout must
//      fail the call with kDeadlineExceeded;
//   3. the connection must remain usable after the timeout (late response is
//      dropped by request_id).
//
// Usage: ./rpc_client_demo [concurrency]   (default 50)
#include "Logger.h"
#include "RpcChannel.h"
#include "RpcClient.h"
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
    fields.push_back({1, std::make_shared<Value>(Value::makeString("hello-" + std::to_string(n)))});
    return Value::makeStruct(std::move(fields));
}

}  // namespace

int main(int argc, char* argv[]) {
    const int concurrency = argc > 1 ? std::atoi(argv[1]) : 50;

    RpcClient client;
    auto channel = client.connect("127.0.0.1", 12345);

    // 1) concurrent multiplexing check.
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    {
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(concurrency));
        for (int t = 0; t < concurrency; ++t) {
            workers.emplace_back([&, t]() {
                RpcController ctrl;
                ctrl.setMethod("demo", "echo");
                ctrl.setTimeoutMs(3000);
                Value resp;
                if (channel->call(ctrl, makeEchoRequest(t), &resp)) {
                    const Value* s = resp.find(1);
                    if (s && s->type() == Value::Type::String &&
                        s->asString() == "hello-" + std::to_string(t)) {
                        ok.fetch_add(1);
                        return;
                    }
                }
                fail.fetch_add(1);
            });
        }
        for (auto& w : workers) {
            w.join();
        }
    }
    std::printf("concurrency=%d ok=%d fail=%d\n", concurrency, ok.load(), fail.load());
    if (ok.load() != concurrency || fail.load() != 0) {
        std::printf("MULTIPLEX FAILED\n");
        client.stop();
        return 1;
    }

    // 2) client-side deadline: ai handler sleeps ~250ms, timeout is 100ms.
    {
        RpcController ctrl;
        ctrl.setMethod("demo", "ai");
        ctrl.setTimeoutMs(100);
        Value resp;
        const bool r = channel->call(ctrl, makeEchoRequest(999), &resp);
        std::printf("timeout_call returned=%d status=%u (expect 0 / %u)\n", r,
                    static_cast<unsigned>(ctrl.status()), static_cast<unsigned>(proto::kDeadlineExceeded));
        if (r || ctrl.status() != proto::kDeadlineExceeded) {
            std::printf("TIMEOUT FAILED\n");
            client.stop();
            return 1;
        }
    }

    // 3) the connection must survive the timeout; the late response is dropped.
    {
        RpcController ctrl;
        ctrl.setMethod("demo", "echo");
        ctrl.setTimeoutMs(2000);
        Value resp;
        if (!channel->call(ctrl, makeEchoRequest(7), &resp)) {
            std::printf("REUSE FAILED\n");
            client.stop();
            return 1;
        }
        std::printf("connection reused after timeout: ok\n");
    }

    std::printf("CLIENT DEMO PASSED\n");
    channel->stop();
    client.stop();
    return 0;
}
