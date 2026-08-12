#ifndef MYRPCPROJECT_INCLUDE_RPCCLIENT_H_
#define MYRPCPROJECT_INCLUDE_RPCCLIENT_H_

#include <atomic>
#include <memory>
#include <string>
#include <thread>

class EventLoop;
class RpcChannel;
class RpcClusterChannel;

// Client entry point: owns one EventLoop running on a dedicated thread and
// creates RpcChannel objects (one per server endpoint). The single loop is the
// client-side equivalent of "one loop per thread" from the V1 server design.
class RpcClient {
public:
    RpcClient();
    ~RpcClient();

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    // Create a channel to host:port and start connecting (nonblocking).
    std::shared_ptr<RpcChannel> connect(const std::string& host, uint16_t port);

    // Create an empty cluster channel (instances added via addInstance),
    // sharing the client event loop.
    std::shared_ptr<RpcClusterChannel> createClusterChannel();

    // Stop the loop thread; in-flight calls fail with kUnknown.
    void stop();

private:
    EventLoop* loop_{nullptr};
    std::unique_ptr<EventLoop> loopOwner_;
    std::thread loopThread_;
    std::atomic<bool> running_{false};
};

#endif  // MYRPCPROJECT_INCLUDE_RPCCLIENT_H_
