#include "RpcServer.h"

#include "EventLoop.h"
#include "TcpConnection.h"

#include <chrono>
#include <utility>

RpcServer::RpcServer(EventLoop* loop, const std::string& ip, uint16_t port, int threadNum)
    : tcpServer_(loop, ip, port, threadNum) {
    tcpServer_.setMessageCallback([this](const std::shared_ptr<TcpConnection>& conn, const Message& req) {
        onMessage(conn, req);
    });
}

void RpcServer::registerMethod(const std::string& service, const std::string& method,
                               Router::Handler handler) {
    router_.registerMethod(service, method, std::move(handler));
}

void RpcServer::setExecutor(std::function<void(std::function<void()>)> executor) {
    executor_ = std::move(executor);
}

void RpcServer::start() {
    tcpServer_.start();
}

void RpcServer::stop() {
    tcpServer_.stop();
}

std::size_t RpcServer::pendingTaskSize() const {
    return tcpServer_.pendingTaskSize();
}

void RpcServer::onMessage(const std::shared_ptr<TcpConnection>& conn, const Message& req) {
    switch (req.header.msgType) {
        case proto::kMsgHeartbeat: {
            // Answer heartbeats inline (they must not touch business logic).
            Message ack;
            ack.header = req.header;
            ack.header.msgType = proto::kMsgHeartbeatAck;
            conn->sendMessage(ack);
            return;
        }
        case proto::kMsgRequest:
        case proto::kMsgOneway:
            break;
        default:
            return;  // ignore unexpected frame types
    }

    // Keep handlers off the I/O threads; the default executor is the TcpServer
    // task queue (main loop pending functors), same as the V1 demo.
    std::function<void(std::function<void()>)> dispatch = executor_;
    if (!dispatch) {
        dispatch = [this](std::function<void()> task) { tcpServer_.submitTask(std::move(task)); };
    }
    auto self = this;
    dispatch([self, conn, req]() { self->handleRequest(conn, req); });
}

void RpcServer::handleRequest(const std::shared_ptr<TcpConnection>& conn, const Message& req) {
    auto ctx = std::make_shared<RequestContext>();
    ctx->header = req.header;
    ctx->start = std::chrono::steady_clock::now();

    if (!req.body.empty()) {
        if (!Serializer::decode(req.body, &ctx->request)) {
            LOG_ERROR("RpcServer: serialization error for method_id=" + std::to_string(req.header.methodId));
            sendResponse(conn, ctx, proto::kSerializationError);
            return;
        }
    }

    Router::Handler handler;
    if (!router_.find(req.header.methodId, &handler)) {
        LOG_ERROR("RpcServer: method not found: id=" + std::to_string(req.header.methodId));
        sendResponse(conn, ctx, proto::kMethodNotFound);
        return;
    }

    const bool reply = req.header.msgType != proto::kMsgOneway;
    const auto weakConn = std::weak_ptr<TcpConnection>(conn);
    handler(ctx->request, &ctx->response,
            [weakConn, ctx, reply](proto::Status status) {
                // Server-side deadline: the handler exceeded the client's
                // timeout, so the response would arrive too late.
                if (ctx->header.timeoutMs > 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - ctx->start);
                    if (elapsed.count() >= static_cast<int64_t>(ctx->header.timeoutMs)) {
                        status = proto::kDeadlineExceeded;
                    }
                }
                if (!reply) {
                    return;  // oneway calls never receive a response
                }
                if (const auto locked = weakConn.lock()) {
                    Message resp;
                    resp.header = ctx->header;
                    resp.header.msgType = proto::kMsgResponse;
                    resp.header.status = status;
                    if (status == proto::kOk) {
                        resp.body = Serializer::encode(ctx->response);
                    }
                    locked->sendMessage(resp);
                }
            });
}

void RpcServer::sendResponse(const std::shared_ptr<TcpConnection>& conn,
                             const std::shared_ptr<RequestContext>& ctx, proto::Status status) {
    if (ctx->header.msgType == proto::kMsgOneway) {
        return;
    }
    Message resp;
    resp.header = ctx->header;
    resp.header.msgType = proto::kMsgResponse;
    resp.header.status = status;
    conn->sendMessage(resp);
}
