#include "http/HttpContext.h"

#include <gtest/gtest.h>
#include <string>

TEST(HttpContextTest, PipelineConsumedBytes) {
    const std::string reqA = "GET /a HTTP/1.1\r\n"
                             "Host: local\r\n"
                             "\r\n";
    const std::string reqB = "GET /b HTTP/1.1\r\n"
                             "Host: local\r\n"
                             "\r\n";
    const std::string merged = reqA + reqB;

    HttpContext ctx;
    int consumedA = 0;
    bool ok = ctx.parse(merged.data(), static_cast<int>(merged.size()), &consumedA);

    ASSERT_TRUE(ok) << "解析失败（第一条请求）";
    EXPECT_TRUE(ctx.isComplete()) << "第一条请求应已完整";
    EXPECT_EQ(ctx.request().url(), "/a");
    EXPECT_EQ(consumedA, static_cast<int>(reqA.size()));

    ctx.reset();
    int consumedB = 0;
    ok = ctx.parse(merged.data() + consumedA, static_cast<int>(merged.size()) - consumedA,
                   &consumedB);

    ASSERT_TRUE(ok) << "解析失败（第二条请求）";
    EXPECT_TRUE(ctx.isComplete()) << "第二条请求应已完整";
    EXPECT_EQ(ctx.request().url(), "/b");
    EXPECT_EQ(consumedB, static_cast<int>(reqB.size()));
}

TEST(HttpContextTest, BodyFragmentAndTailRequest) {
    HttpContext ctx;

    const std::string part1 = "POST /echo HTTP/1.1\r\n"
                              "Host: local\r\n"
                              "Content-Length: 11\r\n"
                              "\r\n"
                              "hello";

    const std::string part2 = " world"
                              "GET /next HTTP/1.1\r\n"
                              "Host: local\r\n"
                              "\r\n";

    int consumed1 = 0;
    bool ok = ctx.parse(part1.data(), static_cast<int>(part1.size()), &consumed1);
    ASSERT_TRUE(ok) << "part1 解析失败";
    EXPECT_FALSE(ctx.isComplete()) << "part1 后请求不应完整";
    EXPECT_EQ(consumed1, static_cast<int>(part1.size()));

    int consumed2 = 0;
    ok = ctx.parse(part2.data(), static_cast<int>(part2.size()), &consumed2);
    ASSERT_TRUE(ok) << "part2 解析失败";
    EXPECT_TRUE(ctx.isComplete()) << "补齐 body 后请求应完整";
    EXPECT_EQ(ctx.request().body(), "hello world");
    EXPECT_EQ(consumed2, 6) << "part2 中本次应只消费剩余 body 的 6 字节";

    ctx.reset();
    const char *tailReq = part2.data() + consumed2;
    const int tailLen = static_cast<int>(part2.size()) - consumed2;
    int consumed3 = 0;
    ok = ctx.parse(tailReq, tailLen, &consumed3);

    ASSERT_TRUE(ok) << "尾随请求解析失败";
    EXPECT_TRUE(ctx.isComplete()) << "尾随请求应完整";
    EXPECT_EQ(ctx.request().url(), "/next");
    EXPECT_EQ(consumed3, tailLen);
}

TEST(HttpContextTest, InvalidMethod) {
    HttpContext ctx;
    const std::string badReq = "BAD / HTTP/1.1\r\n"
                               "Host: local\r\n"
                               "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(badReq.data(), static_cast<int>(badReq.size()), &consumed);
    EXPECT_FALSE(ok) << "非法方法请求应解析失败";
    EXPECT_TRUE(ctx.isInvalid()) << "上下文状态应为 Invalid";
    EXPECT_GT(consumed, 0) << "非法请求也应推进解析位置";
}
