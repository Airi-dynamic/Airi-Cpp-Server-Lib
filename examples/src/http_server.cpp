// =============================================================================
//  Airi-Cpp-Server-Lib 全特性演示服务器
//
//  此文件演示 day30 收官状态下库的全部核心特性：
//    - 路由表（精确路径 + 前缀路径）
//    - 中间件链（洋葱模型）
//    - per-IP 令牌桶限流（429）
//    - Bearer Token / API Key 双模式鉴权（403）
//    - CORS 跨域（OPTIONS 预检 → 204）
//    - gzip 响应压缩
//    - 静态文件服务（ETag / Range / 304 / sendfile 零拷贝）
//    - Prometheus 风格 /metrics 端点
//    - 优雅关闭（SIGINT / SIGTERM）
//
//  启动方式：
//      ./examples/http_server                    # 默认 127.0.0.1:8888
//      MYCPPSERVER_BIND_PORT=9090 ./examples/http_server
//      MYCPPSERVER_BIND_IP=0.0.0.0 ./examples/http_server
//
//  快速验证脚本（curl）：
//      curl -i http://localhost:8888/                                       # 首页
//      curl -i http://localhost:8888/api/users                              # 鉴权失败 → 403
//      curl -i -H "Authorization: Bearer demo-token-2024" \
//           http://localhost:8888/api/users                                 # → 200
//      curl -i -H "X-API-Key: api-key-001" http://localhost:8888/api/users  # → 200
//      curl -i -X OPTIONS http://localhost:8888/api/users                   # 预检 → 204
//      curl -i -H "Accept-Encoding: gzip" http://localhost:8888/            # 压缩
//      curl -i http://localhost:8888/files/readme.txt                       # 静态文件
//      curl -i -H "Range: bytes=0-99" http://localhost:8888/files/readme.txt        # 206
//      curl -s http://localhost:8888/metrics                                # Prometheus
//
//  压力测试（触发限流）：
//      for i in $(seq 1 200); do
//          curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8888/
//      done | sort | uniq -c                                                # 200 与 429 混合
// =============================================================================

#include <http/AuthMiddleware.h>
#include <http/CorsMiddleware.h>
#include <http/GzipMiddleware.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
#include <http/RateLimiter.h>
#include <http/ServerMetrics.h>
#include <http/StaticFileHandler.h>
#include <log/Logger.h>
#include <net/SignalHandler.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>

// ── 配置 ──────────────────────────────────────────────────────────────────────
namespace {
constexpr const char *kStaticDir = "static"; // 静态资源目录（相对 CWD）
constexpr const char *kFilesDir = "files";   // 用户文件目录（相对 CWD）

// 演示用凭据（生产环境请使用环境变量 / 密钥管理服务）
constexpr const char *kDemoBearerToken = "demo-token-2024";
constexpr const char *kDemoApiKey = "api-key-001";
} // namespace

// ── 工具：读取整个文件到 string ──────────────────────────────────────────────
static bool readFile(const std::string &path, std::string *out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    *out = oss.str();
    return true;
}

// ── 路由处理器：首页（直接返回 static/index.html，前后端分离）─────────────
//   首次请求时读盘并缓存到内存；后续请求直接命中缓存。
//   静态资源全部位于 examples/static/，由 CMake POST_BUILD 复制到 build/static/。
static void handleIndex(const HttpRequest &, HttpResponse *resp) {
    static std::string cached;
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = readFile(std::string(kStaticDir) + "/index.html", &cached);
    });
    if (!ok) {
        resp->setStatus(HttpResponse::StatusCode::k500InternalServerError, "Internal Server Error");
        resp->setContentType("text/plain; charset=utf-8");
        resp->setBody("index.html not found under " + std::string(kStaticDir) +
                      "/. Please run ./app_demo from the build directory or "
                      "copy examples/static/ next to the binary.\n");
        return;
    }
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("text/html; charset=utf-8");
    resp->setBody(cached);
}

// ── 路由处理器：JSON API ─────────────────────────────────────────────────────
static void handleApiUsers(const HttpRequest &, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("application/json");
    resp->setBody(R"({"users":[
  {"id":1,"name":"Alice","role":"admin"},
  {"id":2,"name":"Bob","role":"user"},
  {"id":3,"name":"Charlie","role":"user"}
]})");
}

// ── 路由处理器：健康检查（白名单，无需鉴权）──────────────────────────────────
static void handleHealth(const HttpRequest &, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("application/json");
    resp->setBody(R"({"status":"ok"})");
}

// ── 路由处理器：Prometheus 指标（白名单，无需鉴权）───────────────────────────
static void handleMetrics(const HttpRequest &, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("text/plain; version=0.0.4; charset=utf-8");
    resp->setBody(ServerMetrics::instance().toPrometheus());
}

// ── 路由处理器：静态文件（前缀路由 /static/* → ./static/，/files/* → ./files/）
//   - 自动 MIME 推断
//   - ETag + If-None-Match → 304
//   - Range → 206 分段传输
//   - sendfile() 零拷贝（Linux）/ 降级 read+send（macOS）
static void handleStatic(const HttpRequest &req, HttpResponse *resp) {
    std::string url = req.url();
    std::string fsPath;
    if (url.compare(0, 8, "/static/") == 0) {
        fsPath = std::string(kStaticDir) + "/" + url.substr(8);
    } else if (url.compare(0, 7, "/files/") == 0) {
        fsPath = std::string(kFilesDir) + "/" + url.substr(7);
    } else {
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setBody("");
        return;
    }
    StaticFileHandler::Options opts;
    opts.cacheControl = "public, max-age=3600";
    opts.enableRange = true;
    bool found = StaticFileHandler::serve(req, resp, fsPath, opts);
    if (!found) {
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setContentType("text/plain");
        resp->setBody("File not found: " + url + "\n");
    }
}

