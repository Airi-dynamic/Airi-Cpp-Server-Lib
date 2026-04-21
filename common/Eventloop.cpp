#include "EventLoop.h"
#include "Channel.h"
#include "Poller/Poller.h"
#include "log/Logger.h"
#include "timer/TimeStamp.h"
#include "timer/TimerQueue.h"


#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
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
    if (evtfd_ == -1) {
        LOG_ERROR << "[Eventloop] eventfd 创建失败，错误=" << strerror(errno) << " errno=" << errno;
        throw std::runtime_error("eventfd create failed");
    }
    evtChannel_ = std::make_unique<Channel>(this, evtfd_);
#elif defined(__APPLE__)
    int pipeFds[2];
    if (pipe(pipeFds) == -1) {
        LOG_ERROR << "[Eventloop] pipe 创建失败，错误=" << strerror(errno) << " errno=" << errno;
        throw std::runtime_error("pipe create failed");
    }
    wakeupReadFd_ = pipeFds[0];
    wakeupWriteFd_ = pipeFds[1];
    int readFlags = fcntl(wakeupReadFd_, F_GETFL);
    int writeFlags = fcntl(wakeupWriteFd_, F_GETFL);
    if (readFlags == -1 || writeFlags == -1 ||
        fcntl(wakeupReadFd_, F_SETFL, readFlags | O_NONBLOCK) == -1 ||
        fcntl(wakeupWriteFd_, F_SETFL, writeFlags | O_NONBLOCK) == -1) {
        LOG_ERROR << "[Eventloop] pipe 设置非阻塞失败，错误=" << strerror(errno)
                  << " errno=" << errno;
        close(wakeupReadFd_);
        close(wakeupWriteFd_);
        wakeupReadFd_ = -1;
        wakeupWriteFd_ = -1;
        throw std::runtime_error("pipe nonblocking setup failed");
    }
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
    if (evtfd_ != -1)
        close(evtfd_);
#elif defined(__APPLE__)
    if (wakeupReadFd_ != -1)
        close(wakeupReadFd_);
    if (wakeupWriteFd_ != -1)
        close(wakeupWriteFd_);
#endif
}

void Eventloop::handleWakeup() {
#ifdef __linux__
    if (evtfd_ == -1)
        return;
    uint64_t val;
    while (true) {
        ssize_t n = read(evtfd_, &val, sizeof(val));
        if (n == static_cast<ssize_t>(sizeof(val)))
            return;
        if (n == -1 && errno == EINTR)
            continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        LOG_WARN << "[Eventloop] 处理 wakeup 读事件失败，错误=" << strerror(errno)
                 << " errno=" << errno;
        return;
    }
#elif defined(__APPLE__)
    if (wakeupReadFd_ == -1)
        return;
    char buf[256];
    while (true) {
        ssize_t n = read(wakeupReadFd_, buf, sizeof(buf));
        if (n > 0)
            continue;
        if (n == -1 && errno == EINTR)
            continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        return;
    }
#endif
}

void Eventloop::wakeup() {
#ifdef __linux__
    if (evtfd_ == -1)
        return;
    uint64_t one = 1;
    while (true) {
        ssize_t n = write(evtfd_, &one, sizeof(one));
        if (n == static_cast<ssize_t>(sizeof(one)))
            return;
        if (n == -1 && errno == EINTR)
            continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        LOG_WARN << "[Eventloop] wakeup 写入失败，错误=" << strerror(errno) << " errno=" << errno;
        return;
    }
#elif defined(__APPLE__)
    if (wakeupWriteFd_ == -1)
        return;
    char buf = 'w';
    while (true) {
        ssize_t n = write(wakeupWriteFd_, &buf, 1);
        if (n == 1)
            return;
        if (n == -1 && errno == EINTR)
            continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        LOG_WARN << "[Eventloop] wakeup 写入失败，错误=" << strerror(errno) << " errno=" << errno;
        return;
    }
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
        timerQueue_->addTimer(TimeStamp::addSeconds(TimeStamp::now(), seconds), std::move(cb),
                              seconds);
    });
}