#pragma once
#include <chrono>
#include <cstdint>

// SteadyClock：单调时钟封装（clock_gettime(CLOCK_MONOTONIC) / std::chrono::steady_clock）
//
// 与 TimeStamp（wall clock / gettimeofday）的分工：
//   - SteadyClock  → 用于 内部计时（延迟测量、超时判断、定时器间隔）
//                     不受 NTP 校时跳变影响，保证单调递增
//   - TimeStamp    → 用于 日志输出、人可读时间戳（需要挂钟语义）
//
// 重构动机：
//   gettimeofday() 是挂钟（wall clock），在 NTP 校时、系统休眠恢复后可能回退，
//   导致 "定时器提前触发 / 延迟测量为负" 等异常。
//   引入 SteadyClock 后，所有延迟 / 超时计算路径使用单调时钟，彻底消除此类问题。
class SteadyClock {
  public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    // 获取当前单调时钟时刻
    static time_point now() noexcept { return clock::now(); }

    // 计算两个时刻之间的微秒差
    static int64_t elapsedUs(time_point start, time_point end) noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    }

    // 计算从 start 到现在的微秒数
    static int64_t elapsedSinceUs(time_point start) noexcept { return elapsedUs(start, now()); }

    // 计算从 start 到现在的秒数（浮点）
    static double elapsedSinceSec(time_point start) noexcept {
        return static_cast<double>(elapsedSinceUs(start)) / 1000000.0;
    }
};
