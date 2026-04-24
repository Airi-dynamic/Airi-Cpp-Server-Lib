#pragma once
#include "Latch.h"
#include "Macros.h"
#include "log/LogStream.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// AsyncLogging：异步日志后端（双缓冲区 / double buffering）。
//
// 设计思想：
//   前端（业务线程）只需把日志写入内存缓冲区，不涉及任何磁盘 IO，
//   后端线程每隔 flushIntervalSec 秒（或缓冲区写满时立即）将数据批量刷写到 LogFile。
//
// 双缓冲区原理：
//   current_  ──→ 前端当前写入的缓冲区
//   next_     ──→ 预分配的备用缓冲区（避免 current_ 满时临时 new）
//   buffers_  ──→ 已满的缓冲区列表，等待后端写入
//
//   前端 append():
//     ① current_ 有空间 → 直接 append
//     ② 满了 → push current_ 到 buffers_，把 next_ 换为 current_，notify 后端
//   后端 threadFunc():
//     ① 等待（超时或被 notify）
//     ② swap current_ 和 buffers_ 到本地（临界区内）
//     ③ 批量写入 LogFile，归还缓冲区对象（避免频繁 new/delete）
//
// 使用方式：
//   AsyncLogging alog("server");
//   alog.start();
//   Logger::setOutput([&](const char* d, int n){ alog.append(d, n); });

class AsyncLogging {
  public:
    DISALLOW_COPY_AND_MOVE(AsyncLogging)

    // basename         : 日志文件名前缀
    // rollSizeBytes    : 单个日志文件最大字节数（默认 50MB）
    // flushIntervalSec : 后端刷写间隔（秒），默认 3s
    explicit AsyncLogging(const std::string &basename,
                          size_t rollSizeBytes = 50 * 1024 * 1024,
                          int flushIntervalSec = 3);
    ~AsyncLogging();

    void start(); // 启动后端写线程（start() 返回时后端已就绪）
    void stop();  // 停止后端写线程，刷完剩余数据

    // 前端调用：将一条日志追加到当前缓冲区（持锁，极短临界区）
    void append(const char *data, int len);

  private:
    using Buffer = FixedBuffer<kLargeBuffer>;
    using BufferVec = std::vector<std::unique_ptr<Buffer>>;

    void threadFunc(); // 后端线程主函数

    std::string basename_;
    size_t rollSizeBytes_;
    int flushIntervalSec_;

    std::atomic<bool> running_{false};
    Latch latch_{1};           // 保证 start() 返回时后端线程已初始化
    std::thread thread_;

    std::mutex mutex_;
    std::condition_variable cv_;

    std::unique_ptr<Buffer> current_; // 前端当前写入缓冲
    std::unique_ptr<Buffer> next_;    // 预分配备用缓冲
    BufferVec buffers_;               // 已满缓冲区列表
};
