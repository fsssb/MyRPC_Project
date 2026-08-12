#ifndef MYRPCPROJECT_INCLUDE_RPCFRAMER_H_
#define MYRPCPROJECT_INCLUDE_RPCFRAMER_H_

#include "Buffer.h"
#include "Message.h"

#include <cstddef>
#include <functional>
#include <string>

// Encodes / decodes the V2.0 wire format: [20-byte RpcHeader][body bytes].
//
// The framer keeps the V1 length-prefix semantics (TCP packet splitting and
// coalescing): an incomplete frame stays in the input buffer waiting for more
// bytes, and a single read event may produce multiple complete frames. Frames
// larger than proto::kMaxBodyLen are rejected: the buffer is drained and the
// caller is expected to close the connection.
class RpcFramer {
public:
    using MessageCallback = std::function<void(const Message&)>;

    // Encode a message into a wire frame. The magic/version fields are forced
    // to the current protocol values regardless of what the caller set.
    // Returns an empty string when the body exceeds kMaxBodyLen.
    static std::string encode(const Message& message);

    // Decode all complete frames from the buffer, invoking cb per frame.
    static void decode(Buffer* buffer, const MessageCallback& cb);
};

#endif  // MYRPCPROJECT_INCLUDE_RPCFRAMER_H_
