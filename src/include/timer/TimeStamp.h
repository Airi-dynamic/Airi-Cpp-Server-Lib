#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <sys/time.h>

// 微秒精度时间戳（header-only）
// 内部以 int64_t 存储自 Unix 纪元起经过的微秒数，支持比较与格式化输出。
class TimeStamp {
  public:
    static constexpr int64_t kMicrosecondsPerSecond = 1000LL * 1000;

    TimeStamp() : us_(0) {}
    explicit TimeStamp(int64_t us) : us_(us) {}

    bool operator<(const TimeStamp &rhs) const { return us_ < rhs.us_; }
    bool operator<=(const TimeStamp &rhs) const { return us_ <= rhs.us_; }
    bool operator==(const TimeStamp &rhs) const { return us_ == rhs.us_; }

    int64_t microseconds() const { return us_; }

    // 格式化为 "YYYY-MM-DD HH:MM:SS.uuuuuu"
    std::string toString() const {
        char buf[64];
        time_t sec = static_cast<time_t>(us_ / kMicrosecondsPerSecond);
        int usec = static_cast<int>(us_ % kMicrosecondsPerSecond);
        struct tm tm_time;
        localtime_r(&sec, &tm_time);
        snprintf(buf, sizeof(buf), "%4d-%02d-%02d %02d:%02d:%02d.%06d",
                 tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
                 tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, usec);
        return buf;
    }

    // 获取当前时刻
    static TimeStamp now() {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return TimeStamp(tv.tv_sec * kMicrosecondsPerSecond + tv.tv_usec);
    }

    // 在时间戳 ts 上加 seconds 秒（支持小数，精确到微秒）
    static TimeStamp addSeconds(TimeStamp ts, double seconds) {
        int64_t delta = static_cast<int64_t>(seconds * kMicrosecondsPerSecond);
        return TimeStamp(ts.us_ + delta);
    }

  private:
    int64_t us_; // 微秒数
};
