#include "RpcChannel.h"

#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Logger.h"
#include "Protocol.h"
#include "RpcFramer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

RpcChannel::RpcChannel(EventLoop* loop, std::string host, uint16_t port)
    : loop_(loop),
      host_(std::move(host)),
      port_(port),
      inputBuffer_(std::make_shared<Buffer>()) {}

RpcChannel::~RpcChannel() {
    // shared_from_this() is unavailable during destruction, so do a
    // synchronous close instead of stop()'s loop-thread dispatch. Callers
    // should stop() the channel (or the owning RpcClient) before releasing the
    // last reference; a channel already dead (heartbeat / peer close) makes
    // this path a no-op.
    int fd = -1;
    std::vector<std::unique_ptr<PendingCall>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Closed;
        fd = sockfd_;
        sockfd_ = -1;
        doomed.swap(pending_);
        std::queue<size_t> empty;
        freeSlots_.swap(empty);
    }
    if (fd >= 0 && channel_) {
        channel_->disableAll();
    }
    if (fd >= 0) {
        ::close(fd);
    }
    channel_.reset();
    for (auto& pc : doomed) {
        if (pc && pc->done) {
            pc->ctrl->setStatus(proto::kUnknown, "channel destroyed");
            pc->done();
        }
    }
}

void RpcChannel::start() {
    loop_->runInLoop([self = shared_from_this()]() {
        if (self->state_ == State::Idle) {
            self->doConnect();
        }
    });
}

void RpcChannel::stop() {
    loop_->runInLoop([self = shared_from_this()]() {
        if (self->sockfd_ >= 0) {
            if (self->channel_) {
                self->channel_->disableAll();
                self->loop_->removeChannel(self->channel_.get());
            }
            ::close(self->sockfd_);
            self->sockfd_ = -1;
        }
        self->channel_.reset();
        std::vector<std::unique_ptr<PendingCall>> doomed;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->state_ = State::Closed;
            doomed.swap(self->pending_);
            std::queue<size_t> empty;
            self->freeSlots_.swap(empty);  // slots no longer valid
        }
        for (auto& pc : doomed) {
            if (pc && pc->done) {
                pc->ctrl->setStatus(proto::kUnknown, "channel stopped");
                pc->done();
            }
        }
    });
}

void RpcChannel::setConnectTimeoutMs(uint32_t ms) {
    connectTimeoutMs_ = ms;
}

void RpcChannel::setHeartbeatIntervalMs(uint32_t ms) {
    heartbeatIntervalMs_ = ms;
}

void RpcChannel::callAsync(RpcController& controller, const Value& request,
                           Value* response, Done done) {
    asyncCall(controller, request, response, std::move(done));
}

bool RpcChannel::call(RpcController& controller, const Value& request, Value* response) {
    std::mutex m;
    std::condition_variable cv;
    bool doneCalled = false;
    const auto notify = [&]() {
        std::lock_guard<std::mutex> lk(m);
        doneCalled = true;
        cv.notify_all();
    };
    const uint32_t id = asyncCall(controller, request, response, notify);

    std::unique_lock<std::mutex> lk(m);
    const uint32_t waitMs = controller.timeoutMs() > 0 ? controller.timeoutMs() + 500 : 10000;
    cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&]() { return doneCalled; });
    if (!doneCalled) {
        // Force-complete the call on the loop thread so done runs before we
        // return; the late response (if any) is discarded by request_id.
        cancelCall(id, proto::kDeadlineExceeded, "client sync wait timed out");
        cv.wait(lk, [&]() { return doneCalled; });
    }
    return controller.status() == proto::kOk;
}

uint32_t RpcChannel::asyncCall(RpcController& controller, const Value& request,
                               Value* response, Done done) {
    auto pc = std::make_unique<PendingCall>();
    pc->ctrl = &controller;
    pc->response = response;
    pc->done = std::move(done);

    const std::string body = Serializer::encode(request);
    size_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Closed) {
            // The channel is gone (peer died / stopped): fail the call right
            // away instead of touching the (now empty) pending table.
            pc->ctrl->setStatus(proto::kUnknown, "channel closed");
            pc->done();
            return 0;
        }
        if (freeSlots_.empty()) {
            slot = pending_.size();
            pending_.push_back(nullptr);
        } else {
            slot = freeSlots_.front();
            freeSlots_.pop();
        }
        pc->id = static_cast<uint32_t>(slot) + 1;
        pending_[slot] = std::move(pc);
    }

    // Client-side deadline timer; fires on the loop thread. The timeout value
    // is read under the lock so a completed/reused slot is never scheduled.
    const uint32_t id = slot + 1;
    const uint32_t methodId = controller.methodId();
    const uint32_t timeoutMs = controller.timeoutMs();
    if (timeoutMs > 0) {
        loop_->runInLoop([self = shared_from_this(), id]() {
            uint32_t ms = 0;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                const size_t s = id - 1;
                if (s < self->pending_.size() && self->pending_[s] &&
                    self->pending_[s]->id == id) {
                    ms = self->pending_[s]->ctrl->timeoutMs();
                }
            }
            if (ms > 0) {
                self->loop_->runAfter(std::chrono::milliseconds(ms),
                                      [self, id]() { self->onCallTimeout(id); });
            }
        });
    }

    // Send the request on the loop thread; frames issued before the connection
    // is ready are queued and flushed by onConnected.
    loop_->runInLoop([self = shared_from_this(), id, methodId, timeoutMs, body]() {
        self->sendInLoop(id, methodId, timeoutMs, body);
    });
    return id;
}

