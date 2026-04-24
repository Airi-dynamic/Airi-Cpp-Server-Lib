#pragma once
#include "Macros.h"
#include <condition_variable>
#include <mutex>

// CountdownLatch：倒计时门闩，用于线程间的一次性启动同步。
//
// 典型用法：
//   Latch latch(1);
//   std::thread t([&]{ latch.countDown(); doWork(); });
//   latch.wait();   // 等待 t 进入 doWork 之前的准备阶段完成
//
// 此处用于 AsyncLogging：主线程调用 start() 后等待后端线程就绪再返回，
// 防止 append() 在后端 LogFile 初始化之前被调用。
class Latch {
  public:
    DISALLOW_COPY_AND_MOVE(Latch)
    explicit Latch(int count) : count_(count) {}

    // 等待计数归零
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return count_ <= 0; });
    }

    // 计数减一；归零时唤醒所有等待者
    void countDown() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (--count_ <= 0)
            cv_.notify_all();
    }

  private:
    int count_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
