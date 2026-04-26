#ifndef MYRPCPROJECT_INCLUDE_TCPCONNECTION_H_
#define MYRPCPROJECT_INCLUDE_TCPCONNECTION_H_

#include "Buffer.h"
#include "MemoryPool.h"
#include "Message.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class Channel;
class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum class State {
        Connecting,
        Connected,
        Disconnecting,
        Disconnected
    };

    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback = std::function<void(const std::shared_ptr<TcpConnection>&, const Message&)>;
    using CloseCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    TcpConnection(EventLoop* loop, int sockfd, std::string name);
    virtual ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    EventLoop* getLoop() const;
    const std::string& name() const;
    State state() const;

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setCloseCallback(CloseCallback cb);

    void send(std::string message);
    void sendMessage(const Message& message);
    void shutdown();
    void forceClose();
    bool isIdleFor(std::chrono::seconds timeout) const;

    void connectEstablished();
    void connectDestroyed();

private:
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();
    void sendInLoop(std::string message);
    void shutdownInLoop();
    void forceCloseInLoop();
    void touchActivity();
    void setState(State state);

private:
    EventLoop* loop_;
    std::string name_;
    State state_{State::Connecting};
    int socketFd_{-1};
    std::shared_ptr<Channel> channel_;
    std::shared_ptr<Buffer> inputBuffer_;
    std::shared_ptr<Buffer> outputBuffer_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    mutable std::mutex activityMutex_;
    std::chrono::steady_clock::time_point lastActiveAt_{std::chrono::steady_clock::now()};
};

#endif  // MYRPCPROJECT_INCLUDE_TCPCONNECTION_H_
