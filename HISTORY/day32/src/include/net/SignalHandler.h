#pragma once
//
// SignalHandler —— 异步信号安全的信号分发器
//
// 设计动机（修复旧版的致命缺陷）：
//   旧版直接在 OS 信号上下文里执行 `handlers_[sig]()`：
//     · std::map::operator[] 会在 key 不存在时插入元素（malloc，不是 async-signal-safe）
//     · std::function::operator() 不在 async-signal-safe 函数白名单内
//     · 若信号未注册过对应回调，operator[] 会默认构造空 function 然后调用
//       → 抛 bad_function_call → 信号上下文中无法 unwind → terminate
//   生产中表现为：偶发崩溃、收到未知信号直接挂掉。
//
// 修复方案：经典 self-pipe trick（与本项目 EventLoop 的 wakeup pipe 同思路）
//   1) OS 信号处理函数只做一件事：把信号编号 write 到一根管道（write 是
//      async-signal-safe 函数，POSIX 明确保证）。
//   2) 一个后台 dispatcher 线程阻塞 read 这根管道，在正常线程上下文中
//      查 handlers_ 并调用对应回调——这里随便用什么 STL 都安全。
//
// 接口契约（与旧版兼容，调用方零改动）：
//   Signal::signal(SIGINT, []{ srv.stop(); });
//   - 多次注册同一 sig 会覆盖。
//   - 回调在 dispatcher 线程被调用（非主线程，也非信号上下文）。
//   - 首次调用时 lazy 初始化 pipe 与 dispatcher 线程；析构于进程退出。
//
#include "Macros.h"
#include <csignal>
#include <functional>

class Signal {
  public:
    DISALLOW_COPY_AND_MOVE(Signal)

    // 注册信号回调：handler 在内部 dispatcher 线程上下文中触发，可安全使用任何 STL。
    static void signal(int sig, std::function<void()> handler);

  private:
    Signal() = delete;
};
