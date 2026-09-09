#include "MetricsServer.h"

#include "Acceptor.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Metrics.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr std::size_t kMaxRequestBytes = 8192;
}

MetricsServer::MetricsServer(EventLoop* loop, uint16_t port)
    : loop_(loop), acceptor_(std::make_unique<Acceptor>(loop, "0.0.0.0", port)) {
    acceptor_->setNewConnectionCallback(
        [this](int sockfd, const sockaddr_in& peer) { onNewConnection(sockfd, peer); });
}

MetricsServer::~MetricsServer() = default;

void MetricsServer::start() {
    acceptor_->listen();
}

void MetricsServer::onNewConnection(int sockfd, const sockaddr_in& /*peer*/) {
    auto conn = std::make_shared<Conn>();
    conn->fd = sockfd;
    conn->channel = std::make_shared<Channel>(loop_, sockfd);
    conn->channel->setReadCallback([this, conn]() { onRead(conn); });
    conn->channel->setCloseCallback([this, conn]() { closeConn(conn); });
    conn->channel->setErrorCallback([this, conn]() { closeConn(conn); });
    conn->channel->enableReading();
    conns_[sockfd] = conn;
}

void MetricsServer::onRead(const std::shared_ptr<Conn>& conn) {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(conn->fd, buf, sizeof(buf));
        if (n > 0) {
            conn->buffer.append(buf, static_cast<std::size_t>(n));
            if (conn->buffer.size() > kMaxRequestBytes ||
                conn->buffer.find("\r\n\r\n") != std::string::npos) {
                handleRequest(conn);
                return;
            }
            continue;
        }
        if (n == 0) {
            closeConn(conn);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        closeConn(conn);
        return;
    }
}

void MetricsServer::handleRequest(const std::shared_ptr<Conn>& conn) {
    const bool isMetrics = conn->buffer.rfind("GET /metrics", 0) == 0;
    const std::string body = isMetrics ? Metrics::instance().renderSnapshot() : "not found";
    const std::string reason = isMetrics ? "200 OK" : "404 Not Found";
    std::string response = "HTTP/1.1 " + reason +
                           "\r\nContent-Type: text/plain; version=0.0.4\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body;
    // Response is small enough for a single non-blocking write; drop the rest.
    (void)::write(conn->fd, response.data(), response.size());
    closeConn(conn);
}

void MetricsServer::closeConn(const std::shared_ptr<Conn>& conn) {
    if (conn->fd < 0) {
        return;
    }
    conn->channel->disableAll();
    loop_->removeChannel(conn->channel.get());
    ::close(conn->fd);
    conns_.erase(conn->fd);
    conn->fd = -1;
}
