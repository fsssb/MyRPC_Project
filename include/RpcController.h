#ifndef MYRPCPROJECT_INCLUDE_RPCCONTROLLER_H_
#define MYRPCPROJECT_INCLUDE_RPCCONTROLLER_H_

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <string>

// Per-call context, following the classic RpcController pattern: the caller
// configures the target method and deadline, then reads the result status
// after the call completes (see docs/v2-design-draft.md section 1.4).
//
// The RPC total timeout is a single deadline: when it expires the call fails
// with kDeadlineExceeded and is never retried by the framework. A late
// response for an expired call is discarded by the channel.
class RpcController {
public:
    void setMethod(const std::string& service, const std::string& method) {
        methodKey_ = service + "." + method;
        methodId_ = proto::methodIdOf(methodKey_.c_str());
    }

    // Total RPC timeout in milliseconds; 0 means no deadline (not recommended).
    void setTimeoutMs(uint32_t ms) { timeoutMs_ = ms; }

    // Optional routing key for hash-based load balancers (V2.1).
    void setRequestKey(uint32_t key) { requestKey_ = key; }
    uint32_t requestKey() const { return requestKey_; }

    uint32_t methodId() const { return methodId_; }
    uint32_t timeoutMs() const { return timeoutMs_; }
    bool hasDeadline() const { return timeoutMs_ > 0; }

    proto::Status status() const { return status_; }
    const std::string& errorText() const { return errorText_; }

    // Called by the channel when the call finishes.
    void setStatus(proto::Status status, std::string errorText) {
        status_ = status;
        errorText_ = std::move(errorText);
    }

private:
    std::string methodKey_;
    uint32_t methodId_{0};
    uint32_t timeoutMs_{0};
    uint32_t requestKey_{0};
    proto::Status status_{proto::kOk};
    std::string errorText_;
};

#endif  // MYRPCPROJECT_INCLUDE_RPCCONTROLLER_H_
