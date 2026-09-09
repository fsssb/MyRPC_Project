#ifndef MYRPCPROJECT_INCLUDE_METRICSSERVER_H_
#define MYRPCPROJECT_INCLUDE_METRICSSERVER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

class Acceptor;
class Channel;
class EventLoop;
struct sockaddr_in;

// A minimal HTTP server exposing Prometheus text metrics at GET /metrics
// (see docs/v2-design-draft.md section 3.6). The RPC stack is frame-based, so
// this endpoint runs on its own plain-TCP listener: read until the request
// head, answer 200 with Metrics::renderSnapshot(), close.
class MetricsServer {
public:
    MetricsServer(EventLoop* loop, uint16_t port);
    ~MetricsServer();

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    void start();

private:
    struct Conn {
        int fd{-1};
        std::shared_ptr<Channel> channel;
        std::string buffer;
    };

    void onNewConnection(int sockfd, const sockaddr_in& peer);
    void onRead(const std::shared_ptr<Conn>& conn);
    void handleRequest(const std::shared_ptr<Conn>& conn);
    void closeConn(const std::shared_ptr<Conn>& conn);

private:
    EventLoop* loop_;
    std::unique_ptr<Acceptor> acceptor_;
    std::map<int, std::shared_ptr<Conn>> conns_;
};

#endif  // MYRPCPROJECT_INCLUDE_METRICSSERVER_H_
