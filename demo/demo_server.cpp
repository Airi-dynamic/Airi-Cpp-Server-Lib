/**
 * demo_server.cpp — Phase 4 功能演示服务器
 *
 * 演示所有 Phase 4 新特性：
 *   1. /metrics       — Prometheus 格式可观测性指标
 *   2. 结构化日志     — 每条请求日志自动携带 [req-ID METHOD /url]
 *   3. 单调时钟       — 请求延迟测量使用 steady_clock，免疫 NTP 跳变
 *   4. 限流中间件     — /api/ 路径限流 50 req/s，超限返回 429
 *   5. 鉴权中间件     — /api/admin/ 需要 Bearer Token 或 API Key
 *
 * 启动方式：
 *   ./demo_server
 *
 * 测试端点：
 *   curl http://127.0.0.1:9090/                     → 200 Hello
 *   curl http://127.0.0.1:9090/metrics               → Prometheus 指标
 *   curl http://127.0.0.1:9090/api/echo?msg=hello    → 200（限流保护）
 *   curl http://127.0.0.1:9090/api/admin/status       → 403（需鉴权）
 *   curl -H "Authorization: Bearer demo-token-2024" \
 *        http://127.0.0.1:9090/api/admin/status       → 200
 *   for i in $(seq 200); do curl -s http://127.0.0.1:9090/api/echo; done
 *        → 达到限流阈值后返回 429
 */

#include <SignalHandler.h>
#include <http/AuthMiddleware.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
#include <http/RateLimiter.h>
#include <http/ServerMetrics.h>
#include <log/Logger.h>

#include <atomic>
#include <iostream>

int main() {
    Logger::setLogLevel(Logger::DEBUG);

    HttpServer::Options opts;
    opts.tcp.listenIp = "127.0.0.1";
    opts.tcp.listenPort = 9090;
    opts.tcp.ioThreads = 2;
    opts.tcp.maxConnections = 100000;
    opts.autoClose = true;
    opts.idleTimeoutSec = 60.0;

    HttpServer srv(opts);

    // ── 中间件 1：限流（全局 50 req/s，桶容量 100）───────────────
    RateLimiter::Config limiterCfg;
    limiterCfg.capacity = 100.0;
    limiterCfg.refillRate = 50.0;
    RateLimiter limiter(limiterCfg);
    srv.use(limiter.toMiddleware());

    // ── 中间件 2：鉴权（/api/admin/* 需要 token）────────────────
    AuthMiddleware auth;
    auth.addBearerToken("demo-token-2024");
    auth.addApiKey("demo-key-001");
    auth.addPublicPath("/");
    auth.addPublicPath("/metrics");
    auth.addPublicPrefix("/api/echo");
    srv.use(auth.toMiddleware());

    // ── 路由：首页 ──────────────────────────────────────────────
    srv.addRoute(HttpRequest::Method::kGet, "/", [](const HttpRequest &, HttpResponse *resp) {
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
        resp->setContentType("text/plain");
        resp->setBody("Airi-Cpp-Server-Lib Phase 4 Demo\n"
                      "  GET /metrics           - Prometheus metrics\n"
                      "  GET /api/echo?msg=...  - Echo (rate-limited)\n"
                      "  GET /api/admin/status  - Protected endpoint\n");
    });

    // ── 路由：Echo（受限流保护）───────────────────────────────────
    srv.addRoute(HttpRequest::Method::kGet, "/api/echo",
                 [](const HttpRequest &req, HttpResponse *resp) {
                     std::string msg = req.queryParam("msg");
                     if (msg.empty())
                         msg = "hello";
                     resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
                     resp->setContentType("application/json");
                     resp->setBody("{\"echo\":\"" + msg + "\"}\n");
                 });

    // ── 路由：Admin 状态（受鉴权保护）────────────────────────────
    srv.addRoute(HttpRequest::Method::kGet, "/api/admin/status",
                 [](const HttpRequest &, HttpResponse *resp) {
                     resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
                     resp->setContentType("application/json");
                     resp->setBody("{\"status\":\"ok\",\"role\":\"admin\"}\n");
                 });

    Signal::signal(SIGINT, [&] {
        static std::atomic_flag fired = ATOMIC_FLAG_INIT;
        if (fired.test_and_set())
            return;
        std::cout << "\n[demo_server] Shutting down.\n";
        srv.stop();
    });

    std::cout << "=== Phase 4 Demo Server ===\n"
              << "Listening on http://127.0.0.1:9090\n"
              << "Features: metrics, structured logging, monotonic clock,\n"
              << "          rate limiting (50 req/s), auth (Bearer/API Key)\n"
              << "Press Ctrl+C to stop.\n";

    srv.start();
    return 0;
}
