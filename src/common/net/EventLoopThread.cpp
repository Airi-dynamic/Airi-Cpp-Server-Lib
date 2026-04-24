#include "EventLoopThread.h"
#include "EventLoop.h"

EventLoopThread::EventLoopThread() = default;

EventLoopThread::~EventLoopThread() {
    // 确保线程已退出（调用方通常已通过 stop()+joinAll() 保证，此处兜底）
    join();
    // loop_ unique_ptr 随后自动析构：EventLoop 对象在此时才被销毁。
    // 这保证了在 TcpServer 析构序列中，Connection::~Connection() 调用
    // loop_->deleteChannel() 时 EventLoop 对象仍然有效。
}

Eventloop *EventLoopThread::startLoop() {
    // 启动子线程，子线程将在 threadFunc() 内构造 EventLoop
    thread_ = std::thread(&EventLoopThread::threadFunc, this);

    // 等待子线程通知 EventLoop 已就绪，再返回其指针
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return loopReady_; });
    }
    return loop_.get(); // 返回裸指针（不转移所有权）
}

void EventLoopThread::join() {
    if (thread_.joinable())
        thread_.join();
}

void EventLoopThread::threadFunc() {
    // EventLoop 在此处构造——调用构造函数的是子线程。
    // 构造时 EventLoop::tid_ = std::this_thread::get_id() 捕获子线程 ID，
    // 之后在同一线程内调用 loop()，isInLoopThread() 始终返回 true。
    loop_ = std::make_unique<Eventloop>();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loopReady_ = true;
        cv_.notify_one(); // 唤醒等待在 startLoop() 中的主线程
    }

    // 进入事件循环，阻塞直到外部调用 loop_->setQuit() + loop_->wakeup()
    loop_->loop();

    // loop() 返回后，loop_ 依然有效（由 EventLoopThread 持有 unique_ptr 所有权），
    // 待 EventLoopThread 对象被析构时才真正销毁 EventLoop。
}
