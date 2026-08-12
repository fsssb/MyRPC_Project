#ifndef MYRPCPROJECT_INCLUDE_PROTOCOL_H_
#define MYRPCPROJECT_INCLUDE_PROTOCOL_H_

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// V2.0 wire protocol: a 20-byte fixed header (network byte order) followed by
// the serialized body. This is the V1 Length-Prefix framing upgraded to carry
// RPC semantics (see docs/v2-design-draft.md section 1.2).
//
//   byte  0-1    magic       u16   "MP" (0x4D50) fast protocol identification
//   byte  2      version     u8    protocol version, currently 1
//   byte  3      flags       u8    bit0=body compressed  bit1=attachment  bit2=trace
//   byte  4      msg_type    u8    0=request 1=response 2=oneway 3=heartbeat 4=heartbeat-ack
//   byte  5-6    status      u16   0 for requests; error code in responses
//   byte  7-10   request_id  u32   correlation id, echoed back by the server
//   byte 11-14   method_id   u32   FNV-1a32(service.method) for fast routing
//   byte 15-18   body_len    u32   serialized body bytes (excluding the 20B header)
//   byte 19     reserved    u8    reserved for future protocol extensions
//
// body_len semantics: bytes of the serialized body, the 20-byte header excluded.

struct RpcHeader {
    uint16_t magic{0};
    uint8_t version{0};
    uint8_t flags{0};
    uint8_t msgType{0};
    uint16_t status{0};
    uint32_t requestId{0};
    uint32_t methodId{0};
    uint32_t bodyLen{0};
    uint8_t reserved{0};
};

namespace proto {

constexpr uint16_t kMagic = 0x4D50;
constexpr uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 20;
constexpr uint32_t kMaxBodyLen = 64 * 1024 * 1024;  // 64 MiB; larger frames are rejected

// Message types.
constexpr uint8_t kMsgRequest = 0;
constexpr uint8_t kMsgResponse = 1;
constexpr uint8_t kMsgOneway = 2;
constexpr uint8_t kMsgHeartbeat = 3;
constexpr uint8_t kMsgHeartbeatAck = 4;

// Header flags.
constexpr uint8_t kFlagCompressed = 0x01;
constexpr uint8_t kFlagAttachment = 0x02;
constexpr uint8_t kFlagTrace = 0x04;

// Status codes (see docs/v2-design-draft.md section 1.2.3). Values 8/9 are
// reserved for V2.1 graceful shutdown and server-side limiting.
enum Status : uint16_t {
    kOk = 0,
    kInvalidArgument = 1,
    kMethodNotFound = 2,
    kSerializationError = 3,
    kInternalError = 4,
    kDeadlineExceeded = 5,
    kRequestTooLarge = 6,
    kUnknown = 7,
    kShuttingDown = 8,
    kConcurrencyLimited = 9,
};

// FNV-1a 32-bit hash used for method_id. 32 bits is enough for a single-process
// method table; collisions are practically impossible at this scale and an
// unknown method id is rejected with kMethodNotFound.
inline uint32_t fnv1a32(const char* data, std::size_t len) {
    uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 16777619u;
    }
    return hash;
}

inline uint32_t methodIdOf(const char* methodKey) {
    return fnv1a32(methodKey, std::strlen(methodKey));
}

// Encode the header into a 20-byte buffer (network byte order).
inline void encodeHeader(const RpcHeader& h, char* out) {
    uint16_t magic = htons(h.magic);
    uint16_t status = htons(h.status);
    uint32_t requestId = htonl(h.requestId);
    uint32_t methodId = htonl(h.methodId);
    uint32_t bodyLen = htonl(h.bodyLen);
    std::memcpy(out, &magic, 2);
    out[2] = h.version;
    out[3] = h.flags;
    out[4] = h.msgType;
    std::memcpy(out + 5, &status, 2);
    std::memcpy(out + 7, &requestId, 4);
    std::memcpy(out + 11, &methodId, 4);
    std::memcpy(out + 15, &bodyLen, 4);
    out[19] = h.reserved;
}

// Decode a 20-byte buffer into host byte order. Returns false when the magic or
// version does not match this protocol.
inline bool decodeHeader(const char* in, RpcHeader* h) {
    uint16_t magic = 0;
    uint16_t status = 0;
    uint32_t requestId = 0;
    uint32_t methodId = 0;
    uint32_t bodyLen = 0;
    std::memcpy(&magic, in, 2);
    std::memcpy(&status, in + 5, 2);
    std::memcpy(&requestId, in + 7, 4);
    std::memcpy(&methodId, in + 11, 4);
    std::memcpy(&bodyLen, in + 15, 4);
    h->magic = ntohs(magic);
    h->version = in[2];
    h->flags = in[3];
    h->msgType = in[4];
    h->status = ntohs(status);
    h->requestId = ntohl(requestId);
    h->methodId = ntohl(methodId);
    h->bodyLen = ntohl(bodyLen);
    h->reserved = in[19];
    return h->magic == kMagic && h->version == kVersion;
}

}  // namespace proto

#endif  // MYRPCPROJECT_INCLUDE_PROTOCOL_H_
