/**
 * BenchmarkTest.cpp — HTTP 服务器性能基准测试（内置 mini 压测工具）
 *
 * 功能：
 *   - 用 N 个线程，每个线程复用长连接持续发送 HTTP GET 请求
 *   - 统计 QPS（每秒请求数）、平均延迟、最小/最大延迟
 *   - 无需外部工具（wrk/webbench），项目自包含
 *
 * 用法（在 build/ 目录下运行）：
 *   ./BenchmarkTest [host] [port] [url] [threads] [duration_sec]
 *   ./BenchmarkTest 127.0.0.1 8888 / 4 10
 *
 * 单位说明：
 *   - QPS：requests/second（越高越好）
 *   - 延迟：microseconds（越低越好）
 */

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ── 配置参数 ──────────────────────────────────────────────────────────────────
struct Config {
    std::string host    = "127.0.0.1";
    int         port    = 8888;
    std::string url     = "/";
    int         threads = 4;
    int         durationSec = 10;
};

// ── 每线程统计 ────────────────────────────────────────────────────────────────
struct Stats {
    long long requests  = 0;
    long long errors    = 0;
    long long latencyUs = 0;  // 累积延迟（微秒）
    long long minUs     = std::numeric_limits<long long>::max();
    long long maxUs     = 0;
};

static std::atomic<bool> g_running{false};

