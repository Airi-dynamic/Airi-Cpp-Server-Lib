#pragma once
#include "Macros.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class Eventloop;

// EventLoopThread 将"一个 EventLoop"与"一个操作系统线程"绑定在一起。
//
// 核心原则：EventLoop 必须在其将要运行的线程内部被构造，
// 这样 EventLoop::tid_（构造时捕获的线程 ID）才能与实际运行 loop() 的线程一致，
// isInLoopThread() / runInLoop() 才能正确判断跨线程任务是否需要排队。
//
// 生命周期：
//   - startLoop()  由主线程调用，阻塞直到子线程创建好 EventLoop 并发出就绪信号
//   - threadFunc() 在子线程中运行：构造 EventLoop → 通知 → 进入 loop()
//   - ~EventLoopThread() 确保线程已退出；EventLoop 对象随后由 unique_ptr 自动销毁
class EventLoopThread {
    DISALLOW_COPY_AND_MOVE(EventLoopThread)
  public:
    EventLoopThread();
    ~EventLoopThread();

    // 启动 IO 线程。主线程调用，返回子线程创建的 EventLoop 指针（已就绪）。
    Eventloop *startLoop();

    // 显式 join（析构函数也会调用；内部检查 joinable()，可安全多次调用）
    void join();

  private:
    void threadFunc(); // 子线程入口：构造 EventLoop → 通知主线程 → loop()

    // loop_ 由子线程构造（保证 tid_ 归属正确），由 EventLoopThread 持有所有权。
    // 线程退出后 loop_ 对象仍存活，直到 EventLoopThread 被销毁才析构。
    // 这使 TcpServer 能在"线程已退出 / EventLoop 还存在"的安全窗口内清理 Connection。
    std::unique_ptr<Eventloop> loop_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool loopReady_{false}; // 子线程完成 EventLoop 构造后置为 true，唤醒主线程
};
