#include "EventLoopThreadPool.h"
#include "EventLoop.h"
#include "EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(Eventloop *mainLoop) : mainLoop_(mainLoop) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::setThreadNums(int n) { threadNums_ = n; }

void EventLoopThreadPool::start() {
    for (int i = 0; i < threadNums_; ++i) {
        auto t = std::make_unique<EventLoopThread>();
        // startLoop() 同步等待子线程构造好 EventLoop 再返回，
        // 返回后 loops_ 中的指针立即可用。
        loops_.push_back(t->startLoop());
        threads_.push_back(std::move(t));
    }
}

Eventloop *EventLoopThreadPool::nextLoop() {
    // 若没有子线程（单线程模式），回退到 mainLoop
    if (loops_.empty())
        return mainLoop_;
    Eventloop *loop = loops_[next_++];
    if (next_ == static_cast<int>(loops_.size()))
        next_ = 0; // 轮询回绕
    return loop;
}

void EventLoopThreadPool::stopAll() {
    for (auto *loop : loops_) {
        loop->setQuit();
        loop->wakeup(); // 唤醒阻塞在 poll() 中的线程，使其检测到 quit_ 后退出
    }
}

void EventLoopThreadPool::joinAll() {
    // 等待所有 IO 线程退出，不销毁 EventLoop 对象。
    // TcpServer::~TcpServer() 在成员析构前显式调用此方法，
    // 确保 connections_ 析构时调用 loop->deleteChannel() 不存在并发竞态。
    for (auto &t : threads_)
        t->join();
}

const std::vector<Eventloop *> &EventLoopThreadPool::loops() const { return loops_; }