// ── 建立 TCP 连接 ─────────────────────────────────────────────────────────────
static int connectToServer(const std::string &host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // TCP_NODELAY 减少 Nagle 算法延迟，提高短包测试的准确性
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ── 发送一个 HTTP GET 请求并接收响应头（keep-alive）────────────────────────────
static bool doRequest(int fd, const std::string &request,
                      char *recvBuf, int bufLen) {
    // 发送
    ssize_t nsent = 0;
    while (nsent < static_cast<ssize_t>(request.size())) {
        ssize_t n = write(fd, request.data() + nsent, request.size() - nsent);
        if (n <= 0) return false;
        nsent += n;
    }
    // 接收响应：读到 \r\n\r\n（headers 结束）即停止（简化处理，不解析 Content-Length）
    int total = 0;
    bool foundEnd = false;
    while (total < bufLen - 1) {
        ssize_t n = read(fd, recvBuf + total, bufLen - total - 1);
        if (n <= 0) return false;
        total += static_cast<int>(n);
        recvBuf[total] = '\0';
        if (strstr(recvBuf, "\r\n\r\n") != nullptr) {
            foundEnd = true;
            break;
        }
    }
    return foundEnd;
}

// ── 单个测试线程 ──────────────────────────────────────────────────────────────
static void workerThread(const Config &cfg, Stats &stats) {
    // 构造一次性 HTTP 请求字符串（keep-alive）
    std::string req =
        "GET " + cfg.url + " HTTP/1.1\r\n"
        "Host: " + cfg.host + "\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    char recvBuf[4096];
    int fd = -1;

    // 等待 g_running 变为 true（所有线程同步启动，减少偏差）
    while (!g_running.load(std::memory_order_acquire))
        std::this_thread::yield();

    while (g_running.load(std::memory_order_relaxed)) {
        // 连接不通则重连
        if (fd < 0) {
            fd = connectToServer(cfg.host, cfg.port);
            if (fd < 0) {
                ++stats.errors;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        bool ok = doRequest(fd, req, recvBuf, sizeof(recvBuf));
        auto t1 = std::chrono::steady_clock::now();

        if (!ok) {
            ++stats.errors;
            close(fd);
            fd = -1;
            continue;
        }

        long long us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        ++stats.requests;
        stats.latencyUs += us;
        if (us < stats.minUs) stats.minUs = us;
        if (us > stats.maxUs) stats.maxUs = us;
    }

    if (fd >= 0) close(fd);
}

// ── 打印进度条 ────────────────────────────────────────────────────────────────
static void printProgress(int elapsed, int total) {
    int width = 30;
    int filled = width * elapsed / total;
    std::cout << "\r  [";
    for (int i = 0; i < width; ++i) std::cout << (i < filled ? '#' : '-');
    std::cout << "] " << std::setw(3) << elapsed << "/" << total << "s" << std::flush;
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    Config cfg;
    if (argc > 1) cfg.host        = argv[1];
    if (argc > 2) cfg.port        = std::stoi(argv[2]);
    if (argc > 3) cfg.url         = argv[3];
    if (argc > 4) cfg.threads     = std::stoi(argv[4]);
    if (argc > 5) cfg.durationSec = std::stoi(argv[5]);

    std::cout << "╔══════════════════════════════════════╗\n"
              << "║   Airi-Cpp-Server-Lib — HTTP Benchmark    ║\n"
              << "╚══════════════════════════════════════╝\n\n"
              << "  Target  : http://" << cfg.host << ":" << cfg.port << cfg.url << "\n"
              << "  Threads : " << cfg.threads << "\n"
              << "  Duration: " << cfg.durationSec << " s\n\n";

    // 验证服务器是否可达
    int testFd = connectToServer(cfg.host, cfg.port);
    if (testFd < 0) {
        std::cerr << "[ERROR] Cannot connect to " << cfg.host << ":" << cfg.port << "\n"
                  << "        Please start the server first:\n"
                  << "          cd build && ./http_server\n";
        return 1;
    }
    close(testFd);
    std::cout << "  Server reachable ✓\n\n";

    // 启动工作线程
    std::vector<Stats> statsVec(cfg.threads);
    std::vector<std::thread> workers;
    workers.reserve(cfg.threads);
    for (int i = 0; i < cfg.threads; ++i)
        workers.emplace_back(workerThread, std::cref(cfg), std::ref(statsVec[i]));

    // 同步启动
    g_running.store(true, std::memory_order_release);
    auto startTime = std::chrono::steady_clock::now();

    // 等待测试时间结束（每秒打印一次进度）
    for (int i = 1; i <= cfg.durationSec; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printProgress(i, cfg.durationSec);
    }
    std::cout << "\n\n";

    g_running.store(false, std::memory_order_relaxed);
    for (auto &t : workers) t.join();

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startTime).count();

    // ── 汇总统计 ──────────────────────────────────────────────────────────────
    long long totalReqs = 0, totalErrors = 0, totalUs = 0;
    long long minUs = std::numeric_limits<long long>::max(), maxUs = 0;
    for (const auto &s : statsVec) {
        totalReqs   += s.requests;
        totalErrors += s.errors;
        totalUs     += s.latencyUs;
        if (s.minUs < minUs) minUs = s.minUs;
        if (s.maxUs > maxUs) maxUs = s.maxUs;
    }
    if (minUs == std::numeric_limits<long long>::max()) minUs = 0;

    double qps    = totalReqs / elapsed;
    double avgUs  = totalReqs > 0 ? static_cast<double>(totalUs) / totalReqs : 0;
    double mbSec  = qps * 200 / (1024.0 * 1024.0); // 估算吞吐（假设平均响应 200B）

    std::cout << "  ┌─────────────────────────────────────┐\n"
              << "  │           Benchmark Results          │\n"
              << "  ├─────────────────────────────────────┤\n"
              << "  │ Total requests : " << std::setw(16) << totalReqs  << "  │\n"
              << "  │ Total errors   : " << std::setw(16) << totalErrors << "  │\n"
              << "  │ Duration       : " << std::setw(13) << std::fixed
                                         << std::setprecision(2) << elapsed << " s" << "  │\n"
              << "  │ QPS            : " << std::setw(12)
                                         << std::setprecision(0) << qps << " req/s" << "  │\n"
              << "  │ Throughput     : " << std::setw(12)
                                         << std::setprecision(2) << mbSec << " MB/s" << "  │\n"
              << "  │ Latency avg    : " << std::setw(12)
                                         << std::setprecision(1) << avgUs << " µs" << "  │\n"
              << "  │ Latency min    : " << std::setw(12) << minUs << " µs" << "  │\n"
              << "  │ Latency max    : " << std::setw(12) << maxUs << " µs" << "  │\n"
              << "  └─────────────────────────────────────┘\n\n";

    if (totalErrors > 0)
        std::cerr << "  [WARN] " << totalErrors << " errors (connection refused / timeout)\n\n";

    return 0;
}
