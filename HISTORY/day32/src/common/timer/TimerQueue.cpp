#include "timer/TimerQueue.h"
#include "timer/Timer.h"
#include <cstdint> // UINTPTR_MAX

TimerQueue::TimerQueue(Eventloop *loop) : loop_(loop) {}

TimerQueue::~TimerQueue() {
    // 销毁所有仍在队列中的 Timer 对象（已触发并重新入队的重复定时器也在此释放）
    for (auto &[ts, timer] : timers_)
        delete timer;
}

void TimerQueue::addTimer(TimeStamp when, std::function<void()> cb, double interval) {
    Timer *timer = new Timer(when, std::move(cb), interval);
    insert(timer);
}

void TimerQueue::insert(Timer *timer) {
    timers_.emplace(timer->expiration(), timer);
}

int TimerQueue::nextTimeoutMs() const {
    if (timers_.empty())
        return -1; // 无定时器：poll 永久阻塞，直到有 IO 事件或被 wakeup 唤醒

    const TimeStamp &earliest = timers_.begin()->first;
    TimeStamp now = TimeStamp::now();
    if (earliest <= now)
        return 0; // 已有到期任务：通知 poll 立即返回

    int64_t diffUs = earliest.microseconds() - now.microseconds();
    return static_cast<int>(diffUs / 1000); // 微秒 → 毫秒（poll 接受 ms 参数）
}

void TimerQueue::processExpiredTimers() {
    TimeStamp now = TimeStamp::now();

    // 哨兵：{now, 最大指针值}
    // set::upper_bound 返回第一个"严格大于哨兵"的迭代器，
    // 即第一个到期时刻 > now 的定时器。
    // 哨兵之前的所有元素满足 ts <= now（已到期），是我们要取出的范围。
    Entry sentinel{now, reinterpret_cast<Timer *>(UINTPTR_MAX)};
    auto endIt = timers_.upper_bound(sentinel);

    // 将已到期项移到临时 vector，再从 set 中删除
    // （先 collect 再 erase，防止回调中操作 timers_ 产生迭代器失效）
    std::vector<Entry> expired(timers_.begin(), endIt);
    timers_.erase(timers_.begin(), endIt);

    for (auto &[ts, timer] : expired) {
        timer->run(); // 执行回调（可能在回调内通过 runInLoop 添加新定时器，安全）
        if (timer->isRepeat()) {
            timer->restart(now); // 重算下次到期时刻
            insert(timer);       // 重新入队
        } else {
            delete timer; // 一次性定时器，释放内存
        }
    }
}
