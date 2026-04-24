#pragma once
#include "http/HttpServer.h"
#include "http/ServerMetrics.h"
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

// RateLimiter：令牌桶限流中间件
//
// 每个客户端 IP 维护独立的令牌桶：
//   - capacity     ：桶容量（最大突发）
//   - refillRate   ：每秒补充的令牌数
//   - 每次请求消耗 1 个令牌，令牌耗尽则返回 429 Too Many Requests
//
// 线程安全：令牌桶状态用 mutex 保护（IP 查找 + 令牌扣减是轻量操作，不构成瓶颈）。
// 实际生产场景可换用 ConcurrentHashMap 或分片锁优化。
class RateLimiter {
  public:
    struct Config {
        double capacity = 100.0;  // 桶容量（最大突发请求数）
        double refillRate = 50.0; // 每秒补充令牌数
    };

    RateLimiter() : config_{} {}
    explicit RateLimiter(const Config &config) : config_(config) {}

    // 生成可注册到 HttpServer::use() 的中间件
    HttpServer::Middleware toMiddleware() {
        return [this](const HttpRequest &req, HttpResponse *resp,
                      const HttpServer::MiddlewareNext &next) {
            std::string ip = extractClientIp(req);
            if (tryConsume(ip)) {
                next();
            } else {
                ServerMetrics::instance().onRateLimited();
                resp->setStatus(HttpResponse::StatusCode::k429TooManyRequests, "Too Many Requests");
                resp->setContentType("application/json");
                resp->setBody(R"({"error":"rate_limit_exceeded","retry_after_sec":1})");
                resp->addHeader("Retry-After", "1");
                resp->setCloseConnection(false);
            }
        };
    }

  private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point lastRefill;
    };

    bool tryConsume(const std::string &ip) {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        auto &b = buckets_[ip];

        // 首次访问初始化
        if (b.tokens == 0.0 && b.lastRefill == std::chrono::steady_clock::time_point{}) {
            b.tokens = config_.capacity;
            b.lastRefill = now;
        }

        // 补充令牌
        double elapsed = std::chrono::duration<double>(now - b.lastRefill).count();
        b.tokens = std::min(config_.capacity, b.tokens + elapsed * config_.refillRate);
        b.lastRefill = now;

        // 扣减
        if (b.tokens >= 1.0) {
            b.tokens -= 1.0;
            return true;
        }
        return false;
    }

    static std::string extractClientIp(const HttpRequest &req) {
        // 优先读取反代转发头
        std::string forwarded = req.header("X-Forwarded-For");
        if (!forwarded.empty()) {
            auto pos = forwarded.find(',');
            return pos != std::string::npos ? forwarded.substr(0, pos) : forwarded;
        }
        std::string realIp = req.header("X-Real-IP");
        if (!realIp.empty())
            return realIp;
        // 无转发头时降级为 "unknown"（TCP 层未暴露对端地址到 HTTP 层）
        return "unknown";
    }

    Config config_;
    std::mutex mu_;
    std::unordered_map<std::string, Bucket> buckets_;
};
