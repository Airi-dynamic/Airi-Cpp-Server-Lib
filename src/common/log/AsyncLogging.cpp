#include "log/AsyncLogging.h"
#include "log/LogFile.h"
#include <cassert>
#include <chrono>

AsyncLogging::AsyncLogging(const std::string &basename, size_t rollSizeBytes, int flushIntervalSec)
    : basename_(basename),
      rollSizeBytes_(rollSizeBytes),
      flushIntervalSec_(flushIntervalSec),
      current_(std::make_unique<Buffer>()),
      next_(std::make_unique<Buffer>()) {
    current_->bzero();
    next_->bzero();
    buffers_.reserve(16);
}

AsyncLogging::~AsyncLogging() {
    if (running_)
        stop();
}

void AsyncLogging::start() {
    running_ = true;
    thread_ = std::thread([this] { threadFunc(); });
    latch_.wait(); // 等待后端线程完成 LogFile 初始化后再返回
}

void AsyncLogging::stop() {
    running_ = false;
    cv_.notify_one(); // 唤醒后端线程以便它检测到 running_=false
    if (thread_.joinable())
        thread_.join();
}

// ── 前端（业务线程调用）─────────────────────────────────────────────────────
void AsyncLogging::append(const char *data, int len) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (current_->avail() >= len) {
        // 最常见路径：缓冲区有空间，直接 append，临界区极短
        current_->append(data, len);
    } else {
        // 缓冲区已满：将其移入等待队列
        buffers_.push_back(std::move(current_));

        // 优先使用预分配的备用缓冲，避免临界区内 new
        if (next_) {
            current_ = std::move(next_);
        } else {
            current_ = std::make_unique<Buffer>(); // 极少发生的慢路径
        }
        current_->append(data, len);
        cv_.notify_one(); // 通知后端立即写入
    }
}

// ── 后端（写线程）────────────────────────────────────────────────────────────
void AsyncLogging::threadFunc() {
    LogFile output(basename_, rollSizeBytes_);

    // 后端持有两个备用缓冲区，用于和前端缓冲区交换（避免在锁外 new）
    auto spare1 = std::make_unique<Buffer>();
    auto spare2 = std::make_unique<Buffer>();
    spare1->bzero();
    spare2->bzero();

    BufferVec localBuffers;
    localBuffers.reserve(16);

    latch_.countDown(); // 通知 start() 可以返回了

    while (running_) {
        // ── 进入临界区：与前端交换缓冲区 ──────────────────────────────────
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 等待：要么超时，要么前端写满了 buffer 并 notify
            cv_.wait_for(lock, std::chrono::seconds(flushIntervalSec_),
                         [this] { return !buffers_.empty(); });

            // 把当前前端缓冲区也纳入待写集合（无论是否满）
            buffers_.push_back(std::move(current_));
            current_ = std::move(spare1); // 用备用缓冲换出，前端可立即继续写
            current_->reset();

            localBuffers.swap(buffers_); // 把所有待写缓冲区移到本地（出锁前完成）

            if (!next_) {
                next_ = std::move(spare2); // 补充前端的备用缓冲
                next_->reset();
            }
        } // ── 临界区结束，以下为无锁磁盘 IO ───────────────────────────────

        // 防止前端日志爆发时积压过多缓冲区（超过 25 个 ≈ 100MB 就丢弃）
        if (localBuffers.size() > 25) {
            fprintf(stderr, "[AsyncLogging] Dropping %zu buffers (log overrun)\n",
                    localBuffers.size() - 2);
            localBuffers.erase(localBuffers.begin() + 2, localBuffers.end());
        }

        for (const auto &buf : localBuffers) {
            if (buf->len() > 0)
                output.append(buf->data(), buf->len());
        }

        // 归还两个缓冲区给备用位置（reset 后可复用，避免 new/delete）
        if (!spare1 && !localBuffers.empty()) {
            spare1 = std::move(localBuffers.back());
            spare1->reset();
            localBuffers.pop_back();
        }
        if (!spare2 && !localBuffers.empty()) {
            spare2 = std::move(localBuffers.back());
            spare2->reset();
            localBuffers.pop_back();
        }

        localBuffers.clear();
        output.flush();
    }

    // ── 退出时：刷完所有剩余数据 ────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_ && current_->len() > 0)
            buffers_.push_back(std::move(current_));
    }
    for (const auto &buf : buffers_) {
        if (buf && buf->len() > 0)
            output.append(buf->data(), buf->len());
    }
    output.flush();
}
