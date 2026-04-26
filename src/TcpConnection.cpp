#include "TcpConnection.h"

#include "Channel.h"
#include "Codec.h"
#include "EventLoop.h"
#include "Logger.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

TcpConnection::TcpConnection(EventLoop* loop, int sockfd, std::string name)
    : loop_(loop),
      name_(std::move(name)),
      socketFd_(sockfd),
      channel_(std::make_shared<Channel>(loop_, socketFd_)),
      inputBuffer_(BufferMemoryPool::instance().acquire()),
      outputBuffer_(BufferMemoryPool::instance().acquire()) {
    channel_->setReadCallback([this]() {
        handleRead();
    });
    channel_->setWriteCallback([this]() {
        handleWrite();
    });
    channel_->setCloseCallback([this]() {
        handleClose();
    });
    channel_->setErrorCallback([this]() {
        handleError();
    });
}

TcpConnection::~TcpConnection() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
}

EventLoop* TcpConnection::getLoop() const {
    return loop_;
}

const std::string& TcpConnection::name() const {
    return name_;
}

TcpConnection::State TcpConnection::state() const {
    return state_;
}

void TcpConnection::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpConnection::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpConnection::setCloseCallback(CloseCallback cb) {
    closeCallback_ = std::move(cb);
}

void TcpConnection::send(std::string message) {
    if (state_ != State::Connected) {
        return;
    }

    auto self = shared_from_this();
    if (loop_->isInLoopThread()) {
        sendInLoop(std::move(message));
    } else {
        loop_->runInLoop([self, message = std::move(message)]() mutable {
            self->sendInLoop(std::move(message));
        });
    }
}

void TcpConnection::sendMessage(const Message& message) {
    send(Codec::encode(message));
}

void TcpConnection::shutdown() {
    if (state_ != State::Connected) {
        return;
    }
    setState(State::Disconnecting);
    auto self = shared_from_this();
    loop_->runInLoop([self]() {
        self->shutdownInLoop();
    });
}

void TcpConnection::forceClose() {
    auto self = shared_from_this();
    loop_->runInLoop([self]() {
        self->forceCloseInLoop();
    });
}

bool TcpConnection::isIdleFor(std::chrono::seconds timeout) const {
    std::lock_guard<std::mutex> lock(activityMutex_);
    return std::chrono::steady_clock::now() - lastActiveAt_ >= timeout;
}

void TcpConnection::connectEstablished() {
    setState(State::Connected);
    touchActivity();
    channel_->enableReading();
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    if (state_ == State::Connected || state_ == State::Disconnecting) {
        setState(State::Disconnected);
        channel_->disableAll();
    }
    loop_->removeChannel(channel_.get());
}

void TcpConnection::handleRead() {
    char buf[4096];
#if defined(__linux__)
    for (;;) {
        const ssize_t n = ::read(socketFd_, buf, sizeof(buf));
        if (n > 0) {
            inputBuffer_->append(buf, static_cast<std::size_t>(n));
            touchActivity();
            if (messageCallback_) {
                auto self = shared_from_this();
                Codec::decode(inputBuffer_.get(), [self, this](const Message& msg) {
                    if (messageCallback_) {
                        messageCallback_(self, msg);
                    }
                });
            }
            continue;
        }
        if (n == 0) {
            handleClose();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        handleError();
        return;
    }
#else
    const ssize_t n = ::read(socketFd_, buf, sizeof(buf));
    if (n > 0) {
        inputBuffer_->append(buf, static_cast<std::size_t>(n));
        touchActivity();
        if (messageCallback_) {
            auto self = shared_from_this();
            Codec::decode(inputBuffer_.get(), [self, this](const Message& msg) {
                if (messageCallback_) {
                    messageCallback_(self, msg);
                }
            });
        }
        return;
    }

    if (n == 0) {
        handleClose();
        return;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        handleError();
    }
#endif
}

void TcpConnection::handleWrite() {
#if defined(__linux__)
    for (;;) {
        if (outputBuffer_->readableBytes() == 0) {
            channel_->disableWriting();
            return;
        }

        const ssize_t n = ::write(socketFd_, outputBuffer_->peek(), outputBuffer_->readableBytes());
        if (n > 0) {
            outputBuffer_->retrieve(static_cast<std::size_t>(n));
            touchActivity();
            if (outputBuffer_->readableBytes() == 0) {
                channel_->disableWriting();
                if (state_ == State::Disconnecting) {
                    shutdownInLoop();
                }
                return;
            }
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        handleError();
        return;
    }
#else
    if (outputBuffer_->readableBytes() == 0) {
        channel_->disableWriting();
        return;
    }

    const ssize_t n = ::write(socketFd_, outputBuffer_->peek(), outputBuffer_->readableBytes());
    if (n > 0) {
        outputBuffer_->retrieve(static_cast<std::size_t>(n));
        touchActivity();
        if (outputBuffer_->readableBytes() == 0) {
            channel_->disableWriting();
            if (state_ == State::Disconnecting) {
                shutdownInLoop();
            }
        }
        return;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        handleError();
    }
#endif
}

void TcpConnection::handleClose() {
    setState(State::Disconnected);
    channel_->disableAll();

    if (closeCallback_) {
        closeCallback_(shared_from_this());
    }
}

void TcpConnection::handleError() {
    LOG_ERROR("tcp connection error on " + name_ + ": " + std::string(std::strerror(errno)));
}

void TcpConnection::sendInLoop(std::string message) {
    if (state_ == State::Disconnected) {
        return;
    }

    if (outputBuffer_->readableBytes() == 0) {
        const ssize_t n = ::write(socketFd_, message.data(), message.size());
        if (n >= 0) {
            const std::size_t written = static_cast<std::size_t>(n);
            if (written < message.size()) {
                outputBuffer_->append(message.data() + written, message.size() - written);
                channel_->enableWriting();
            }
            return;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            handleError();
            return;
        }
    }

    outputBuffer_->append(std::move(message));
    channel_->enableWriting();
}

void TcpConnection::shutdownInLoop() {
    if (outputBuffer_->readableBytes() > 0 || channel_->isWriting()) {
        return;
    }
    ::shutdown(socketFd_, SHUT_WR);
}

void TcpConnection::forceCloseInLoop() {
    if (state_ == State::Connected || state_ == State::Disconnecting) {
        handleClose();
    }
}

void TcpConnection::touchActivity() {
    std::lock_guard<std::mutex> lock(activityMutex_);
    lastActiveAt_ = std::chrono::steady_clock::now();
}

void TcpConnection::setState(State state) {
    state_ = state;
}
