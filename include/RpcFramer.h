#ifndef MYRPCPROJECT_INCLUDE_CODEC_H_
#define MYRPCPROJECT_INCLUDE_CODEC_H_

#include "Buffer.h"
#include "Message.h"

#include <cstddef>
#include <functional>
#include <string>

class Codec {
public:
    using MessageCallback = std::function<void(const Message&)>;

    static std::string encode(const Message& message);
    static void decode(Buffer* buffer, const MessageCallback& cb);

private:
    static constexpr std::size_t kHeaderLen = 4;
    static constexpr std::size_t kMaxBodyLen = 64 * 1024 * 1024;
};

#endif  // MYRPCPROJECT_INCLUDE_CODEC_H_
