/**
 * MetricsTest.cpp — ServerMetrics 单元测试
 *
 * 覆盖场景：
 *   1. 连接计数器递增
 *   2. 请求完成计数 + 延迟分桶
 *   3. 状态码分类计数（2xx/4xx/5xx）
 *   4. 流量字节计数
 *   5. 限流 / 鉴权拒绝计数
 *   6. Prometheus text 格式导出
 *   7. reset() 清零
 */

#include "http/ServerMetrics.h"
#include <gtest/gtest.h>
#include <string>

class MetricsTest : public ::testing::Test {
  protected:
    void SetUp() override { ServerMetrics::instance().reset(); }
};

TEST_F(MetricsTest, ConnectionCounting) {
    ServerMetrics::instance().onConnectionAccepted();
    ServerMetrics::instance().onConnectionAccepted();
    ServerMetrics::instance().onConnectionAccepted();

    EXPECT_EQ(ServerMetrics::instance().connectionsTotal(), 3);
}

TEST_F(MetricsTest, RequestCompleteCounting) {
    ServerMetrics::instance().onRequestComplete(200, 50);     // ≤100µs bucket
    ServerMetrics::instance().onRequestComplete(200, 300);    // ≤500µs bucket
    ServerMetrics::instance().onRequestComplete(404, 2000);   // ≤5000µs bucket
    ServerMetrics::instance().onRequestComplete(500, 200000); // >100ms bucket

    // 检查 Prometheus 输出
    std::string output = ServerMetrics::instance().toPrometheus();

    // 总请求数
    EXPECT_NE(output.find("mcpp_requests_total 4"), std::string::npos);
    // 2xx = 2
    EXPECT_NE(output.find("mcpp_responses_2xx 2"), std::string::npos);
    // 4xx = 1
    EXPECT_NE(output.find("mcpp_responses_4xx 1"), std::string::npos);
    // 5xx = 1
    EXPECT_NE(output.find("mcpp_responses_5xx 1"), std::string::npos);
}

TEST_F(MetricsTest, LatencyBuckets) {
    // 全部落入 ≤100µs 桶
    for (int i = 0; i < 10; ++i) {
        ServerMetrics::instance().onRequestComplete(200, 50);
    }

    std::string output = ServerMetrics::instance().toPrometheus();
    // 累积直方图：le="100" 应该有 10 个
    EXPECT_NE(output.find("mcpp_request_duration_us_bucket{le=\"100\"} 10"), std::string::npos);
}

TEST_F(MetricsTest, BytesCounting) {
    ServerMetrics::instance().addBytesRead(1024);
    ServerMetrics::instance().addBytesRead(2048);
    ServerMetrics::instance().addBytesWritten(4096);

    std::string output = ServerMetrics::instance().toPrometheus();
    EXPECT_NE(output.find("mcpp_bytes_read 3072"), std::string::npos);
    EXPECT_NE(output.find("mcpp_bytes_written 4096"), std::string::npos);
}

TEST_F(MetricsTest, RateLimitAndAuthCounters) {
    ServerMetrics::instance().onRateLimited();
    ServerMetrics::instance().onRateLimited();
    ServerMetrics::instance().onAuthRejected();

    std::string output = ServerMetrics::instance().toPrometheus();
    EXPECT_NE(output.find("mcpp_rate_limited_total 2"), std::string::npos);
    EXPECT_NE(output.find("mcpp_auth_rejected_total 1"), std::string::npos);
}

TEST_F(MetricsTest, ResetClearsAll) {
    ServerMetrics::instance().onConnectionAccepted();
    ServerMetrics::instance().onRequestComplete(200, 100);
    ServerMetrics::instance().addBytesRead(100);
    ServerMetrics::instance().onRateLimited();

    ServerMetrics::instance().reset();

    EXPECT_EQ(ServerMetrics::instance().connectionsTotal(), 0);
    std::string output = ServerMetrics::instance().toPrometheus();
    EXPECT_NE(output.find("mcpp_requests_total 0"), std::string::npos);
    EXPECT_NE(output.find("mcpp_bytes_read 0"), std::string::npos);
}
