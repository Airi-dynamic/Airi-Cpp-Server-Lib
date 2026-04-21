# Day 23 — 跨平台定时器（TimerQueue）

> **主题**：在 Reactor 事件循环中集成通用定时器系统，支持一次性与重复定时任务。  
> **基于**：Day 22（EventLoopThreadPool 多 Reactor）

---

## 1. 引言

### 1.1 问题上下文

到 Day 22，Reactor 网络层完整、稳定，但缺少一个工业级服务器的标配：**定时器**。HTTP keep-alive 的空闲连接关闭、心跳检查、限流窗口、异步日志的滚动检查、批量任务的 flush——所有这些都需要"在某个时间点 / 周期触发回调"。

朴素做法是开一个定时器线程加 `sleep`，但这会引入跨线程同步、与 Reactor 事件循环抢资源、回调上下文混乱。muduo / Netty / asio 的标准做法是把**定时器集成进事件循环**：在 `epoll_wait` / `kevent` 调用时把"距离最近一个定时器到期还有多久"作为超时参数传入，超时返回后处理过期定时器，这样定时器与 IO 事件共享同一个事件循环线程，无需额外线程或锁。

### 1.2 动机

事件循环集成的定时器有三个核心好处：(a) 无锁——定时器回调与 IO 回调跑在同一线程；(b) 精度可控——`epoll_wait` 超时精度毫秒级足以应对绝大多数业务定时；(c) 跨平台——Linux 用 `timerfd` 或纯应用层堆，macOS 用 `kqueue EVFILT_TIMER`，本项目选择应用层堆（`std::set` 排序）以最大化跨平台一致性。

引入它后，Day 26 的 HTTP 空闲超时、Day 28 的 backpressure 评估都能直接调 `loop->runEvery(...)`。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 单独定时器线程 + sleep | 早期 C | 简单 | 多线程同步、上下文切换 |
| Reactor 集成 + std::set 应用层堆 (本日) | muduo / 本项目 | 无锁、跨平台 | O(log n) 插入 |
| `timerfd_create` + epoll | Linux 2.6.25+ | 内核精度、与 epoll 原生集成 | Linux only |
| `kqueue EVFILT_TIMER` | macOS/BSD | 内核精度 | BSD only |
| HashedWheelTimer | Netty | O(1) 插入、海量定时器 | 精度受 wheel tick 限制 |
| Java `ScheduledThreadPoolExecutor` | Java | 库内置 | 跨语言 |

### 1.4 本日方案概述

本日实现：
1. 新增 `TimeStamp.h`（header-only）：微秒精度时间戳，`now()` / `addSeconds()` / 比较运算。
2. 新增 `Timer.h`：单个定时任务（到期时刻 + 回调 + 重复间隔）。
3. 新增 `TimerQueue.h/cpp`：底层 `std::set<pair<TimeStamp, Timer*>>` 排序，按到期时间从早到晚；提供 `addTimer()` / `getTimeoutMs()` / `handleExpired()`。
4. `EventLoop` 新增 `timerQueue_` 成员 + `runAt(when, cb)` / `runAfter(delay, cb)` / `runEvery(interval, cb)` 三个对外接口。
5. `EventLoop::loop()` 集成定时：`pollerWaitTime = timerQueue_->getTimeoutMs()`；`poll` 返回后先处理 IO，再 `timerQueue_->handleExpired()`。
6. 新增 `test/TimerTest.cpp`：测一次性、重复、跨线程投递（`runInLoop` 包 `addTimer`）。

下一天会用同样"集成进事件循环"的思路加异步日志的双缓冲。

---
## 2. 文件变更总览

| 文件 | 状态 | 说明 |
|------|------|------|
| `include/timer/TimeStamp.h` | **新增** | 微秒精度时间戳（header-only），提供 `now()`、`addSeconds()`、比较运算 |
| `include/timer/Timer.h` | **新增** | 单个定时任务：到期时刻 + 回调 + 重复间隔 |
| `include/timer/TimerQueue.h` | **新增** | 定时器有序队列，管理所有 Timer 的生命周期 |
| `common/timer/TimerQueue.cpp` | **新增** | TimerQueue 实现：插入、超时计算、过期回调处理 |
| `include/EventLoop.h` | **修改** | 新增 `timerQueue_` 成员、`runAt()`/`runAfter()`/`runEvery()` 定时器接口 |
| `common/Eventloop.cpp` | **修改** | `loop()` 集成定时器：计算 poll 超时、处理过期定时器 |
| `server.cpp` | **修改** | 使用 `TimeStamp` 打印连接时间 |
| `test/TimerTest.cpp` | **新增** | 定时器功能测试：一次性、重复、跨线程投递 |

