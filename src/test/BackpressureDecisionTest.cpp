#include "Connection.h"

#include <gtest/gtest.h>

TEST(BackpressureDecisionTest, ConfigValidation) {
    Connection::BackpressureConfig good{};
    EXPECT_TRUE(Connection::isValidBackpressureConfig(good)) << "默认配置应合法";

    Connection::BackpressureConfig bad1{};
    bad1.lowWatermarkBytes = 0;
    EXPECT_FALSE(Connection::isValidBackpressureConfig(bad1)) << "low=0 应非法";

    Connection::BackpressureConfig bad2{};
    bad2.lowWatermarkBytes = 8;
    bad2.highWatermarkBytes = 4;
    EXPECT_FALSE(Connection::isValidBackpressureConfig(bad2)) << "low >= high 应非法";

    Connection::BackpressureConfig bad3{};
    bad3.lowWatermarkBytes = 4;
    bad3.highWatermarkBytes = 8;
    bad3.hardLimitBytes = 8;
    EXPECT_FALSE(Connection::isValidBackpressureConfig(bad3)) << "high >= hardLimit 应非法";
}

TEST(BackpressureDecisionTest, PauseResumeDecision) {
    Connection::BackpressureConfig cfg{};
    cfg.lowWatermarkBytes = 10;
    cfg.highWatermarkBytes = 20;
    cfg.hardLimitBytes = 30;

    auto d1 = Connection::evaluateBackpressure(5, false, cfg);
    EXPECT_FALSE(d1.shouldPauseRead);
    EXPECT_FALSE(d1.shouldResumeRead);
    EXPECT_FALSE(d1.shouldCloseConnection) << "buffer=5 不应触发动作";

    auto d2 = Connection::evaluateBackpressure(21, false, cfg);
    EXPECT_TRUE(d2.shouldPauseRead) << "buffer=21（未暂停）应触发暂停读";
    EXPECT_FALSE(d2.shouldResumeRead);
    EXPECT_FALSE(d2.shouldCloseConnection);

    auto d3 = Connection::evaluateBackpressure(15, true, cfg);
    EXPECT_FALSE(d3.shouldPauseRead);
    EXPECT_FALSE(d3.shouldResumeRead) << "buffer=15（已暂停）不应立即恢复";
    EXPECT_FALSE(d3.shouldCloseConnection);

    auto d4 = Connection::evaluateBackpressure(10, true, cfg);
    EXPECT_FALSE(d4.shouldPauseRead);
    EXPECT_TRUE(d4.shouldResumeRead) << "buffer=10（已暂停）应恢复读";
    EXPECT_FALSE(d4.shouldCloseConnection);
}

TEST(BackpressureDecisionTest, HardLimitDecision) {
    Connection::BackpressureConfig cfg{};
    cfg.lowWatermarkBytes = 10;
    cfg.highWatermarkBytes = 20;
    cfg.hardLimitBytes = 30;

    auto d = Connection::evaluateBackpressure(31, false, cfg);
    EXPECT_TRUE(d.shouldCloseConnection) << "buffer>hardLimit 应触发保护性断连";
}