void RpcChannel::onCallTimeout(uint32_t id) {
    failPending(id, proto::kDeadlineExceeded, "client deadline exceeded");
}

void RpcChannel::cancelCall(uint32_t id, proto::Status status, const std::string& text) {
    loop_->runInLoop([self = shared_from_this(), id, status, text]() {
        self->failPending(id, status, text);
    });
}

void RpcChannel::failPending(uint32_t id, proto::Status status, const std::string& text) {
    std::unique_ptr<PendingCall> pc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t slot = id - 1;
        if (slot >= pending_.size() || !pending_[slot] || pending_[slot]->id != id) {
            return;  // already completed or slot reused
        }
        pc = std::move(pending_[slot]);
        pending_[slot].reset();
        freeSlots_.push(slot);
    }
    pc->ctrl->setStatus(status, text);
    if (pc->done) {
        pc->done();
    }
}

void RpcChannel::dispatchResponse(const Message& resp) {
    std::unique_ptr<PendingCall> pc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t slot = resp.header.requestId - 1;
        if (slot >= pending_.size() || !pending_[slot] || pending_[slot]->id != resp.header.requestId) {
            LOG_INFO("RpcChannel: dropping late response for request_id=" +
                     std::to_string(resp.header.requestId));
            return;
        }
        pc = std::move(pending_[slot]);
        pending_[slot].reset();
        freeSlots_.push(slot);
    }
    pc->ctrl->setStatus(static_cast<proto::Status>(resp.header.status),
                       resp.header.status == proto::kOk ? std::string() : "rpc failed");
    if (pc->ctrl->status() == proto::kOk && !resp.body.empty()) {
        Serializer::decode(resp.body, pc->response);
    }
    if (pc->done) {
        pc->done();
    }
}

// --- connection (all on the loop thread) -----------------------------------

void RpcChannel::doConnect() {
    state_ = State::Connecting;
    sockfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        LOG_ERROR("RpcChannel: socket failed: " + std::string(std::strerror(errno)));
        handleClose();
        return;
    }
    // Nonblocking + CLOEXEC (macOS has no SOCK_NONBLOCK/SOCK_CLOEXEC flags).
    const int flags = ::fcntl(sockfd_, F_GETFL, 0);
    ::fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);
    const int fdFlags = ::fcntl(sockfd_, F_GETFD, 0);
    ::fcntl(sockfd_, F_SETFD, fdFlags | FD_CLOEXEC);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("RpcChannel: invalid address: " + host_);
        handleClose();
        return;
    }

    const int ret = ::connect(sockfd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        onConnected();
        return;
    }
    if (errno != EINPROGRESS) {
        LOG_ERROR("RpcChannel: connect to " + host_ + " failed: " + std::string(std::strerror(errno)));
        handleClose();
        return;
    }

    channel_ = std::make_shared<Channel>(loop_, sockfd_);
    channel_->setWriteCallback([self = shared_from_this()]() { self->handleConnectWrite(); });
    channel_->setCloseCallback([self = shared_from_this()]() { self->handleClose(); });
    channel_->setErrorCallback([self = shared_from_this()]() { self->handleClose(); });
    channel_->enableWriting();

    loop_->runAfter(std::chrono::milliseconds(connectTimeoutMs_), [self = shared_from_this()]() {
        if (self->state_ == State::Connecting) {
            LOG_ERROR("RpcChannel: connect timeout to " + self->host_);
            self->handleClose();
        }
    });
}

void RpcChannel::handleConnectWrite() {
    if (state_ != State::Connecting) {
        return;
    }
    int error = 0;
    socklen_t len = sizeof(error);
    ::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        LOG_ERROR("RpcChannel: connect to " + host_ + " failed: " + std::string(std::strerror(error)));
        handleClose();
        return;
    }
    onConnected();
}

