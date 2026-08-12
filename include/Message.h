#ifndef MYRPCPROJECT_INCLUDE_MESSAGE_H_
#define MYRPCPROJECT_INCLUDE_MESSAGE_H_

#include "Protocol.h"

#include <string>

// An application message: 20-byte RPC header plus raw body bytes. The body of a
// business call is the serialized payload; heartbeat frames carry an empty body.
struct Message {
    RpcHeader header;
    std::string body;
};

#endif  // MYRPCPROJECT_INCLUDE_MESSAGE_H_
