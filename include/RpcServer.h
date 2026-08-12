#ifndef MYRPCPROJECT_INCLUDE_RPCSERVER_H_
#define MYRPCPROJECT_INCLUDE_RPCSERVER_H_

#include "ConcurrencyLimiter.h"
#include "Logger.h"
#include "Protocol.h"
#include "Router.h"
#include "TcpServer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class EventLoop;
class TcpConnection;

// RPC service server: wraps TcpServer and adds RPC semantics on top of the
// raw message callback (see docs/v2-design-draft.md section 1.3):
//
// - request routing by method_id through the Router;
// - server-side deadline: a request whose handler finishes after timeout_ms
//   (carried in the request header) is answered with kDeadlineExceeded;
// - heartbeat answering: msg_type=kMsgHeartbeat is answered with
//   kMsgHeartbeatAck without touching business logic;
// - late-response safety inherited from V1 (weak_ptr + queueInLoop write-back).
//
// Handler execution is dispatched through an executor to keep I/O threads free;
// by default it is the TcpServer task queue (main loop pending functors), same
// as the V1 demo.
class RpcServer {
public:
    RpcServer(EventLoop* loop, const std::string& ip, uint16_t port, int threadNum);

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    void registerMethod(const std::string& service, const std::string& method, Router::Handler handler);

    // Custom executor for handler dispatch; defaults to TcpServer::submitTask.
    void setExecutor(std::function<void(std::function<void()>)> executor);

    void start();
    void stop();

    // Overload protection: max concurrent in-flight requests; 0 = unlimited.
    // Requests beyond the limit get kConcurrencyLimited immediately (no queue).
    void setMaxConcurrency(std::size_t n);

    // Two-phase graceful shutdown: stop accepting + reject new requests with
    // kShuttingDown, wait for in-flight requests to finish (up to timeout),
    // then close the remaining connections. Runs asynchronously so the main
    // loop (executor queue) stays alive while in-flight requests drain;
    // onDone is invoked when shutdown completes.
    void stopGracefully(std::chrono::milliseconds timeout, std::function<void()> onDone = {});

    // Backlog of the executor task queue (V1 limitation: always 0 until V2.2).
    std::size_t pendingTaskSize() const;

private:
    struct RequestContext;  // defined below

    void onMessage(const std::shared_ptr<TcpConnection>& conn, const Message& req);
    void handleRequest(const std::shared_ptr<TcpConnection>& conn, const Message& req);
    void sendResponse(const std::shared_ptr<TcpConnection>& conn,
                      const std::shared_ptr<RequestContext>& ctx, proto::Status status);
    void releaseSlot();

private:
    struct RequestContext {
        Value request;
        Value response;
        RpcHeader header;
        std::chrono::steady_clock::time_point start;
    };

    TcpServer tcpServer_;
    Router router_;
    std::function<void(std::function<void()>)> executor_;
    std::unique_ptr<ConcurrencyLimiter> limiter_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> inflight_{0};
};

#endif  // MYRPCPROJECT_INCLUDE_RPCSERVER_H_
