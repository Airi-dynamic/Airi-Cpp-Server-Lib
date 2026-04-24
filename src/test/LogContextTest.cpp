/**
 * LogContextTest.cpp — LogContext 结构化日志上下文单元测试
 *
 * 覆盖场景：
 *   1. 设置和清除上下文
 *   2. format() 输出格式
 *   3. RAII Guard 自动清除
 *   4. 请求 ID 唯一递增
 */

#include "log/LogContext.h"
#include <gtest/gtest.h>
#include <set>
#include <thread>

TEST(LogContextTest, SetAndClear) {
    EXPECT_FALSE(LogContext::hasContext());
    EXPECT_TRUE(LogContext::format().empty());

    LogContext::set("req-1", "GET", "/api");
    EXPECT_TRUE(LogContext::hasContext());

    std::string fmt = LogContext::format();
    EXPECT_EQ(fmt, "[req-1 GET /api] ");

    LogContext::clear();
    EXPECT_FALSE(LogContext::hasContext());
    EXPECT_TRUE(LogContext::format().empty());
}

TEST(LogContextTest, SimpleContext) {
    LogContext::set("req-99");
    std::string fmt = LogContext::format();
    EXPECT_EQ(fmt, "[req-99] ");
    LogContext::clear();
}

TEST(LogContextTest, GuardRAII) {
    {
        LogContext::Guard guard("req-42", "POST", "/login");
        EXPECT_TRUE(LogContext::hasContext());
        EXPECT_EQ(LogContext::format(), "[req-42 POST /login] ");
    }
    // Guard 析构后上下文应已清除
    EXPECT_FALSE(LogContext::hasContext());
}

TEST(LogContextTest, RequestIdIncreasing) {
    std::string id1 = LogContext::nextRequestId();
    std::string id2 = LogContext::nextRequestId();
    std::string id3 = LogContext::nextRequestId();

    // 三个 ID 应互不相同
    std::set<std::string> ids{id1, id2, id3};
    EXPECT_EQ(ids.size(), 3u);

    // 都以 "req-" 开头
    EXPECT_EQ(id1.substr(0, 4), "req-");
    EXPECT_EQ(id2.substr(0, 4), "req-");
    EXPECT_EQ(id3.substr(0, 4), "req-");
}

TEST(LogContextTest, ThreadIsolation) {
    // 主线程设置上下文
    LogContext::set("main-req", "GET", "/");

    bool childHasContext = true;
    std::thread t([&] {
        // 子线程不应继承主线程的上下文（thread_local 隔离）
        childHasContext = LogContext::hasContext();
    });
    t.join();

    EXPECT_FALSE(childHasContext);

    LogContext::clear();
}
