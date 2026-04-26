#ifndef MYRPCPROJECT_INCLUDE_ACCEPTOR_H_
#define MYRPCPROJECT_INCLUDE_ACCEPTOR_H_

#include <functional>
#include <cstdint>
#include <memory>
#include <string>

struct sockaddr_in;
class Channel;
class EventLoop;

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const sockaddr_in&)>;

    Acceptor(EventLoop* loop, const std::string& ip, uint16_t port);
    virtual ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void setNewConnectionCallback(NewConnectionCallback cb);
    void listen();
    void stop();

private:
    void handleRead();
    int createNonblockingSocket() const;

private:
    EventLoop* loop_;
    int listenFd_{-1};
    bool listening_{false};
    std::shared_ptr<Channel> acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
};

#endif  // MYRPCPROJECT_INCLUDE_ACCEPTOR_H_