// ── 中间件：访问日志 + Metrics 计时 ──────────────────────────────────────────
//   洋葱模型最外层：next() 之前打"入"日志，next() 之后打"出"日志和耗时
static HttpServer::Middleware makeAccessLogMiddleware() {
    return [](const HttpRequest &req, HttpResponse *resp,
              const HttpServer::MiddlewareNext &next) {
        auto start = std::chrono::steady_clock::now();
        next();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        ServerMetrics::instance().onRequestComplete(static_cast<int>(resp->statusCode()), elapsed);
        LOG_INFO << "[access] " << req.methodString() << ' ' << req.url()
                 << " -> " << static_cast<int>(resp->statusCode()) << ' ' << elapsed << "us";
    };
}

// ── 主程序 ────────────────────────────────────────────────────────────────────
int main() {
    Logger::setLogLevel(Logger::INFO);

    // ── HTTP Server 配置 ──────────────────────────────────────────────────────
    HttpServer::Options options;
    if (const char *envIp = std::getenv("MYCPPSERVER_BIND_IP")) {
        options.tcp.listenIp = envIp;
    }
    if (const char *envPort = std::getenv("MYCPPSERVER_BIND_PORT")) {
        options.tcp.listenPort = static_cast<uint16_t>(std::atoi(envPort));
    }
    options.tcp.ioThreads = 0;        // 自动按硬件线程数
    options.tcp.maxConnections = 10000;
    options.requestTimeoutSec = 15.0; // Slowloris 防护
    options.idleTimeoutSec = 60.0;
    options.autoClose = true;
    // 三层请求大小限流（防 OOM）
    options.limits.maxRequestLineBytes = 8 * 1024;
    options.limits.maxHeaderBytes = 16 * 1024;
    options.limits.maxBodyBytes = 8 * 1024 * 1024;

    HttpServer srv(options);

    // ── 中间件链（注册顺序 = 洋葱外层到内层）─────────────────────────────────
    //
    //   1. AccessLog       — 计时 + 日志（最外层）
    //   2. RateLimiter     — per-IP 令牌桶（早截断）
    //   3. CorsMiddleware  — 处理 OPTIONS 预检 / 注入 CORS 头（必须在 Auth 之前）
    //   4. AuthMiddleware  — Bearer / API Key（次内层）
    //   5. GzipMiddleware  — 响应压缩（最内层，next() 之后才能看到完整 body）
    //
    srv.use(makeAccessLogMiddleware());

    // 限流：每 IP 最多 200 个突发请求，稳态 100 RPS
    RateLimiter rateLimiter({/*capacity=*/200.0, /*refillRate=*/100.0});
    srv.use(rateLimiter.toMiddleware());

    // CORS：必须排在 Auth 之前，使 OPTIONS 预检请求能直接 204 返回
    CorsMiddleware cors;
    cors.allowOrigin("*")
        .allowMethods({"GET", "POST", "PUT", "DELETE", "OPTIONS"})
        .allowHeaders({"Content-Type", "Authorization", "X-API-Key"})
        .maxAge(86400);
    srv.use(cors.toMiddleware());

    // 鉴权：Bearer + API Key 双模式，/health /metrics /static/* 白名单
    AuthMiddleware auth;
    auth.addBearerToken(kDemoBearerToken);
    auth.addApiKey(kDemoApiKey);
    auth.addPublicPath("/");
    auth.addPublicPath("/health");
    auth.addPublicPath("/metrics");
    auth.addPublicPath("/favicon.ico");
    auth.addPublicPrefix("/static/");
    auth.addPublicPrefix("/files/");
    srv.use(auth.toMiddleware());

    // gzip：响应体 ≥ 256 字节 + Content-Type 可压缩 + 客户端支持 → 压缩
    GzipMiddleware gzip;
    gzip.setMinSize(256);
    srv.use(gzip.toMiddleware());

    // ── 路由表 ────────────────────────────────────────────────────────────────
    //   精确路径用 addRoute；前缀匹配用 addPrefixRoute
    using M = HttpRequest::Method;
    srv.addRoute(M::kGet, "/", handleIndex);
    srv.addRoute(M::kGet, "/health", handleHealth);
    srv.addRoute(M::kGet, "/metrics", handleMetrics);
    srv.addRoute(M::kGet, "/api/users", handleApiUsers);
    srv.addPrefixRoute(M::kGet, "/static/", handleStatic);
    srv.addPrefixRoute(M::kGet, "/files/", handleStatic);

    // ── 优雅关闭 ──────────────────────────────────────────────────────────────
    Signal::signal(SIGINT, [&srv]() {
        LOG_INFO << "[main] SIGINT received, stopping server...";
        srv.stop();
    });
    Signal::signal(SIGTERM, [&srv]() {
        LOG_INFO << "[main] SIGTERM received, stopping server...";
        srv.stop();
    });

    // ── 启动 ──────────────────────────────────────────────────────────────────
    LOG_INFO << "──────────────────────────────────────────────────────────────";
    LOG_INFO << "  Airi-Cpp-Server-Lib demo listening on http://"
             << options.tcp.listenIp << ":" << options.tcp.listenPort;
    LOG_INFO << "  Routes:  /  /health  /metrics  /api/users  /static/*  /files/*";
    LOG_INFO << "  Demo bearer token : " << kDemoBearerToken;
    LOG_INFO << "  Demo API key      : " << kDemoApiKey;
    LOG_INFO << "──────────────────────────────────────────────────────────────";

    srv.start();
    return 0;
}