void RpcChannel::onConnected() {
    state_ = State::Ready;
    channel_->disableWriting();
    channel_->setReadCallback([self = shared_from_this()]() { self->handleRead(); });
    channel_->setCloseCallback([self = shared_from_this()]() { self->handleClose(); });
    channel_->setErrorCallback([self = shared_from_this()]() { self->handleClose(); });
    channel_->enableReading();
    lastReceivedAt_ = std::chrono::steady_clock::now();
    unansweredHeartbeats_ = 0;
    if (heartbeatIntervalMs_ > 0) {
        loop_->runEvery(std::chrono::milliseconds(heartbeatIntervalMs_),
                        [self = shared_from_this()]() { self->onHeartbeatTick(); });
    }
    flushOutput();
}

void RpcChannel::onHeartbeatTick() {
    if (state_ != State::Ready) {
        return;
    }
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - lastReceivedAt_);
    if (idleMs.count() < static_cast<int64_t>(heartbeatIntervalMs_)) {
        return;  // recent inbound traffic, no heartbeat needed
    }
    if (unansweredHeartbeats_ >= 2) {
        LOG_ERROR("RpcChannel: peer dead (no heartbeat ack for " + host_);
        handleClose();
        return;
    }
    ++unansweredHeartbeats_;
    RpcHeader h;
    h.msgType = proto::kMsgHeartbeat;
    h.requestId = 0;
    writeFrame(h, std::string());
}

void RpcChannel::handleRead() {
    lastReceivedAt_ = std::chrono::steady_clock::now();
    unansweredHeartbeats_ = 0;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(sockfd_, buf, sizeof(buf));
        if (n > 0) {
            inputBuffer_->append(buf, static_cast<std::size_t>(n));
            RpcFramer::decode(inputBuffer_.get(), [self = shared_from_this()](const Message& msg) {
                self->dispatchResponse(msg);
            });
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
        handleClose();
        return;
    }
}

void RpcChannel::flushOutput() {
    if (state_ != State::Ready || outputQueue_.empty()) {
        return;
    }
    std::string frame = std::move(outputQueue_);
    const ssize_t n = ::write(sockfd_, frame.data(), frame.size());
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        handleClose();
        return;
    }
    if (n > 0 && static_cast<std::size_t>(n) < frame.size()) {
        // Partial write: keep the remainder in the queue and let the caller
        // retry on the next call; V2.2 replaces this with a write buffer chain.
        outputQueue_ = frame.substr(static_cast<std::size_t>(n));
    }
}

void RpcChannel::sendInLoop(uint32_t id, uint32_t methodId, uint32_t timeoutMs,
                            const std::string& body) {
    if (state_ == State::Closed) {
        failPending(id, proto::kUnknown, "channel closed before send");
        return;
    }
    RpcHeader header;
    header.msgType = proto::kMsgRequest;
    header.requestId = id;
    header.methodId = methodId;
    header.timeoutMs = timeoutMs;
    writeFrame(header, body);
}

void RpcChannel::writeFrame(const RpcHeader& header, const std::string& body) {
    Message msg;
    msg.header = header;
    msg.body = body;
    const std::string frame = RpcFramer::encode(msg);

    if (state_ != State::Ready) {
        outputQueue_ += frame;  // flushed by onConnected
        return;
    }
    if (!outputQueue_.empty()) {
        outputQueue_ += frame;
        flushOutput();
        return;
    }
    const ssize_t n = ::write(sockfd_, frame.data(), frame.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            outputQueue_ = frame;
            return;
        }
        handleClose();
        return;
    }
    if (static_cast<std::size_t>(n) < frame.size()) {
        outputQueue_ = frame.substr(static_cast<std::size_t>(n));
    }
}

void RpcChannel::handleClose() {
    if (state_ == State::Closed) {
        return;
    }
    state_ = State::Closed;
    if (sockfd_ >= 0) {
        if (channel_) {
            channel_->disableAll();
            loop_->removeChannel(channel_.get());  // poller must forget the fd
        }
        ::close(sockfd_);
        sockfd_ = -1;
    }
    channel_.reset();

    std::vector<std::unique_ptr<PendingCall>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        doomed.swap(pending_);
        std::queue<size_t> empty;
        freeSlots_.swap(empty);  // slots no longer valid
    }
    for (auto& pc : doomed) {
        if (pc) {
            pc->ctrl->setStatus(proto::kUnknown, "connection closed");
            if (pc->done) {
                pc->done();
            }
        }
    }
}
