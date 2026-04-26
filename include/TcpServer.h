#ifndef MYRPCPROJECT_INCLUDE_TCPSERVER_H_
#define MYRPCPROJECT_INCLUDE_TCPSERVER_H_

#include "EventLoop.h"
#include "ThreadPool.h"
#include "TcpConnection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class Acceptor;
class EventLoop;
struct sockaddr_in;

class TcpServer {
public:
    using ConnectionCallback = TcpConnection::ConnectionCallback;
    using MessageCallback = TcpConnection::MessageCallback;

    TcpServer(EventLoop* loop, const std::string& ip, uint16_t port, int threadNum);
    virtual ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void start();
    void stop();
    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void submitTask(ThreadPool::Task task);
    std::size_t pendingTaskSize() const;

private:
    void newConnection(int sockfd, const sockaddr_in& peerAddr);
    void removeConnection(const std::shared_ptr<TcpConnection>& conn);
    void removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn);
    void checkIdleConnections();

private:
    EventLoop* loop_;
    std::unique_ptr<Acceptor> acceptor_;
    std::shared_ptr<ThreadPool> threadPool_;
    std::unordered_map<std::string, std::shared_ptr<TcpConnection>> connections_;
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> nextConnId_{1};
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    EventLoop::TimerId idleCheckTimerId_{0};
    const std::chrono::seconds idleTimeout_{30};
};

#endif  // MYRPCPROJECT_INCLUDE_TCPSERVER_H_