---

## 3. 模块全景与所有权树

```
TcpServer（业务入口）
├── mainReactor: Eventloop*
│   ├── Poller (kqueue/epoll)
│   ├── Channel (wakeup fd)
│   └── TimerQueue ★ NEW
│       └── std::set<{TimeStamp, Timer*}>
│           ├── Timer { expiration, callback, interval }
│           ├── Timer { ... }
│           └── ...
├── Acceptor (accept 新连接)
├── EventLoopThreadPool
│   ├── EventLoopThread → subReactor: Eventloop
│   │   ├── Poller
│   │   ├── Channel (wakeup fd)
│   │   └── TimerQueue ★ NEW
│   │       └── std::set<{TimeStamp, Timer*}>
│   └── ...
└── connections: map<int, unique_ptr<Connection>>
```

每个 EventLoop 拥有独立的 TimerQueue。定时器通过 `runInLoop` 投递到目标线程，TimerQueue 本身无需加锁。

---

## 4. 全流程调用链

### 4.1 添加定时器

```
用户代码
  │ loop->runAfter(3.0, callback)
  ▼
Eventloop::runAfter(seconds, cb)
  │ 计算绝对时刻 when = now + seconds
  │ 调用 runAt(when, cb)
  ▼
Eventloop::runAt(when, cb)
  │ runInLoop(lambda)
  │   ├── 本线程：直接执行 lambda
  │   └── 跨线程：queueInLoop → wakeup
  ▼
lambda 执行：
  timerQueue_->addTimer(when, cb, interval)
    │ new Timer(when, cb, interval)
    │ timers_.emplace({when, timer})
    ▼
  set 按到期时刻有序排列
```

### 4.2 EventLoop::loop() 定时器集成

```
Eventloop::loop()
  │
  ├─── ① timeout = timerQueue_->nextTimeoutMs()
  │      ├── 无定时器 → -1（永久阻塞）
  │      ├── 已过期   → 0（立即返回）
  │      └── 未到期   → 距到期的毫秒数
  │
  ├─── ② poller_->poll(timeout)
  │      └── 阻塞 timeout ms 或有 IO 事件立即返回
  │
  ├─── ③ 处理 IO：ch->handleEvent()
  │
  ├─── ④ timerQueue_->processExpiredTimers()   ★ NEW
  │      ├── 用哨兵 {now, UINTPTR_MAX} upper_bound 找到所有过期项
  │      ├── 移到临时 vector，从 set 中删除
  │      └── 遍历：
  │          ├── timer->run()  执行回调
  │          ├── repeat → restart + 重新入队
  │          └── 一次性 → delete
  │
  └─── ⑤ doPendingFunctors()
```

---

## 5. 代码逐段解析

### 5.1 TimeStamp.h — 微秒精度时间戳

```cpp
class TimeStamp {
  public:
    static constexpr int64_t kMicrosecondsPerSecond = 1000LL * 1000;
    // ...
    static TimeStamp now() {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return TimeStamp(tv.tv_sec * kMicrosecondsPerSecond + tv.tv_usec);
    }
    static TimeStamp addSeconds(TimeStamp ts, double seconds) {
        int64_t delta = static_cast<int64_t>(seconds * kMicrosecondsPerSecond);
        return TimeStamp(ts.us_ + delta);
    }
  private:
    int64_t us_;
};
```

**设计决策**：

- **微秒精度**：`gettimeofday` 在 Linux/macOS 均可用，精度远超 `time()`。
- **Header-only**：无 `.cpp` 文件，全部 inline 实现，零链接开销。
- **值语义**：轻量 8 字节，可安全拷贝、作为 `std::set` 的键。

### 5.2 Timer.h — 单个定时任务

```cpp
class Timer {
  public:
    Timer(TimeStamp expiration, std::function<void()> cb, double interval)
        : expiration_(expiration), cb_(std::move(cb)),
          interval_(interval), repeat_(interval > 0.0) {}

    void run() const { if (cb_) cb_(); }
    void restart(TimeStamp now) {
        if (repeat_) expiration_ = TimeStamp::addSeconds(now, interval_);
    }
  private:
    TimeStamp expiration_;
    std::function<void()> cb_;
    double interval_;   // 0 = 一次性
    bool repeat_;
};
```

**要点**：

- `interval > 0` 自动判定为重复定时器，`restart()` 重算下次到期时刻。
- 到期后由 TimerQueue 决定 delete（一次性）或重新入队（重复）。

### 5.3 TimerQueue — 有序集合管理

