#include "AIService.h"
#include "EventLoop.h"
#include "Logger.h"
#include "Metrics.h"
#include "RpcServer.h"
#include "Scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <string>
#include <thread>

namespace {
std::atomic<bool> gStopRequested {false};

void handleStopSignal(int /*sig*/) {
    gStopRequested.store(true);
}
}  // namespace

int main(int argc, char* argv[]) {
    int threadNum = 4;
    uint16_t port = 12345;
    if (argc > 1) {
        threadNum = std::atoi(argv[1]);
        if (threadNum < 0) {
            threadNum = 4;
        }
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);

    EventLoop mainLoop;
    const std::size_t aiWorkers = static_cast<std::size_t>(threadNum <= 0 ? 8 : threadNum * 4);
    AIService aiService(aiWorkers);
    // V2.2: run handlers on the M:N coroutine scheduler so a blocking
    // handler (demo.slow) yields its worker instead of occupying a thread.
    Scheduler scheduler;
    scheduler.start(static_cast<std::size_t>(threadNum <= 0 ? 4 : threadNum));

    RpcServer server(&mainLoop, "0.0.0.0", port, threadNum);
    server.setExecutor([&scheduler](std::function<void()> task) {
        scheduler.spawn(std::move(task));
    });
    if (argc > 3) {
        const std::size_t maxConcurrency = static_cast<std::size_t>(std::atoi(argv[3]));
        if (maxConcurrency > 0) {
            server.setMaxConcurrency(maxConcurrency);
        }
    }
    if (argc > 4) {
        const std::size_t highWaterKB = static_cast<std::size_t>(std::atoi(argv[4]));
        if (highWaterKB > 0) {
            server.setWriteHighWaterMark(highWaterKB * 1024);
            server.setSlowConsumerTimeout(std::chrono::milliseconds(500));
        }
    }

    // demo.echo: reply with the exact request struct.
    server.registerMethod("demo", "echo", [](const Value& request, Value* response,
                                             Router::Done done) {
        *response = request;
        done(proto::kOk);
    });

    // demo.ai: simulated long-running AI task; the handler completes
    // asynchronously, exercising the cross-thread write-back path.
    server.registerMethod("demo", "ai", [&aiService](const Value& request, Value* response,
                                                     Router::Done done) {
        std::string prompt;
        if (const Value* field = request.find(1)) {
            if (field->type() == Value::Type::String) {
                prompt = field->asString();
            }
        }
        aiService.inferAsync(std::move(prompt), [response, done](std::string result) mutable {
            *response = Value::makeString(std::move(result));
            done(proto::kOk);
        });
    });

    // demo.slow: 2s task that yields its coroutine (does not occupy a worker
    // thread), used to exercise graceful shutdown draining.
    server.registerMethod("demo", "slow", [](const Value& /*request*/, Value* response,
                                             Router::Done done) {
        Scheduler::coSleep(std::chrono::seconds(2));
        *response = Value::makeString("slow-done");
        done(proto::kOk);
    });

    server.start();

    mainLoop.runEvery(std::chrono::seconds(1), [&server]() {
        Metrics::instance().setTaskQueueDepth(server.pendingTaskSize());
        LOG_INFO("metrics\n" + Metrics::instance().renderPrometheusAndRotate());
    });

    mainLoop.runEvery(std::chrono::milliseconds(200), [&mainLoop, &server]() {
        if (gStopRequested.load()) {
            LOG_INFO("stop signal received, graceful shutdown start");
            server.stopGracefully(std::chrono::seconds(10),
                                  [&mainLoop]() { mainLoop.quit(); });
        }
    });

    LOG_INFO("RPC server started on 0.0.0.0:" + std::to_string(port) +
             ", io_threads=" + std::to_string(threadNum) +
             ", ai_workers=" + std::to_string(aiWorkers));
    mainLoop.loop();
    scheduler.stop();
    aiService.stop();
    return 0;
}
