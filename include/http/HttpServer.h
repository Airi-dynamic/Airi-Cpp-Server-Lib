#pragma once
#include "Macros.h"
#include "TcpServer.h"
#include "http/HttpContext.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// HttpServer：在 TcpServer 之上封装 HTTP/1.x 协议处理。
//
// 职责划分：
//   TcpServer  ← 负责 accept / IO / 多 Reactor / 连接生命周期
//   HttpServer ← 负责 HTTP 报文解析（HttpContext）、路由分发（HttpCallback）
//
// 使用方式：
//   HttpServer srv;
//   srv.setHttpCallback([](const HttpRequest& req, HttpResponse* resp) {
//       resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
//       resp->setContentType("text/plain");
//       resp->setBody("Hello World\n");
//   });
//   srv.start();
class Connection;

class HttpServer {
  public:
    DISALLOW_COPY_AND_MOVE(HttpServer)

    struct Options {
        TcpServer::Options tcp;
        bool autoClose{false};
        double idleTimeoutSec{60.0};
        double requestTimeoutSec{15.0};
        HttpContext::Limits limits{};
    };

    // callback 类型：用户处理函数签名，收到一个完整 HttpRequest，填写 HttpResponse
    using HttpCallback = std::function<void(const HttpRequest &, HttpResponse *)>;
    using RouteHandler = HttpCallback;
    using MiddlewareNext = std::function<void()>;
    using Middleware =
        std::function<void(const HttpRequest &, HttpResponse *, const MiddlewareNext &)>;

    HttpServer();
    explicit HttpServer(const Options &options);
    ~HttpServer() = default;

    // 设置业务回调（未设置时返回 404）
    void setHttpCallback(HttpCallback cb) { httpCallback_ = std::move(cb); }

    // 路由表：优先精确匹配 method + path，未命中时再走 prefix 匹配。
    void addRoute(HttpRequest::Method method, const std::string &path, RouteHandler handler);
    void addPrefixRoute(HttpRequest::Method method, const std::string &prefix,
                        RouteHandler handler);

    // 中间件链：按注册顺序执行，middleware 内部不调用 next() 可中断后续链路。
    void use(Middleware middleware) { middlewares_.emplace_back(std::move(middleware)); }

    // 最大连接数保护（透传到 TcpServer）
    void setMaxConnections(size_t maxConnections) { server_->setMaxConnections(maxConnections); }

    // ── Day21：空闲连接自动关闭 ──────────────────────────────────────────────
    // 启用后，若某连接在 timeoutSec 秒内没有任何数据交互，服务器主动关闭它。
    // 实现原理：新连接建立时在其归属的 sub-reactor 上调度一个定时器，
    //   定时器回调通过 weak_ptr<bool> alive flag 判断连接是否仍然存活，
    //   再比对 lastActive 时间戳决定是否关闭。
    void setAutoClose(bool enable, double timeoutSec = 60.0) {
        autoClose_ = enable;
        idleTimeout_ = timeoutSec;
    }

    // 启动服务器（阻塞，直到 stop() 被调用）
    void start();
    void stop();

  private:
    struct PrefixRoute {
        HttpRequest::Method method{HttpRequest::Method::kInvalid};
        std::string prefix;
        RouteHandler handler;
    };

    void onNewConnection(Connection *conn);
    void onMessage(Connection *conn);
    // 返回 true 表示连接可继续处理后续请求；false 表示已进入关闭流程。
    bool onRequest(Connection *conn, const HttpRequest &req);

    // 默认响应：404 Not Found
    void defaultCallback(const HttpRequest &req, HttpResponse *resp);

    // 为 conn 调度一个空闲超时检测定时器（递归调度，直到连接关闭）
    void scheduleIdleClose(Connection *conn);

    static std::string makeRouteKey(HttpRequest::Method method, const std::string &path);

    std::unique_ptr<TcpServer> server_;
    HttpCallback httpCallback_;

    std::vector<Middleware> middlewares_;
    std::vector<PrefixRoute> prefixRoutes_;
    std::unordered_map<std::string, RouteHandler> routes_;

    HttpContext::Limits limits_{};
    double requestTimeoutSec_{15.0};

    bool autoClose_{false};
    double idleTimeout_{60.0}; // 秒
};
