#include "EventLoop.h"
#include "Channel.h"
#include "Poller/Poller.h"
#include "timer/TimerQueue.h"
#include "timer/TimeStamp.h"
#include "util.h"
#include <functional>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <sys/eventfd.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#endif

Eventloop::Eventloop() : poller_(nullptr), quit_(false), tid_(std::this_thread::get_id()) {
    // tid_ 在此捕获：调用构造函数的线程即为本 EventLoop 的归属线程。
    // 配合 EventLoopThread 使用后，构造和 loop() 调用均在同一子线程，
    // isInLoopThread() 的判断结果永远正确。
    poller_ = Poller::newDefaultPoller(this); // 工厂返回 unique_ptr，直接 move 赋值
    timerQueue_ = std::make_unique<TimerQueue>(this);

#ifdef __linux__
    evtfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ErrIf(evtfd_ == -1, "eventfd create error.");
    evtChannel_ = std::make_unique<Channel>(this, evtfd_);
#elif defined(__APPLE__)
    int pipeFds[2];
    ErrIf(pipe(pipeFds) == -1, "pipe create error.");
    wakeupReadFd_ = pipeFds[0];
    wakeupWriteFd_ = pipeFds[1];
    fcntl(wakeupReadFd_, F_SETFL, fcntl(wakeupReadFd_, F_GETFL) | O_NONBLOCK);
    fcntl(wakeupWriteFd_, F_SETFL, fcntl(wakeupWriteFd_, F_GETFL) | O_NONBLOCK);
    evtChannel_ = std::make_unique<Channel>(this, wakeupReadFd_);
#endif

    evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));
    evtChannel_->enableReading();
    // 唤醒 channel 不启用 ET，用 LT，确保每次都能被读到
}

Eventloop::~Eventloop() {
    // evtChannel_（unique_ptr）先析构（声明在 poller_ 之后，逆序析构）
    // poller_（unique_ptr）后析构——Channel 已注销后再销毁 poller，顺序安全
    // 文件描述符由平台 OS 对象管理，仍需手动关闭
#ifdef __linux__
    close(evtfd_);
#elif defined(__APPLE__)
    close(wakeupReadFd_);
    close(wakeupWriteFd_);
#endif
}

void Eventloop::handleWakeup() {
#ifdef __linux__
    uint64_t val;
    (void)read(evtfd_, &val, sizeof(val));
#elif defined(__APPLE__)
    char buf[256];
    while (read(wakeupReadFd_, buf, sizeof(buf)) > 0) {
    }
#endif
}

void Eventloop::wakeup() {
#ifdef __linux__
    uint64_t one = 1;
    (void)write(evtfd_, &one, sizeof(one));
#elif defined(__APPLE__)
    char buf = 'w';
    (void)write(wakeupWriteFd_, &buf, 1);
#endif
}

void Eventloop::loop() {
    while (!quit_) {
        // 动态计算 poll 超时：
        //   -1  → 无定时器，永久阻塞直到有 IO 事件或 wakeup
        //   0   → 有已过期定时器，立即返回
        //   >0  → 等待该毫秒数后最早的定时器将到期
        int timeout = timerQueue_->nextTimeoutMs();
        std::vector<Channel *> channels = poller_->poll(timeout);
        for (auto *ch : channels)
            ch->handleEvent();
        // 处理到期定时器（在 IO 事件之后、pending functors 之前）
        timerQueue_->processExpiredTimers();
        doPendingFunctors();
    }
}

void Eventloop::queueInLoop(std::function<void()> func) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(func);
    }
    wakeup();
}

void Eventloop::doPendingFunctors() {
    std::vector<std::function<void()>> functors;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (const auto &func : functors)
        func();
}

void Eventloop::updateChannel(Channel *ch) { poller_->updateChannel(ch); }
void Eventloop::deleteChannel(Channel *ch) { poller_->deleteChannel(ch); }
void Eventloop::setQuit() { quit_ = true; }

bool Eventloop::isInLoopThread() const { return tid_ == std::this_thread::get_id(); }

void Eventloop::runInLoop(std::function<void()> func) {
    if (isInLoopThread()) {
        // 当前线程即本 EventLoop 的归属线程，直接执行，无需排队
        func();
    } else {
        // 跨线程调用：投递到 pendingFunctors_，由归属线程在下次 doPendingFunctors() 时执行
        queueInLoop(std::move(func));
    }
}

void Eventloop::runAt(TimeStamp when, std::function<void()> cb) {
    // 通过 runInLoop 确保 addTimer 在 EventLoop 线程执行（TimerQueue 不加锁）
    runInLoop([this, when, cb = std::move(cb)]() mutable {
        timerQueue_->addTimer(when, std::move(cb), 0.0);
    });
}

void Eventloop::runAfter(double seconds, std::function<void()> cb) {
    runAt(TimeStamp::addSeconds(TimeStamp::now(), seconds), std::move(cb));
}

void Eventloop::runEvery(double seconds, std::function<void()> cb) {
    runInLoop([this, seconds, cb = std::move(cb)]() mutable {
        timerQueue_->addTimer(TimeStamp::addSeconds(TimeStamp::now(), seconds),
                              std::move(cb), seconds);
    });
}