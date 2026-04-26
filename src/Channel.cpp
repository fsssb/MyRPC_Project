#include "Channel.h"

#include "EventLoop.h"

#include <poll.h>
#include <utility>

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

void Channel::handleEvent() const {
    if ((revents_ & (POLLERR | POLLNVAL)) && errorCallback_) {
        errorCallback_();
    }

    if ((revents_ & POLLHUP) && closeCallback_) {
        closeCallback_();
    }

    if ((revents_ & (POLLIN | POLLPRI | POLLHUP)) && readCallback_) {
        readCallback_();
    }

    if ((revents_ & POLLOUT) && writeCallback_) {
        writeCallback_();
    }
}

void Channel::setReadCallback(EventCallback cb) {
    readCallback_ = std::move(cb);
}

void Channel::setWriteCallback(EventCallback cb) {
    writeCallback_ = std::move(cb);
}

void Channel::setCloseCallback(EventCallback cb) {
    closeCallback_ = std::move(cb);
}

void Channel::setErrorCallback(EventCallback cb) {
    errorCallback_ = std::move(cb);
}

int Channel::fd() const {
    return fd_;
}

short Channel::events() const {
    return events_;
}

void Channel::setRevents(short revents) {
    revents_ = revents;
}

bool Channel::isNoneEvent() const {
    return events_ == 0;
}

bool Channel::isWriting() const {
    return (events_ & POLLOUT) != 0;
}

void Channel::enableReading() {
    events_ = static_cast<short>(events_ | POLLIN);
    update();
}

void Channel::enableWriting() {
    events_ = static_cast<short>(events_ | POLLOUT);
    update();
}

void Channel::disableWriting() {
    events_ = static_cast<short>(events_ & ~POLLOUT);
    update();
}

void Channel::disableAll() {
    events_ = 0;
    update();
}

void Channel::update() {
    loop_->updateChannel(this);
}
