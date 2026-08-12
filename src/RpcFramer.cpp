#include "RpcFramer.h"

#include "Logger.h"
#include "Protocol.h"

#include <cstring>

std::string RpcFramer::encode(const Message& message) {
    if (message.body.size() > proto::kMaxBodyLen) {
        LOG_ERROR("RpcFramer::encode: body too large: " + std::to_string(message.body.size()));
        return std::string();
    }

    RpcHeader header = message.header;
    header.magic = proto::kMagic;
    header.version = proto::kVersion;
    header.bodyLen = static_cast<uint32_t>(message.body.size());

    std::string frame;
    frame.resize(proto::kHeaderSize + message.body.size());
    proto::encodeHeader(header, frame.data());
    if (!message.body.empty()) {
        std::memcpy(frame.data() + proto::kHeaderSize, message.body.data(), message.body.size());
    }
    return frame;
}

void RpcFramer::decode(Buffer* buffer, const MessageCallback& cb) {
    while (buffer->readableBytes() >= proto::kHeaderSize) {
        RpcHeader header;
        if (!proto::decodeHeader(buffer->peek(), &header)) {
            // magic / version mismatch: not our protocol on this connection.
            LOG_ERROR("RpcFramer::decode: protocol mismatch, draining buffer");
            buffer->retrieveAll();
            return;
        }

        if (header.bodyLen > proto::kMaxBodyLen) {
            LOG_ERROR("RpcFramer::decode: frame too large: " + std::to_string(header.bodyLen));
            buffer->retrieveAll();
            return;
        }

        if (buffer->readableBytes() < proto::kHeaderSize + header.bodyLen) {
            return;  // incomplete frame, wait for more data
        }

        buffer->retrieve(proto::kHeaderSize);
        Message msg;
        msg.header = header;
        if (header.bodyLen > 0) {
            msg.body = buffer->retrieveAsString(header.bodyLen);
        }
        if (cb) {
            cb(msg);
        }
    }
}
