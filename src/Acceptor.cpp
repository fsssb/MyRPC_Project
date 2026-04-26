#include "Acceptor.h"

#include "Channel.h"
#include "EventLoop.h"
#include "Logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

Acceptor::Acceptor(EventLoop* loop, const std::string& ip, uint16_t port) : loop_(loop) {
    listenFd_ = createNonblockingSocket();
    if (listenFd_ < 0) {
        throw std::runtime_error("create listen socket failed");
    }

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, static_cast<socklen_t>(sizeof(opt)));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error("invalid ip address");
    }

    if (::bind(listenFd_, reinterpret_cast<const sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error("bind failed");
    }

    acceptChannel_ = std::make_shared<Channel>(loop_, listenFd_);
    acceptChannel_->setReadCallback([this]() {
        handleRead();
    });
    acceptChannel_->setErrorCallback([]() {
        LOG_ERROR("accept channel error");
    });
}

Acceptor::~Acceptor() {
    if (acceptChannel_) {
        acceptChannel_->disableAll();
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void Acceptor::setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
}

void Acceptor::listen() {
    if (listening_) {
        return;
    }

    if (::listen(listenFd_, SOMAXCONN) < 0) {
        throw std::runtime_error("listen failed");
    }
    listening_ = true;
    acceptChannel_->enableReading();
}

void Acceptor::stop() {
    if (!listening_) {
        return;
    }
    listening_ = false;
    if (acceptChannel_) {
        acceptChannel_->disableAll();
    }
}

void Acceptor::handleRead() {
#if defined(__linux__)
    for (;;) {
        sockaddr_in peerAddr {};
        socklen_t len = static_cast<socklen_t>(sizeof(peerAddr));
        const int connfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peerAddr), &len);
        if (connfd >= 0) {
            int flags = ::fcntl(connfd, F_GETFL, 0);
            if (flags >= 0) {
                ::fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
            }
            if (newConnectionCallback_) {
                newConnectionCallback_(connfd, peerAddr);
            } else {
                ::close(connfd);
            }
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        LOG_ERROR("accept failed: " + std::string(std::strerror(errno)));
        return;
    }
#else
    sockaddr_in peerAddr {};
    socklen_t len = static_cast<socklen_t>(sizeof(peerAddr));
    const int connfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peerAddr), &len);
    if (connfd >= 0) {
        int flags = ::fcntl(connfd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
        }

        if (newConnectionCallback_) {
            newConnectionCallback_(connfd, peerAddr);
        } else {
            ::close(connfd);
        }
        return;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR("accept failed: " + std::string(std::strerror(errno)));
    }
#endif
}

int Acceptor::createNonblockingSocket() const {
    const int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    const int flags = ::fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }
    return sockfd;
}
