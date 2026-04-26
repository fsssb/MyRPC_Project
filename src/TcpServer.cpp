#include "TcpServer.h"

#include "Acceptor.h"
#include "EventLoop.h"
#include "Logger.h"
#include "Metrics.h"
#include "ThreadPool.h"

#include <chrono>
#include <netinet/in.h>
#include <string>

TcpServer::TcpServer(EventLoop* loop, const std::string& ip, uint16_t port, int threadNum)
    : loop_(loop),
      acceptor_(std::make_unique<Acceptor>(loop, ip, port)),
      threadPool_(std::make_shared<ThreadPool>(loop, threadNum)) {
    acceptor_->setNewConnectionCallback([this](int sockfd, const sockaddr_in& peerAddr) {
        newConnection(sockfd, peerAddr);
    });
}

TcpServer::~TcpServer() = default;

void TcpServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    threadPool_->start();
    acceptor_->listen();
    idleCheckTimerId_ = loop_->runEvery(std::chrono::seconds(1), [this]() {
        checkIdleConnections();
    });
}

void TcpServer::stop() {
    if (!started_.exchange(false)) {
        return;
    }

    loop_->runInLoop([this]() {
        if (idleCheckTimerId_ != 0) {
            loop_->cancelTimer(idleCheckTimerId_);
            idleCheckTimerId_ = 0;
        }

        acceptor_->stop();
        for (auto& item : connections_) {
            item.second->forceClose();
        }
        connections_.clear();
    });

    threadPool_->stop();
}

void TcpServer::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpServer::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpServer::submitTask(ThreadPool::Task task) {
    threadPool_->submit(std::move(task));
}

std::size_t TcpServer::pendingTaskSize() const {
    return threadPool_->pendingTaskSize();
}

void TcpServer::newConnection(int sockfd, const sockaddr_in& /*peerAddr*/) {
    EventLoop* ioLoop = threadPool_->getNextLoop();
    const uint64_t connId = nextConnId_.fetch_add(1);
    const std::string connName = "conn-" + std::to_string(connId);

    auto conn = std::make_shared<TcpConnection>(ioLoop, sockfd, connName);
    conn->setConnectionCallback(connectionCallback_ ? connectionCallback_
                                                    : [](const std::shared_ptr<TcpConnection>& connection) {
                                                          LOG_INFO("connection established: " + connection->name());
                                                      });
    conn->setMessageCallback(messageCallback_ ? messageCallback_
                                              : [](const std::shared_ptr<TcpConnection>& connection,
                                                   const Message& message) {
                                                    connection->sendMessage(message);
                                                });
    conn->setCloseCallback([this](const std::shared_ptr<TcpConnection>& connection) {
        removeConnection(connection);
    });

    connections_[connName] = conn;
    Metrics::instance().setActiveConnections(connections_.size());
    ioLoop->runInLoop([conn]() {
        conn->connectEstablished();
    });
}

void TcpServer::removeConnection(const std::shared_ptr<TcpConnection>& conn) {
    loop_->runInLoop([this, conn]() {
        removeConnectionInLoop(conn);
    });
}

void TcpServer::removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn) {
    connections_.erase(conn->name());
    Metrics::instance().setActiveConnections(connections_.size());
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn]() {
        conn->connectDestroyed();
    });
}

void TcpServer::checkIdleConnections() {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->second->isIdleFor(idleTimeout_)) {
            LOG_INFO("closing idle connection: " + it->first);
            it->second->forceClose();
            it = connections_.erase(it);
            Metrics::instance().setActiveConnections(connections_.size());
        } else {
            ++it;
        }
    }
}
