#include <gtest/gtest.h>

#include "http/CorsMiddleware.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

// 辅助：创建一个带指定 method 的请求
static HttpRequest makeRequest(HttpRequest::Method method, const std::string &url = "/") {
    HttpRequest req;
    switch (method) {
    case HttpRequest::Method::kGet:
        req.setMethod("GET");
        break;
    case HttpRequest::Method::kPost:
        req.setMethod("POST");
        break;
    case HttpRequest::Method::kOptions:
        req.setMethod("OPTIONS");
        break;
    default:
        break;
    }
    req.setUrl(url);
    return req;
}

// ── 默认配置 ──────────────────────────────────────────────────

TEST(CorsMiddleware, DefaultAllowOriginStar) {
    CorsMiddleware cors;
    auto mw = cors.toMiddleware();

    HttpRequest req = makeRequest(HttpRequest::Method::kGet);
    HttpResponse resp;
    bool nextCalled = false;

    mw(req, &resp, [&]() { nextCalled = true; });

    EXPECT_TRUE(nextCalled) << "GET 请求应调用 next()";
    EXPECT_EQ(resp.header("Access-Control-Allow-Origin"), "*");
}

// ── OPTIONS 预检请求 ──────────────────────────────────────────

TEST(CorsMiddleware, OptionsPreflightReturns204) {
    CorsMiddleware cors;
    auto mw = cors.toMiddleware();

    HttpRequest req = makeRequest(HttpRequest::Method::kOptions);
    HttpResponse resp;
    bool nextCalled = false;

    mw(req, &resp, [&]() { nextCalled = true; });

    EXPECT_FALSE(nextCalled) << "OPTIONS 预检不应调用 next()";
    EXPECT_EQ(resp.statusCode(), HttpResponse::StatusCode::k204NoContent);
    EXPECT_FALSE(resp.header("Access-Control-Allow-Methods").empty());
    EXPECT_FALSE(resp.header("Access-Control-Allow-Headers").empty());
    EXPECT_FALSE(resp.header("Access-Control-Max-Age").empty());
}

// ── 自定义来源 ────────────────────────────────────────────────

TEST(CorsMiddleware, CustomOrigin) {
    CorsMiddleware cors;
    cors.allowOrigin("https://example.com");
    auto mw = cors.toMiddleware();

    HttpRequest req = makeRequest(HttpRequest::Method::kGet);
    HttpResponse resp;

    mw(req, &resp, []() {});

    EXPECT_EQ(resp.header("Access-Control-Allow-Origin"), "https://example.com");
}

// ── 允许凭证 ──────────────────────────────────────────────────

TEST(CorsMiddleware, AllowCredentials) {
    CorsMiddleware cors;
    cors.allowCredentials(true);
    auto mw = cors.toMiddleware();

    HttpRequest req = makeRequest(HttpRequest::Method::kGet);
    HttpResponse resp;

    mw(req, &resp, []() {});

    EXPECT_EQ(resp.header("Access-Control-Allow-Credentials"), "true");
}

// ── 自定义方法和头 ────────────────────────────────────────────

TEST(CorsMiddleware, CustomMethodsAndHeaders) {
    CorsMiddleware cors;
    cors.allowMethods({"GET", "POST", "PUT", "DELETE"})
        .allowHeaders({"Content-Type", "Authorization", "X-API-Key"})
        .maxAge(3600);
    auto mw = cors.toMiddleware();

    HttpRequest req = makeRequest(HttpRequest::Method::kOptions);
    HttpResponse resp;

    mw(req, &resp, []() {});

    EXPECT_EQ(resp.header("Access-Control-Allow-Methods"), "GET, POST, PUT, DELETE");
    EXPECT_EQ(resp.header("Access-Control-Allow-Headers"),
              "Content-Type, Authorization, X-API-Key");
    EXPECT_EQ(resp.header("Access-Control-Max-Age"), "3600");
}
