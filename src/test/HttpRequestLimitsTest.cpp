#include "http/HttpContext.h"

#include <gtest/gtest.h>
#include <string>

TEST(HttpRequestLimitsTest, RequestLineLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 16;
    limits.maxHeaderBytes = 1024;
    limits.maxBodyBytes = 1024;

    HttpContext ctx(limits);
    const std::string req = "GET /0123456789abcdef HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    EXPECT_FALSE(ok) << "超长请求行应解析失败";
    EXPECT_TRUE(ctx.isInvalid());
    EXPECT_TRUE(ctx.payloadTooLarge());
    EXPECT_GT(consumed, 0) << "失败请求应推进消费字节";
}

TEST(HttpRequestLimitsTest, HeaderLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 1024;
    limits.maxHeaderBytes = 24;
    limits.maxBodyBytes = 1024;

    HttpContext ctx(limits);
    const std::string req = "GET / HTTP/1.1\r\n"
                            "X-Long-Header: 12345678901234567890\r\n"
                            "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    EXPECT_FALSE(ok) << "超长请求头应解析失败";
    EXPECT_TRUE(ctx.isInvalid());
    EXPECT_TRUE(ctx.payloadTooLarge());
}

TEST(HttpRequestLimitsTest, BodyLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 1024;
    limits.maxHeaderBytes = 1024;
    limits.maxBodyBytes = 4;

    HttpContext ctx(limits);
    const std::string req = "POST /upload HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "Content-Length: 10\r\n"
                            "\r\n"
                            "abcdefghij";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    EXPECT_FALSE(ok) << "超长请求体应解析失败";
    EXPECT_TRUE(ctx.isInvalid());
    EXPECT_TRUE(ctx.payloadTooLarge());
}

TEST(HttpRequestLimitsTest, ValidRequestWithinLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 128;
    limits.maxHeaderBytes = 256;
    limits.maxBodyBytes = 16;

    HttpContext ctx(limits);
    const std::string req = "POST /ok HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "Content-Length: 4\r\n"
                            "\r\n"
                            "ping";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    ASSERT_TRUE(ok) << "限制内请求应解析成功";
    EXPECT_TRUE(ctx.isComplete());
    EXPECT_EQ(ctx.request().url(), "/ok");
    EXPECT_EQ(ctx.request().body(), "ping");
}
