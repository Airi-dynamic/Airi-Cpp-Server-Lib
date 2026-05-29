#pragma once
#include "http/HttpServer.h"
#include "http/ServerMetrics.h"
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

// AuthMiddleware：HTTP 鉴权中间件
//
// 支持两种鉴权方式（按优先级）：
//   1. Bearer Token — Authorization: Bearer <token>
//   2. API Key      — X-API-Key: <key> 或 ?api_key=<key>
//
// 用法：
//   AuthMiddleware auth;
//   auth.addBearerToken("my-secret-token");
//   auth.addApiKey("prod-key-001");
//   auth.addPublicPath("/metrics");    // 白名单路径不鉴权
//   auth.addPublicPrefix("/public/");  // 白名单前缀
//   srv.use(auth.toMiddleware());
class AuthMiddleware {
  public:
    AuthMiddleware() = default;

    void addBearerToken(const std::string &token) { bearerTokens_.insert(token); }
    void addApiKey(const std::string &key) { apiKeys_.insert(key); }

    // 白名单：精确路径和前缀路径不需要鉴权
    void addPublicPath(const std::string &path) { publicPaths_.insert(path); }
    void addPublicPrefix(const std::string &prefix) { publicPrefixes_.push_back(prefix); }

    // 生成可注册到 HttpServer::use() 的中间件
    HttpServer::Middleware toMiddleware() {
        return [this](const HttpRequest &req, HttpResponse *resp,
                      const HttpServer::MiddlewareNext &next) {
            // 白名单检查
            if (isPublic(req.url())) {
                next();
                return;
            }

            // 无 token/key 配置时相当于禁用鉴权
            if (bearerTokens_.empty() && apiKeys_.empty()) {
                next();
                return;
            }

            if (authenticate(req)) {
                next();
            } else {
                ServerMetrics::instance().onAuthRejected();
                resp->setStatus(HttpResponse::StatusCode::k403Forbidden, "Forbidden");
                resp->setContentType("application/json");
                resp->setBody(
                    R"({"error":"unauthorized","message":"valid token or API key required"})");
                resp->setCloseConnection(false);
            }
        };
    }

  private:
    bool isPublic(const std::string &url) const {
        if (publicPaths_.count(url))
            return true;
        for (const auto &prefix : publicPrefixes_) {
            if (url.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
        return false;
    }

    bool authenticate(const HttpRequest &req) const {
        // 1. Bearer Token
        std::string auth = req.header("Authorization");
        if (auth.size() > 7 && auth.compare(0, 7, "Bearer ") == 0) {
            std::string token = auth.substr(7);
            if (bearerTokens_.count(token))
                return true;
        }

        // 2. X-API-Key header
        std::string apiKey = req.header("X-API-Key");
        if (!apiKey.empty() && apiKeys_.count(apiKey))
            return true;

        // 3. Query parameter ?api_key=...
        std::string queryKey = req.queryParam("api_key");
        if (!queryKey.empty() && apiKeys_.count(queryKey))
            return true;

        return false;
    }

    std::unordered_set<std::string> bearerTokens_;
    std::unordered_set<std::string> apiKeys_;
    std::unordered_set<std::string> publicPaths_;
    std::vector<std::string> publicPrefixes_;
};
