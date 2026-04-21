#pragma once
#include "Macros.h"
#include "TimeStamp.h"
#include <functional>

// 单个定时任务：到期绝对时刻 + 回调函数 + 可选的重复间隔。
// 一次性定时器：interval == 0.0，到期后由 TimerQueue 负责 delete。
// 重复定时器：  interval > 0.0，到期后调用 restart() 重算下次到期时间，再入队。
class Timer {
    DISALLOW_COPY_AND_MOVE(Timer)
  public:
    Timer(TimeStamp expiration, std::function<void()> cb, double interval)
        : expiration_(expiration), cb_(std::move(cb)), interval_(interval),
          repeat_(interval > 0.0) {}

    void run() const {
        if (cb_)
            cb_();
    }

    TimeStamp expiration() const { return expiration_; }
    bool isRepeat() const { return repeat_; }

    // 重复定时器到期后重算下一次绝对到期时刻
    void restart(TimeStamp now) {
        if (repeat_)
            expiration_ = TimeStamp::addSeconds(now, interval_);
    }

  private:
    TimeStamp expiration_;     // 下次到期的绝对时刻
    std::function<void()> cb_; // 到期时执行的回调
    double interval_;          // 重复间隔（秒），0 表示一次性
    bool repeat_;
};
