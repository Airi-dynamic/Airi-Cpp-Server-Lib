#pragma once
#include "http/HttpServer.h"
#include <string>
#include <vector>

// CorsMiddleware：跨域资源共享中间件
//
// 处理 CORS 预检请求（OPTIONS）和普通请求的跨域响应头。
//
// 用法：
//   CorsMiddleware cors;
//   cors.allowOrigin("https://example.com")
//       .allowMethods({"GET", "POST", "PUT", "DELETE"})
//       .allowHeaders({"Content-Type", "Authorization", "X-API-Key"})
//       .maxAge(86400);
//   server.use(cors.toMiddleware());
//
// 默认配置：
//   - 允许所有来源（"*"）
//   - 允许 GET, POST, OPTIONS
//   - 允许 Content-Type, Authorization
//   - 预检缓存 86400 秒
class CorsMiddleware {
  public:
    CorsMiddleware() = default;

    // 设置允许的来源（默认 "*"）
    CorsMiddleware &allowOrigin(const std::string &origin) {
        allowOrigin_ = origin;
        return *this;
    }

    // 设置允许的方法列表
    CorsMiddleware &allowMethods(const std::vector<std::string> &methods) {
        allowMethods_.clear();
        for (size_t i = 0; i < methods.size(); ++i) {
            if (i > 0)
                allowMethods_ += ", ";
            allowMethods_ += methods[i];
        }
        return *this;
    }

    // 设置允许的请求头列表
    CorsMiddleware &allowHeaders(const std::vector<std::string> &headers) {
        allowHeaders_.clear();
        for (size_t i = 0; i < headers.size(); ++i) {
            if (i > 0)
                allowHeaders_ += ", ";
            allowHeaders_ += headers[i];
        }
        return *this;
    }

    // 预检缓存有效期（秒）
    CorsMiddleware &maxAge(int seconds) {
        maxAge_ = std::to_string(seconds);
        return *this;
    }

    // 是否允许携带 Cookie / Authorization
    CorsMiddleware &allowCredentials(bool allow) {
        allowCredentials_ = allow;
        return *this;
    }

    // 生成可注册到 HttpServer::use() 的中间件
    HttpServer::Middleware toMiddleware() {
        return [this](const HttpRequest &req, HttpResponse *resp,
                      const HttpServer::MiddlewareNext &next) {
            // 注入 CORS 响应头
            resp->addHeader("Access-Control-Allow-Origin", allowOrigin_);
            if (allowCredentials_) {
                resp->addHeader("Access-Control-Allow-Credentials", "true");
            }

            // OPTIONS 预检请求：直接返回 204，不进入后续中间件和路由
            if (req.method() == HttpRequest::Method::kOptions) {
                resp->addHeader("Access-Control-Allow-Methods", allowMethods_);
                resp->addHeader("Access-Control-Allow-Headers", allowHeaders_);
                resp->addHeader("Access-Control-Max-Age", maxAge_);
                resp->setStatus(HttpResponse::StatusCode::k204NoContent, "No Content");
                resp->setBody("");
                return; // 不调用 next()，中断后续链路
            }

            next();
        };
    }

  private:
    std::string allowOrigin_{"*"};
    std::string allowMethods_{"GET, POST, OPTIONS"};
    std::string allowHeaders_{"Content-Type, Authorization"};
    std::string maxAge_{"86400"};
    bool allowCredentials_{false};
};
