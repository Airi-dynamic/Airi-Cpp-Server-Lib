#pragma once
#include "Macros.h"
#include <memory>
#include <vector>

class Eventloop;
class EventLoopThread;

// EventLoopThreadPool 管理 N 个 IO 线程，每个线程拥有一个 EventLoop（sub-reactor）。
//
// 使用流程：
//   1. setThreadNums(n)  设置线程数
//   2. start()           启动所有线程，每个线程在内部构造 EventLoop 并运行 loop()
//   3. nextLoop()        轮询返回下一个 sub-reactor（供 TcpServer 给新连接选择归属）
//   4. stopAll()         通知所有 sub-loop 退出（setQuit + wakeup）
//   5. joinAll()         等待所有 IO 线程实际退出（不销毁 EventLoop 对象）
//
// 析构顺序安全性（详见 TcpServer.h 注释）：
//   TcpServer::~TcpServer() 先显式调 joinAll()，
//   之后 connections_ 析构时调 loop->deleteChannel() 不存在竞态，
//   最后 EventLoopThreadPool 析构时销毁 EventLoopThread 对象（进而销毁 EventLoop）。
class EventLoopThreadPool {
    DISALLOW_COPY_AND_MOVE(EventLoopThreadPool)
  public:
    explicit EventLoopThreadPool(Eventloop *mainLoop);
    ~EventLoopThreadPool();

    void setThreadNums(int n);

    // 启动所有 IO 线程。startLoop() 内部同步等待每个 EventLoop 就绪再继续。
    void start();

    // 轮询返回下一个 sub-reactor loop；若线程数为 0，回退到 mainLoop（单线程模式）
    Eventloop *nextLoop();

    // 令所有 sub-loop setQuit() + wakeup()，使线程从 poll() 返回并退出 loop()
    void stopAll();

    // 等待所有 IO 线程实际 join（不销毁 EventLoop 对象）
    void joinAll();

    // 返回所有 sub-loop 指针（TcpServer::stop() 读取）
    const std::vector<Eventloop *> &loops() const;

  private:
    Eventloop *mainLoop_; // 主 Reactor，nextLoop 在无子线程时的回退值
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<Eventloop *> loops_; // 各子线程的 EventLoop 裸指针（观察者，不拥有）
    int threadNums_{0};
    int next_{0}; // 轮询计数器
};
