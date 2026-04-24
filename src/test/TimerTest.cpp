// TimerTest.cpp
// 验证 EventLoop 定时器系统的基本功能：
//   1. runAfter：一次性定时器
//   2. runEvery：重复定时器
//   3. 跨线程投递：从主线程向子 Reactor 的 EventLoop 添加定时器
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "timer/TimeStamp.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    std::cout << "[TimerTest] start at " << TimeStamp::now().toString() << std::endl;

    // ── 场景 1：在当前线程的 EventLoop 中使用 runAfter / runEvery ─────────────
    EventLoopThread elt;
    Eventloop *loop = elt.startLoop(); // loop 在子线程中运行

    std::atomic<int> everyCount{0};

    // 每 0.5 秒打印一次（重复定时器）
    loop->runEvery(0.5, [&everyCount]() {
        ++everyCount;
        std::cout << "[runEvery] tick #" << everyCount
                  << " at " << TimeStamp::now().toString() << std::endl;
    });

    // 1 秒后打印一次（一次性定时器）
    loop->runAfter(1.0, []() {
        std::cout << "[runAfter] fired at " << TimeStamp::now().toString() << std::endl;
    });

    // 2.2 秒后停止循环
    loop->runAfter(2.2, [loop]() {
        std::cout << "[TimerTest] stopping loop..." << std::endl;
        loop->setQuit();
        loop->wakeup();
    });

    // ── 场景 2：从主线程跨线程投递定时器 ──────────────────────────────────────
    // 主线程 sleep 0.8 秒后，通过 runInLoop 向子线程 EventLoop 添加一个即时任务
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    loop->runInLoop([]() {
        std::cout << "[runInLoop from main thread] executed in loop thread at "
                  << TimeStamp::now().toString() << std::endl;
    });

    // 等待子线程（EventLoop）自然退出
    elt.join();

    std::cout << "[TimerTest] done. everyCount=" << everyCount
              << " (expected ~4 ticks in 2.2s)" << std::endl;
    return 0;
}
