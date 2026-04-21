# Day 23 — 跨平台定时器（TimerQueue）

在 EventLoop 中集成定时器系统，支持一次性（`runAfter`）与重复（`runEvery`）定时任务。采用 poll 超时驱动方案，无需 Linux 专有的 `timerfd_create`，Linux/macOS 通用。

## 新增模块

| 文件 | 说明 |
|------|------|
| `include/timer/TimeStamp.h` | 微秒精度时间戳（header-only） |
| `include/timer/Timer.h` | 单个定时任务：到期时刻 + 回调 + 重复间隔 |
| `include/timer/TimerQueue.h` | 定时器有序队列（`std::set`） |
| `common/timer/TimerQueue.cpp` | 插入、超时计算、过期处理 |
| `test/TimerTest.cpp` | 定时器功能测试 |

## 构建 & 运行

```bash
cd HISTORY/day23
cmake -S . -B build && cmake --build build -j4

# 运行定时器测试
./build/TimerTest

# 运行 echo 服务器
./build/server
# 另一终端
./build/client
```

## 可执行文件

| 名称 | 说明 |
|------|------|
| `server` | Echo 服务器（带定时器） |
| `client` | TCP 客户端 |
| `TimerTest` | 定时器测试（一次性 + 重复 + 跨线程） |
| `ThreadPoolTest` | 线程池测试 |
| `StressTest` | 压力测试客户端 |

## 核心设计

- `TimerQueue` 使用 `std::set<{TimeStamp, Timer*}>` 按到期时刻排序
- `nextTimeoutMs()` 返回值直接传给 `poll()`/`kevent()` 的 timeout 参数
- 所有定时器操作通过 `runInLoop` 投递，TimerQueue 无需加锁
