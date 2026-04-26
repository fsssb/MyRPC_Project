#include "Buffer.h"

#include <algorithm>
#include <cstring>

Buffer::Buffer(std::size_t initialSize) : buffer_(initialSize, 0) {}

std::size_t Buffer::readableBytes() const {
    return writeIndex_ - readIndex_;
}

std::size_t Buffer::writableBytes() const {
    return buffer_.size() - writeIndex_;
}

const char* Buffer::peek() const {
    return buffer_.data() + readIndex_;
}

void Buffer::retrieve(std::size_t len) {
    if (len >= readableBytes()) {
        retrieveAll();
        return;
    }
    readIndex_ += len;
}

void Buffer::retrieveAll() {
    readIndex_ = 0;
    writeIndex_ = 0;
}

void Buffer::reset() {
    retrieveAll();
}

std::string Buffer::retrieveAsString(std::size_t len) {
    len = std::min(len, readableBytes());
    std::string result(peek(), len);
    retrieve(len);
    return result;
}

std::string Buffer::retrieveAllAsString() {
    return retrieveAsString(readableBytes());
}

void Buffer::append(const char* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }

    ensureWritableBytes(len);
    std::memcpy(buffer_.data() + writeIndex_, data, len);
    writeIndex_ += len;
}

void Buffer::append(const std::string& data) {
    append(data.data(), data.size());
}

void Buffer::append(std::string&& data) {
    append(data.data(), data.size());
}

void Buffer::ensureWritableBytes(std::size_t len) {
    if (writableBytes() >= len) {
        return;
    }

    const std::size_t required = writeIndex_ + len;
    const std::size_t newSize = std::max(required, buffer_.size() * 2);
    buffer_.resize(newSize);
}