```cpp
using Entry = std::pair<TimeStamp, Timer*>;
std::set<Entry> timers_;
```

**为什么用 `std::set<pair<TimeStamp, Timer*>>`？**

- `std::set` 自动按 `first`（到期时刻）排序，`begin()` 即最早到期的定时器。
- 同一时刻可能有多个定时器，`Timer*` 指针值作为唯一键区分（pair 的第二元素参与比较）。

```cpp
int TimerQueue::nextTimeoutMs() const {
    if (timers_.empty()) return -1;
    const TimeStamp &earliest = timers_.begin()->first;
    TimeStamp now = TimeStamp::now();
    if (earliest <= now) return 0;
    int64_t diffUs = earliest.microseconds() - now.microseconds();
    return static_cast<int>(diffUs / 1000);
}
```

**关键**：返回值直接传给 `poll(timeout)`，实现精确唤醒——不早于定时器到期，不晚于下一次 IO。

```cpp
void TimerQueue::processExpiredTimers() {
    TimeStamp now = TimeStamp::now();
    Entry sentinel{now, reinterpret_cast<Timer*>(UINTPTR_MAX)};
    auto endIt = timers_.upper_bound(sentinel);
    // ...
}
```

**哨兵技巧**：`{now, UINTPTR_MAX}` 保证 `upper_bound` 返回的迭代器之前的所有元素满足 `ts <= now`，一次性取出全部过期项。

### 5.4 EventLoop 定时器接口

```cpp
void Eventloop::runAfter(double seconds, std::function<void()> cb) {
    runAt(TimeStamp::addSeconds(TimeStamp::now(), seconds), std::move(cb));
}
void Eventloop::runEvery(double seconds, std::function<void()> cb) {
    runInLoop([this, seconds, cb = std::move(cb)]() mutable {
        timerQueue_->addTimer(
            TimeStamp::addSeconds(TimeStamp::now(), seconds),
            std::move(cb), seconds);
    });
}
```

- `runAfter`：先算出绝对时刻，再调 `runAt`。
- `runEvery`：传入 `interval = seconds`，Timer 构造时 `repeat_ = true`。
- 所有方法通过 `runInLoop` 保证线程安全。

### 5.5 TimerTest.cpp — 功能验证

```
TimerTest:
  ├── EventLoopThread 启动子线程 EventLoop
  ├── runEvery(0.5s, tick)       → 每 0.5s 打印 tick
  ├── runAfter(1.0s, fired)     → 1s 后打印 fired
  ├── runAfter(2.2s, setQuit)   → 2.2s 后停止循环
  ├── 主线程 sleep 0.8s 后 runInLoop → 跨线程投递
  └── elt.join() 等待退出
  期望：everyCount ≈ 4 ticks in 2.2s
```

---

### 5.6 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day23/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day23/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 跨平台设计

本次 TimerQueue 采用 **poll 超时驱动** 方案，而非 Linux 专有的 `timerfd_create`：

| 方案 | 优点 | 缺点 |
|------|------|------|
| `timerfd_create` (Linux) | 与 epoll 统一事件模型 | Linux 专有，macOS 不支持 |
| **poll timeout（本方案）** | **跨平台**，Linux/macOS/BSD 通用 | 精度受 poll 返回时机影响（毫秒级） |
| kqueue `EVFILT_TIMER` | macOS 原生定时器 | 代码分叉，复杂度高 |

`nextTimeoutMs()` 返回值直接作为 `kevent()/epoll_wait()` 的 timeout 参数，零额外系统调用。

---

## 7. 职责划分表

| 类 | 单一职责 |
|----|----------|
| `TimeStamp` | 微秒时间点的表示与运算（值语义） |
| `Timer` | 单个定时任务的数据持有与回调触发 |
| `TimerQueue` | 定时器的有序存储、超时计算、过期处理 |
| `Eventloop` | 将 TimerQueue 集成到事件循环，提供线程安全定时器 API |

---

## 8. 局限与后续

| 当前局限 | 后续改进方向 |
|----------|------------|
| Timer 裸指针管理，无取消机制 | 返回 TimerId，支持 `cancelTimer()` |
| 无法精确到微秒级（受 poll 精度限制） | 高精度场景考虑 `timerfd` + kqueue `EVFILT_TIMER` 双后端 |
| `reinterpret_cast<Timer*>(UINTPTR_MAX)` 作为哨兵 | 语义不够清晰，可用自定义比较器替代 |
| server.cpp 中 runAfter 空闲超时被注释（Connection 生命周期风险） | 需将 Connection 改为 shared_ptr + weak_ptr |
| **→ Day 24**：日志系统（异步双缓冲日志框架） | |
