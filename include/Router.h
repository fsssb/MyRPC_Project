#ifndef MYRPCPROJECT_INCLUDE_ROUTER_H_
#define MYRPCPROJECT_INCLUDE_ROUTER_H_

#include "Protocol.h"
#include "Serializer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

// Maps method_id (FNV-1a32 of "service.method") to a handler.
//
// Handlers are invoked with the decoded request value; they fill *response and
// eventually call done(status). The done callback may be invoked from any
// thread (e.g. an async worker), matching the V1 cross-thread write-back model:
// RpcServer only touches the connection through weak_ptr + queueInLoop.
class Router {
public:
    using Done = std::function<void(proto::Status status)>;
    using Handler = std::function<void(const Value& request, Value* response, Done done)>;

    // Register "service.method"; a duplicate method_id (FNV-1a32 collision)
    // overwrites the previous entry and is logged.
    void registerMethod(const std::string& service, const std::string& method, Handler handler);

    // Returns false when no handler is registered for the method id.
    bool find(uint32_t methodId, Handler* out) const;

    // Human-readable "service.method" for the id, empty when unknown.
    std::string methodName(uint32_t methodId) const;

private:
    std::unordered_map<uint32_t, Handler> handlers_;
    std::unordered_map<uint32_t, std::string> names_;
};

inline void Router::registerMethod(const std::string& service, const std::string& method,
                                   Handler handler) {
    const std::string key = service + "." + method;
    const uint32_t id = proto::methodIdOf(key.c_str());
    handlers_[id] = std::move(handler);
    names_[id] = key;
}

inline bool Router::find(uint32_t methodId, Handler* out) const {
    const auto it = handlers_.find(methodId);
    if (it == handlers_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

inline std::string Router::methodName(uint32_t methodId) const {
    const auto it = names_.find(methodId);
    return it == names_.end() ? std::string() : it->second;
}

#endif  // MYRPCPROJECT_INCLUDE_ROUTER_H_
