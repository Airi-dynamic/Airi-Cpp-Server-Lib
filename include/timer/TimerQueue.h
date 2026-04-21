#pragma once
#include "Macros.h"
#include "TimeStamp.h"
#include <functional>
#include <set>
#include <utility>

class Timer;
class Eventloop;

// TimerQueue：用有序集合（std::set）管理所有定时任务。
//
// 核心数据结构：std::set<{TimeStamp, Timer*}>
//   - 按到期时刻从小到大排序
//   - 同一时刻可能有多个定时器，用 Timer* 指针值唯一区分
//
// 线程安全性：本类不加锁，所有方法必须在归属 EventLoop 线程中调用。
// EventLoop::runAt/runAfter/runEvery 通过 runInLoop 保证这一点。
//
// 与 EventLoop::loop() 的集成：
//   loop() 每次迭代时：
//     1. timeout = nextTimeoutMs()  → 传给 poll()，poll 在有定时任务时不永久阻塞
//     2. poll(timeout) 返回后，立即调用 processExpiredTimers() 触发到期回调
class TimerQueue {
    DISALLOW_COPY_AND_MOVE(TimerQueue)
  public:
    explicit TimerQueue(Eventloop *loop);
    ~TimerQueue(); // 销毁所有未触发的 Timer 对象，防止内存泄漏

    // 添加一个定时任务（必须在 EventLoop 线程调用）
    // interval > 0 表示重复定时器，否则一次性
    void addTimer(TimeStamp when, std::function<void()> cb, double interval);

    // 计算距离最近一个定时器到期还有多少毫秒。
    // 返回 -1  → 无定时器，poll 可永久阻塞（直到 IO 事件或 wakeup）
    // 返回 0   → 已有过期任务，poll 应立即返回
    // 返回 >0  → 等待该毫秒数后最早的定时器将到期
    int nextTimeoutMs() const;

    // 找出所有到期的定时器，依次执行回调：
    //   - 重复型：restart() 重算到期时间，重新入队
    //   - 一次性：delete
    void processExpiredTimers();

  private:
    // set 的 Entry：{到期时刻, Timer 裸指针}
    // Timer* 作为同一时刻多个定时器的唯一键（指针值唯一）
    using Entry = std::pair<TimeStamp, Timer *>;

    void insert(Timer *timer); // 将 Timer 插入有序集合

    Eventloop *loop_;        // 归属 EventLoop（不拥有）
    std::set<Entry> timers_; // 按到期时刻有序的定时器集合
};
