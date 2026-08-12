#ifndef MYRPCPROJECT_INCLUDE_RPCCHANNEL_H_
#define MYRPCPROJECT_INCLUDE_RPCCHANNEL_H_

#include "EventLoop.h"
#include "Message.h"
#include "RpcController.h"
#include "Serializer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class Channel;
class Buffer;

// A virtual channel to a single server endpoint: it owns the underlying TCP
// connection and multiplexes many in-flight calls over it by request_id
// (see docs/v2-design-draft.md section 1.4, modeled on bRPC correlation_id /
// Netpoll mux).
//
// - responses may arrive out of order and are matched by request_id in O(1);
// - a late response (no pending entry) is silently dropped;
// - every async call's done callback is invoked exactly once: on response, on
//   deadline expiry, or when the connection fails;
// - call() is the synchronous flavor (async + blocking wait with a forced
//   completion on timeout, so no callback ever touches a destroyed stack).
//
// Thread safety: callAsync/call may be used from any thread; the connection,
// timers and decode happen on the channel's EventLoop thread.
class RpcChannel : public std::enable_shared_from_this<RpcChannel> {
public:
    using Done = std::function<void()>;

    RpcChannel(EventLoop* loop, std::string host, uint16_t port);
    ~RpcChannel();

    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;

    // Begin the nonblocking connect. Called by RpcClient; requests issued
    // before the connection is ready are queued and flushed on connect.
    void start();

    // Close the connection and fail all in-flight calls.
    void stop();

    // Async call; *response is written before done is invoked. The controller's
    // status is updated when the call finishes.
    void callAsync(RpcController& controller, const Value& request,
                   Value* response, Done done);

    // Sync call; returns true when the RPC finished with kOk.
    bool call(RpcController& controller, const Value& request, Value* response);

    void setConnectTimeoutMs(uint32_t ms);
    uint32_t connectTimeoutMs() const { return connectTimeoutMs_; }

    // --- load-balancer observation points (V2.1) ---------------------------
    bool isHealthy() const;            // connection is Ready
    std::size_t inflightCount() const; // active pending calls
    double latencyEmaMs() const;       // exponential moving average of RTT

    // Application-level keepalive: when the connection has been idle (no
    // inbound bytes) for one interval, a heartbeat frame is sent; after two
    // consecutive heartbeats without any inbound frame the connection is
    // declared dead and closed (period must stay below the server idle
    // cleanup timeout, e.g. 5s vs 30s). 0 disables heartbeats.
    void setHeartbeatIntervalMs(uint32_t ms);
    uint32_t heartbeatIntervalMs() const { return heartbeatIntervalMs_; }

private:
    struct PendingCall {
        uint32_t id{0};
        // The caller owns the controller and keeps it alive until done; the
        // channel updates its status when the call finishes (classic
        // RpcController contract).
        RpcController* ctrl{nullptr};
        Value* response{nullptr};
        Done done;
        EventLoop::TimerId timerId{0};
        std::chrono::steady_clock::time_point start;
    };

    enum class State { Idle, Connecting, Ready, Closed };

    // loop thread
    void doConnect();
    void handleConnectWrite();
    void handleRead();
    void handleClose();
    void onConnected();
    void flushOutput();
    void writeFrame(const RpcHeader& header, const std::string& body);
    void sendInLoop(uint32_t id, uint32_t methodId, uint32_t timeoutMs, const std::string& body);
    void onCallTimeout(uint32_t id);
    void onHeartbeatTick();
    void scheduleReconnect();
    void failPending(uint32_t id, proto::Status status, const std::string& text);
    void dispatchResponse(const Message& resp);

    // any thread
    uint32_t asyncCall(RpcController& controller, const Value& request,
                       Value* response, Done done);
    void cancelCall(uint32_t id, proto::Status status, const std::string& text);

private:
    EventLoop* loop_;
    std::string host_;
    uint16_t port_;
    int sockfd_{-1};
    std::shared_ptr<Channel> channel_;
    std::shared_ptr<Buffer> inputBuffer_;

    mutable std::mutex mutex_;
    State state_{State::Idle};
    std::vector<std::unique_ptr<PendingCall>> pending_;  // slot index = id - 1
    std::queue<size_t> freeSlots_;
    uint32_t connectTimeoutMs_{200};
    std::string outputQueue_;  // frames queued before the connection is ready

    // heartbeat state (loop thread only)
    uint32_t heartbeatIntervalMs_{0};
    std::chrono::steady_clock::time_point lastReceivedAt_{std::chrono::steady_clock::now()};
    uint32_t unansweredHeartbeats_{0};

    // latency EMA observed by the load balancer
    double latencyEmaMs_{0.0};

    // automatic reconnect (loop thread only); disabled by stop()
    bool reconnectEnabled_{true};
    EventLoop::TimerId reconnectTimerId_{0};
};

#endif  // MYRPCPROJECT_INCLUDE_RPCCHANNEL_H_
