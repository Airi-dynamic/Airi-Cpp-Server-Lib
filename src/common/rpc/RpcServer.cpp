#include "rpc/RpcServer.h"
#include "net/Connection.h"
#include <string>

struct RpcConnCtx {
    std::string buf; // 已收到但未解析完整帧的字节数据
};

RpcServer::RpcServer(const std::string &ip, uint16_t port, int ioThreads)
    : server_([&] {
          TcpServer::Options opt;
          opt.listenIp = ip;
          opt.listenPort = port;
          opt.ioThreads = ioThreads;
          return opt;
      }()) {
    server_.newConnect([this](Connection *conn) { onNewConn(conn); });
    server_.onMessage([this](Connection *conn) { onMessage(conn); });
}

void RpcServer::addHandler(const std::string &method, Handler handler) {
    handlers_[method] = std::move(handler);
}

void RpcServer::start() { server_.start(); }
void RpcServer::stop() { server_.stop(); }

void RpcServer::onNewConn(Connection *conn) {
    conn->setContext(RpcConnCtx{});
}

void RpcServer::onMessage(Connection *conn) {
    auto *ctx = conn->getContextAs<RpcConnCtx>();

    Buffer *buf = conn->getInputBuffer();
    size_t n = buf->readableBytes();
    ctx->buf.append(buf->peek(), n);
    buf->retrieve(n);

    while (true) {
        RpcMessage msg;
        int consumed = 0;
        bool ok = RpcMessage::decode(ctx->buf.data(), static_cast<int>(ctx->buf.size()), &msg,
                                     &consumed);
        if(!ok) break;
        ctx->buf.erase(0, consumed);
        if(msg.type != RpcMessage::Type::kRequest) continue;
        std::string responsePayload = "{\"error\":\"unknown method\"}";
        auto it = handlers_.find(msg.method);
        if(it != handlers_.end()){
            responsePayload = it->second(msg.payload);
        }
        RpcMessage resp;
        resp.type    = RpcMessage::Type::kResponse;
        resp.reqId   = msg.reqId;
        resp.method  = msg.method;
        resp.payload = responsePayload;
        conn->send(resp.encode());
    }
}