#include "http/HttpServer.h"
#include "Connection.h"
#include "EventLoop.h"
#include "http/HttpContext.h"
#include "log/Logger.h"
#include "timer/TimeStamp.h"
#include <cmath>
#include <functional>

namespace {

struct HttpConnectionContext {
    explicit HttpConnectionContext(const HttpContext::Limits &limits) : parser(limits) {}

    HttpContext parser;
    bool requestInProgress{false};
    TimeStamp requestStart;
};

constexpr double kMicrosecondsToSeconds = 1000.0 * 1000.0;

} // namespace

HttpServer::HttpServer() : HttpServer(Options{}) {}

HttpServer::HttpServer(const Options &options) : server_(std::make_unique<TcpServer>(options.tcp)) {
    autoClose_ = options.autoClose;
    idleTimeout_ = options.idleTimeoutSec > 0.0 ? options.idleTimeoutSec : 60.0;
    requestTimeoutSec_ = options.requestTimeoutSec > 0.0 ? options.requestTimeoutSec : 15.0;
    limits_ = options.limits;

    // 注册 TCP 层回调，桥接到 HTTP 处理逻辑
    server_->newConnect(std::bind(&HttpServer::onNewConnection, this, std::placeholders::_1));
    server_->onMessage(std::bind(&HttpServer::onMessage, this, std::placeholders::_1));

    // 若用户没有设置业务回调，使用默认 404 处理
    httpCallback_ =
        std::bind(&HttpServer::defaultCallback, this, std::placeholders::_1, std::placeholders::_2);
}

void HttpServer::start() { server_->Start(); }
void HttpServer::stop() { server_->stop(); }

std::string HttpServer::makeRouteKey(HttpRequest::Method method, const std::string &path) {
    return std::to_string(static_cast<int>(method)) + " " + path;
}

void HttpServer::addRoute(HttpRequest::Method method, const std::string &path,
                          RouteHandler handler) {
    routes_[makeRouteKey(method, path)] = std::move(handler);
}

void HttpServer::addPrefixRoute(HttpRequest::Method method, const std::string &prefix,
                                RouteHandler handler) {
    prefixRoutes_.push_back(PrefixRoute{method, prefix, std::move(handler)});
}

// ── 新连接建立：为每条连接创建独立的 HttpContext ─────────────────────────────
void HttpServer::onNewConnection(Connection *conn) {
    conn->setContext(HttpConnectionContext{limits_});
    conn->touchLastActive(); // 记录连接建立时间作为初始活跃时间

    if (autoClose_)
        scheduleIdleClose(conn);

    LOG_INFO << "[HttpServer] new connection fd=" << conn->getSocket()->getFd();
}

// ── 空闲超时定时器（递归调度）────────────────────────────────────────────────
// 安全模型：
//   - aliveFlag 是 weak_ptr<bool>，Connection 析构时将 *alive 置 false
//   - 定时器与 Connection 共属同一个 sub-reactor EventLoop（单线程），无竞态
//   - 若到期时连接已关闭（*alive==false），lambda 直接返回，不会出现悬空访问
void HttpServer::scheduleIdleClose(Connection *conn) {
    std::weak_ptr<bool> weak = conn->aliveFlag();
    double timeout = idleTimeout_;

    conn->getLoop()->runAfter(timeout, [weak, conn, this]() {
        // 尝试锁定 alive flag；若 Connection 已析构，lock() 返回 nullptr
        auto alive = weak.lock();
        if (!alive || !*alive)
            return; // 连接已经消失，无需处理

        // 比对最后活跃时间
        TimeStamp deadline = TimeStamp::addSeconds(conn->lastActive(), idleTimeout_);
        if (TimeStamp::now() < deadline) {
            // 连接仍然活跃，重新调度下一次检测
            scheduleIdleClose(conn);
        } else {
            LOG_INFO << "[HttpServer] idle timeout, closing fd=" << conn->getSocket()->getFd();
            conn->close();
        }
    });
}

