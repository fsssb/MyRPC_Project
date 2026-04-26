#include "AIService.h"
#include "EventLoop.h"
#include "Logger.h"
#include "Message.h"
#include "Metrics.h"
#include "TcpServer.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <string>

namespace {
std::atomic<bool> gStopRequested {false};

void handleStopSignal(int /*sig*/) {
    gStopRequested.store(true);
}
}  // namespace

int main(int argc, char* argv[]) {
    int threadNum = 4;
    if (argc > 1) {
        threadNum = std::atoi(argv[1]);
        if (threadNum < 0) {
            threadNum = 4;
        }
    }

    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);

    EventLoop mainLoop;
    const std::size_t aiWorkers = static_cast<std::size_t>(threadNum <= 0 ? 8 : threadNum * 4);
    AIService aiService(aiWorkers);
    TcpServer server(&mainLoop, "0.0.0.0", 12345, threadNum);
    server.setMessageCallback([&aiService, &server](const std::shared_ptr<TcpConnection>& conn, const Message& req) {
        Metrics::instance().markRequest();
        const auto weakConn = std::weak_ptr<TcpConnection>(conn);
        auto prompt = req.body;
        server.submitTask([&aiService, weakConn, prompt = std::move(prompt)]() mutable {
            aiService.inferAsync(std::move(prompt), [weakConn](std::string respText) {
                if (const auto locked = weakConn.lock()) {
                    Message resp;
                    resp.body = std::move(respText);
                    locked->sendMessage(resp);
                }
            });
        });
    });
    server.start();

    mainLoop.runEvery(std::chrono::seconds(1), [&server]() {
        Metrics::instance().setTaskQueueDepth(server.pendingTaskSize());
        LOG_INFO("metrics\n" + Metrics::instance().renderPrometheusAndRotate());
    });

    mainLoop.runEvery(std::chrono::milliseconds(200), [&mainLoop, &server]() {
        if (gStopRequested.load()) {
            LOG_INFO("stop signal received, graceful shutdown start");
            server.stop();
            mainLoop.quit();
        }
    });

    LOG_INFO("AI gateway started on 0.0.0.0:12345, worker threads=" + std::to_string(threadNum) +
             ", ai_workers=" + std::to_string(aiWorkers));
    mainLoop.loop();
    aiService.stop();
    return 0;
}
