#include "rpc/RpcServer.h"
#include "net/Connection.h"
#include "net/EventLoop.h"
#include "log/Logger.h"
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace {
// RPC 帧 payload 上限：超过即视为协议错误，关闭连接，防止恶意 length 让缓冲无界膨胀。
constexpr uint32_t kMaxRpcPayloadBytes = 16u * 1024u * 1024u; // 16 MiB
} // namespace

struct RpcConnCtx {
    std::string buf; // 已收到但未解析完整帧的字节数据
};

RpcServer::RpcServer(const std::string &ip, uint16_t port, int ioThreads)
    : server_([&] {
          TcpServer::Options opt;
          opt.listenIp   = ip;
          opt.listenPort = port;
          opt.ioThreads  = ioThreads;
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

void RpcServer::onNewConn(Connection *conn) { conn->setContext(RpcConnCtx{}); }

void RpcServer::onMessage(Connection *conn) {
    auto *ctx = conn->getContextAs<RpcConnCtx>();

    Buffer *buf = conn->getInputBuffer();
    size_t  n   = buf->readableBytes();
    ctx->buf.append(buf->peek(), n);
    buf->retrieve(n);

    while (true) {
        // ── 防御性：在 decode 前先 peek 头 4 字节的 length，拒绝过大的帧 ──────
        // 旧版直接 decode，恶意客户端可以声明 length=4GB 让 ctx->buf 无界膨胀。
        if (ctx->buf.size() >= 4) {
            uint32_t netLen = 0;
            std::memcpy(&netLen, ctx->buf.data(), 4);
            uint32_t payloadLen = ntohl(netLen);
            if (payloadLen > kMaxRpcPayloadBytes) {
                LOG_WARN << "[RpcServer] 拒绝过大帧并关闭连接 fd=" << conn->getSocket()->getFd()
                         << " claimed=" << payloadLen << " limit=" << kMaxRpcPayloadBytes;
                conn->close();
                return;
            }
        }

        RpcMessage msg;
        int        consumed = 0;
        bool       ok = RpcMessage::decode(ctx->buf.data(), static_cast<int>(ctx->buf.size()), &msg,
                                           &consumed);
        if (!ok) break;
        ctx->buf.erase(0, consumed);
        if (msg.type != RpcMessage::Type::kRequest) continue;

        auto it = handlers_.find(msg.method);
        if (it == handlers_.end()) {
            // 未注册方法：立即同步回包
            RpcMessage resp;
            resp.type    = RpcMessage::Type::kResponse;
            resp.reqId   = msg.reqId;
            resp.method  = msg.method;
            resp.payload = R"({"error":"unknown method"})";
            conn->send(resp.encode());
            continue;
        }

        // ── 构造 done 回调：handler 完成后调用即可把响应写回 ──────────────────
        // 关键点：
        //   1) done 可在任意线程调用（典型场景：handler 把任务投递到自己的 loop_，
        //      处理完后在 loop_ 线程调 done）。
        //   2) 写回 conn 必须在 conn 的 owning loop 线程，因此 done 内 runInLoop。
        //   3) 此时连接可能已关闭——通过 aliveFlag (weak_ptr<bool>) 安全判活，
        //      避免 use-after-free。
        Eventloop          *connLoop  = conn->getLoop();
        std::weak_ptr<bool> aliveWeak = conn->aliveFlag();
        const uint32_t      reqId     = msg.reqId;
        std::string         method    = msg.method;
        Connection         *connPtr   = conn;

        Done done = [connLoop, aliveWeak, reqId, method = std::move(method),
                     connPtr](std::string responsePayload) mutable {
            connLoop->runInLoop([connPtr, aliveWeak, reqId, method = std::move(method),
                                 payload = std::move(responsePayload)]() mutable {
                auto a = aliveWeak.lock();
                if (!a || !*a) return; // 连接已销毁，丢弃响应
                RpcMessage resp;
                resp.type    = RpcMessage::Type::kResponse;
                resp.reqId   = reqId;
                resp.method  = std::move(method);
                resp.payload = std::move(payload);
                connPtr->send(resp.encode());
            });
        };

        // handler 立刻返回；sub-reactor 继续解下一帧，永不阻塞。
        it->second(msg.payload, std::move(done));
    }
}
