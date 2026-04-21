#include "http/HttpServer.h"
#include "Connection.h"
#include "EventLoop.h"
#include "http/HttpContext.h"
#include "log/Logger.h"
#include "timer/TimeStamp.h"
#include <functional>

HttpServer::HttpServer() : server_(std::make_unique<TcpServer>()) {
    // 注册 TCP 层回调，桥接到 HTTP 处理逻辑
    server_->newConnect(std::bind(&HttpServer::onNewConnection, this, std::placeholders::_1));
    server_->onMessage(std::bind(&HttpServer::onMessage, this, std::placeholders::_1));

    // 若用户没有设置业务回调，使用默认 404 处理
    httpCallback_ = std::bind(&HttpServer::defaultCallback, this,
                              std::placeholders::_1, std::placeholders::_2);
}

void HttpServer::start() { server_->Start(); }
void HttpServer::stop() { server_->stop(); }

// ── 新连接建立：为每条连接创建独立的 HttpContext ─────────────────────────────
void HttpServer::onNewConnection(Connection *conn) {
    conn->setContext(HttpContext{});
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
            LOG_INFO << "[HttpServer] idle timeout, closing fd="
                     << conn->getSocket()->getFd();
            conn->close();
        }
    });
}

// ── 数据到达：调用 HttpContext 状态机解析 ─────────────────────────────────────
void HttpServer::onMessage(Connection *conn) {
    if (conn->getState() != Connection::State::kConnected)
        return;

    HttpContext *ctx = conn->getContextAs<HttpContext>();
    if (!ctx) {
        // 上下文不存在（理论上不应发生），视为非法连接
        conn->send("HTTP/1.1 500 Internal Server Error\r\n\r\n");
        conn->close();
        return;
    }

    // 每次收到数据即更新活跃时间（供空闲超时计时器参考）
    if (autoClose_)
        conn->touchLastActive();

    Buffer *buf = conn->getInputBuffer();
    const char *data = buf->peek();
    int len = static_cast<int>(buf->readableBytes());

    if (!ctx->parse(data, len)) {
        // 报文格式非法
        LOG_WARN << "[HttpServer] bad request, fd=" << conn->getSocket()->getFd();
        conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
        conn->close();
        return;
    }

    // 消耗掉已经被状态机读取的字节
    buf->retrieve(buf->readableBytes());

    if (ctx->isComplete()) {
        onRequest(conn, ctx->request());
        ctx->reset(); // 准备解析下一个请求（keep-alive）
    }
}

// ── 完整请求到达：调用业务回调，序列化并发送响应 ───────────────────────────────
void HttpServer::onRequest(Connection *conn, const HttpRequest &req) {
    const std::string connHeader = req.header("Connection");

    // HTTP/1.0 默认关闭；HTTP/1.1 默认 keep-alive，除非显式 "Connection: close"
    bool close = (connHeader == "close") ||
                 (req.version() == HttpRequest::Version::kHttp10 &&
                  connHeader != "keep-alive");

    HttpResponse resp(close);
    httpCallback_(req, &resp);

    LOG_INFO << "[HttpServer] " << req.methodString() << " " << req.url()
             << " -> " << static_cast<int>(resp.closeConnection() ? 0 : 200);

    conn->send(resp.serialize());

    if (resp.closeConnection())
        conn->close();
}

void HttpServer::defaultCallback(const HttpRequest & /*req*/, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
    resp->setContentType("text/plain");
    resp->setCloseConnection(true);
    resp->setBody("404 Not Found\n");
}
