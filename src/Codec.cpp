#include "Codec.h"

#include <arpa/inet.h>
#include <cstring>

std::string Codec::encode(const Message& message) {
    const std::size_t bodyLen = message.body.size();
    uint32_t netLen = htonl(static_cast<uint32_t>(bodyLen));

    std::string frame;
    frame.resize(kHeaderLen + bodyLen);
    std::memcpy(frame.data(), &netLen, kHeaderLen);
    if (bodyLen > 0) {
        std::memcpy(frame.data() + kHeaderLen, message.body.data(), bodyLen);
    }
    return frame;
}

void Codec::decode(Buffer* buffer, const MessageCallback& cb) {
    while (buffer->readableBytes() >= kHeaderLen) {
        uint32_t netLen = 0;
        std::memcpy(&netLen, buffer->peek(), kHeaderLen);
        const uint32_t bodyLen = ntohl(netLen);

        if (bodyLen > kMaxBodyLen) {
            buffer->retrieveAll();
            return;
        }

        if (buffer->readableBytes() < kHeaderLen + bodyLen) {
            return;
        }

        buffer->retrieve(kHeaderLen);
        Message msg;
        msg.body = buffer->retrieveAsString(bodyLen);
        if (cb) {
            cb(msg);
        }
    }
}