// ── 数据到达：调用 HttpContext 状态机解析 ─────────────────────────────────────
void HttpServer::onMessage(Connection *conn) {
    if (conn->getState() != Connection::State::kConnected)
        return;

    HttpConnectionContext *ctx = conn->getContextAs<HttpConnectionContext>();
    if (!ctx) {
        conn->setContext(HttpConnectionContext{limits_});
        ctx = conn->getContextAs<HttpConnectionContext>();
        if (!ctx) {
            conn->send("HTTP/1.1 500 Internal Server Error\r\n\r\n");
            conn->close();
            return;
        }
    }

    // 每次收到数据即更新活跃时间（供空闲超时计时器参考）
    if (autoClose_)
        conn->touchLastActive();

    Buffer *buf = conn->getInputBuffer();
    // 关键修复：同一个 TCP 包中可能包含多个 HTTP 请求（pipeline / 粘包）。
    // 逐轮 parse + 按“实际消费字节”retrieve，避免把后续请求误清空。
    while (buf->readableBytes() > 0) {
        if (!ctx->requestInProgress) {
            ctx->requestInProgress = true;
            ctx->requestStart = TimeStamp::now();
        }

        const double elapsedSec = static_cast<double>(TimeStamp::now().microseconds() -
                                                      ctx->requestStart.microseconds()) /
                                  kMicrosecondsToSeconds;
        if (elapsedSec > requestTimeoutSec_) {
            LOG_WARN << "[HttpServer] 请求解析超时，fd=" << conn->getSocket()->getFd()
                     << " elapsedSec=" << elapsedSec;
            conn->send(
                "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            conn->close();
            return;
        }

        int consumed = 0;
        if (!ctx->parser.parse(buf->peek(), static_cast<int>(buf->readableBytes()), &consumed)) {
            LOG_WARN << "[HttpServer] 非法请求，fd=" << conn->getSocket()->getFd();
            if (ctx->parser.payloadTooLarge()) {
                conn->send("HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\nContent-Length: "
                           "0\r\n\r\n");
            } else {
                conn->send(
                    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            }
            conn->close();
            return;
        }

        if (consumed > 0) {
            buf->retrieve(static_cast<size_t>(consumed));
        } else {
            // 理论上 len>0 时应至少消费 1 字节。该保护用于规避异常输入导致的死循环。
            LOG_WARN << "[HttpServer] 解析未前进，等待更多数据，fd=" << conn->getSocket()->getFd();
            break;
        }

        // 只解析到半包时先退出，等待下一个 read 事件继续。
        if (!ctx->parser.isComplete())
            break;

        if (!onRequest(conn, ctx->parser.request()))
            return;
        ctx->parser.reset();
        ctx->requestInProgress = false;
    }
}

// ── 完整请求到达：调用业务回调，序列化并发送响应 ───────────────────────────────
bool HttpServer::onRequest(Connection *conn, const HttpRequest &req) {
    const std::string connHeader = req.header("Connection");

    // HTTP/1.0 默认关闭；HTTP/1.1 默认 keep-alive，除非显式 "Connection: close"
    bool close = (connHeader == "close") ||
                 (req.version() == HttpRequest::Version::kHttp10 && connHeader != "keep-alive");

    HttpResponse resp(close);

    auto dispatch = [&]() {
        auto it = routes_.find(makeRouteKey(req.method(), req.url()));
        if (it != routes_.end()) {
            it->second(req, &resp);
            return;
        }
        for (const auto &route : prefixRoutes_) {
            if (route.method == req.method() &&
                req.url().compare(0, route.prefix.size(), route.prefix) == 0) {
                route.handler(req, &resp);
                return;
            }
        }
        httpCallback_(req, &resp);
    };

    size_t index = 0;
    std::function<void()> runChain = [&]() {
        if (index < middlewares_.size()) {
            auto &mw = middlewares_[index++];
            mw(req, &resp, runChain);
            return;
        }
        dispatch();
    };
    runChain();

    LOG_INFO << "[HttpServer] " << req.methodString() << " " << req.url() << " -> "
             << static_cast<int>(resp.statusCode());

    conn->send(resp.serialize());

    if (resp.hasSendFile()) {
        conn->sendFile(resp.sendFilePath(), resp.sendFileOffset(), resp.sendFileCount());
    }

    if (resp.closeConnection()) {
        conn->close();
        return false;
    }
    return true;
}

void HttpServer::defaultCallback(const HttpRequest & /*req*/, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
    resp->setContentType("text/plain");
    resp->setCloseConnection(true);
    resp->setBody("404 Not Found\n");
}
