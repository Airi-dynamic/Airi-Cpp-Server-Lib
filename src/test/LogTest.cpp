#include "log/AsyncLogging.h"
#include "log/Logger.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// ── 测试 1：同步日志输出到 stdout ────────────────────────────────────────────
static void testSync() {
    std::cout << "\n=== Test 1: Sync logging to stdout ===\n";

    Logger::setLogLevel(Logger::DEBUG);
    LOG_DEBUG << "debug message (visible only when level <= DEBUG)";
    LOG_INFO << "server started, port=" << 8080;
    LOG_WARN << "buffer near limit: " << 95 << "%";
    LOG_ERROR << "connection reset by peer, fd=" << 42;

    // 恢复默认级别
    Logger::setLogLevel(Logger::INFO);
    LOG_DEBUG << "this debug message should NOT appear (level=INFO)";
    LOG_INFO << "this INFO message should appear";
}

// ── 测试 2：异步日志写入文件 ─────────────────────────────────────────────────
static void testAsync() {
    std::cout << "\n=== Test 2: Async logging to file (basename: test_async_log) ===\n";

    // 创建异步日志后端：basename="test_async_log"，滚动大小 1MB，刷新间隔 1s
    AsyncLogging alog("test_async_log", 1 * 1024 * 1024, 1);
    alog.start();

    // 将 Logger 的输出函数重定向到 AsyncLogging::append
    Logger::setOutput([&alog](const char *data, int len) { alog.append(data, len); });
    Logger::setFlush([]() {}); // 刷新由 AsyncLogging 后端控制

    LOG_INFO << "AsyncLogging started, writing to file...";

    // 多线程并发写日志
    constexpr int kThreads = 4;
    constexpr int kLogsPerThread = 500;
    std::atomic<int> total{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &total]() {
            for (int i = 0; i < kLogsPerThread; ++i) {
                LOG_INFO << "thread=" << t << " seq=" << i;
                ++total;
            }
        });
    }
    for (auto &th : threads)
        th.join();

    LOG_INFO << "All threads done, total=" << total.load()
             << " logs, waiting for backend flush...";

    // 等待后端线程完成写入（flushInterval=1s，稍等即可）
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    alog.stop();

    // 恢复 stdout 输出
    Logger::setOutput(nullptr);
    Logger::setFlush(nullptr);

    std::cout << "Async log file written. Check test_async_log.*.log\n";
}

// ── 测试 3：Fmt 格式化辅助 ───────────────────────────────────────────────────
static void testFmt() {
    std::cout << "\n=== Test 3: Fmt formatting helper ===\n";
    double ratio = 98.567;
    int bytes = 1048576;
    LOG_INFO << "ratio=" << Fmt("%.2f", ratio) << "% bytes=" << Fmt("%d", bytes);
}

int main() {
    testSync();
    testFmt();
    testAsync();

    std::cout << "\n=== LogTest PASSED ===\n";
    return 0;
}
