#include "RpcClient.h"

#include "EventLoop.h"
#include "RpcChannel.h"

#include <future>

// The EventLoop must be constructed on the thread that runs loop(): V1's
// threadId_ (recorded at construction) is what isInLoopThread() compares
// against, so building the loop on the caller thread but running it elsewhere
// breaks every runInLoop/queueInLoop decision. RpcClient therefore creates the
// loop inside the loop thread and waits until it is ready.
RpcClient::RpcClient() {
    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    running_.store(true);
    loopThread_ = std::thread([this, &ready]() {
        loopOwner_ = std::make_unique<EventLoop>();
        loop_ = loopOwner_.get();
        ready.set_value();
        loop_->loop();
    });
    readyFuture.wait();
}

RpcClient::~RpcClient() {
    stop();
}

std::shared_ptr<RpcChannel> RpcClient::connect(const std::string& host, uint16_t port) {
    auto channel = std::make_shared<RpcChannel>(loop_, host, port);
    channel->setHeartbeatIntervalMs(5000);  // 5s < server idle cleanup (30s)
    channel->start();
    return channel;
}

void RpcClient::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    loop_->quit();
    if (loopThread_.joinable()) {
        loopThread_.join();
    }
}
