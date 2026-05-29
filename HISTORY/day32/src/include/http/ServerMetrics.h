#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// ServerMetrics：全局可观测性指标收集器（lock-free，原子计数器）。
//
// 设计原则：
//   - 所有计数器使用 std::atomic，无锁安全递增，热路径开销 ≈ 一条 lock xadd 指令
//   - 延迟分桶用固定 bucket 数组，避免动态分配
//   - 单例模式，HttpServer 启动时自动注册 /metrics 端点
//
// 典型输出（Prometheus text format）：
//   mcpp_connections_total 42
//   mcpp_connections_active 7
//   mcpp_requests_total 12345
//   mcpp_request_duration_us_sum 678900
//   mcpp_request_duration_us_bucket{le="100"} 1000
//   ...
class ServerMetrics {
  public:
    static ServerMetrics &instance() {
        static ServerMetrics inst;
        return inst;
    }

    // ── 连接指标 ──────────────────────────────────────────────────
    void onConnectionAccepted() { connectionsTotal_.fetch_add(1, std::memory_order_relaxed); }

    int64_t connectionsTotal() const { return connectionsTotal_.load(std::memory_order_relaxed); }

    // ── 请求指标 ──────────────────────────────────────────────────
    void onRequestComplete(int statusCode, int64_t durationUs) {
        requestsTotal_.fetch_add(1, std::memory_order_relaxed);
        requestDurationUsSum_.fetch_add(durationUs, std::memory_order_relaxed);

        if (statusCode >= 200 && statusCode < 300)
            responses2xx_.fetch_add(1, std::memory_order_relaxed);
        else if (statusCode >= 400 && statusCode < 500)
            responses4xx_.fetch_add(1, std::memory_order_relaxed);
        else if (statusCode >= 500)
            responses5xx_.fetch_add(1, std::memory_order_relaxed);

        // 延迟分桶：≤100µs, ≤500µs, ≤1ms, ≤5ms, ≤10ms, ≤50ms, ≤100ms, >100ms
        if (durationUs <= 100)
            latencyBuckets_[0].fetch_add(1, std::memory_order_relaxed);
        else if (durationUs <= 500)
            latencyBuckets_[1].fetch_add(1, std::memory_order_relaxed);
        else if (durationUs <= 1000)
            latencyBuckets_[2].fetch_add(1, std::memory_order_relaxed);
        else if (durationUs <= 5000)
            latencyBuckets_[3].fetch_add(1, std::memory_order_relaxed);
        else if (durationUs <= 10000)
            latencyBuckets_[4].fetch_add(1, std::memory_order_relaxed);
        else if (durationUs <= 50000)
            latencyBuckets_[5].fetch_add(1, std::memory_order_relaxed);
        else if (durationUs <= 100000)
            latencyBuckets_[6].fetch_add(1, std::memory_order_relaxed);
        else
            latencyBuckets_[7].fetch_add(1, std::memory_order_relaxed);

        // 跟踪最大延迟（CAS 循环）
        int64_t prev = maxDurationUs_.load(std::memory_order_relaxed);
        while (durationUs > prev &&
               !maxDurationUs_.compare_exchange_weak(prev, durationUs, std::memory_order_relaxed))
            ;
    }

    // ── 流量指标 ──────────────────────────────────────────────────
    void addBytesRead(int64_t n) { bytesRead_.fetch_add(n, std::memory_order_relaxed); }
    void addBytesWritten(int64_t n) { bytesWritten_.fetch_add(n, std::memory_order_relaxed); }

    // ── 限流指标 ──────────────────────────────────────────────────
    void onRateLimited() { rateLimitedTotal_.fetch_add(1, std::memory_order_relaxed); }
    void onAuthRejected() { authRejectedTotal_.fetch_add(1, std::memory_order_relaxed); }

    // ── 导出为 Prometheus text exposition format ──────────────────
    std::string toPrometheus() const {
        std::string out;
        out.reserve(1024);

        auto line = [&](const char *name, int64_t val) {
            out += name;
            out += ' ';
            out += std::to_string(val);
            out += '\n';
        };

        out += "# HELP mcpp_connections_total Total accepted connections\n";
        out += "# TYPE mcpp_connections_total counter\n";
        line("mcpp_connections_total", connectionsTotal_.load(std::memory_order_relaxed));

        out += "# HELP mcpp_requests_total Total completed requests\n";
        out += "# TYPE mcpp_requests_total counter\n";
        line("mcpp_requests_total", requestsTotal_.load(std::memory_order_relaxed));

        out += "# TYPE mcpp_responses_2xx counter\n";
        line("mcpp_responses_2xx", responses2xx_.load(std::memory_order_relaxed));
        out += "# TYPE mcpp_responses_4xx counter\n";
        line("mcpp_responses_4xx", responses4xx_.load(std::memory_order_relaxed));
        out += "# TYPE mcpp_responses_5xx counter\n";
        line("mcpp_responses_5xx", responses5xx_.load(std::memory_order_relaxed));

        out += "# HELP mcpp_request_duration_us_sum Sum of request durations in microseconds\n";
        out += "# TYPE mcpp_request_duration_us_sum counter\n";
        line("mcpp_request_duration_us_sum", requestDurationUsSum_.load(std::memory_order_relaxed));

        out += "# HELP mcpp_request_duration_us_max Max request duration in microseconds\n";
        out += "# TYPE mcpp_request_duration_us_max gauge\n";
        line("mcpp_request_duration_us_max", maxDurationUs_.load(std::memory_order_relaxed));

        int64_t total = requestsTotal_.load(std::memory_order_relaxed);
        int64_t sum = requestDurationUsSum_.load(std::memory_order_relaxed);
        out += "# TYPE mcpp_request_duration_us_avg gauge\n";
        line("mcpp_request_duration_us_avg", total > 0 ? sum / total : 0);

        out += "# HELP mcpp_request_duration_us_bucket Latency distribution\n";
        out += "# TYPE mcpp_request_duration_us_bucket histogram\n";
        static constexpr const char *labels[] = {
            "mcpp_request_duration_us_bucket{le=\"100\"}",
            "mcpp_request_duration_us_bucket{le=\"500\"}",
            "mcpp_request_duration_us_bucket{le=\"1000\"}",
            "mcpp_request_duration_us_bucket{le=\"5000\"}",
            "mcpp_request_duration_us_bucket{le=\"10000\"}",
            "mcpp_request_duration_us_bucket{le=\"50000\"}",
            "mcpp_request_duration_us_bucket{le=\"100000\"}",
            "mcpp_request_duration_us_bucket{le=\"+Inf\"}",
        };
        int64_t cumulative = 0;
        for (int i = 0; i < kNumBuckets; ++i) {
            cumulative += latencyBuckets_[i].load(std::memory_order_relaxed);
            out += labels[i];
            out += ' ';
            out += std::to_string(cumulative);
            out += '\n';
        }

        out += "# HELP mcpp_bytes_read Total bytes read from clients\n";
        out += "# TYPE mcpp_bytes_read counter\n";
        line("mcpp_bytes_read", bytesRead_.load(std::memory_order_relaxed));

        out += "# HELP mcpp_bytes_written Total bytes written to clients\n";
        out += "# TYPE mcpp_bytes_written counter\n";
        line("mcpp_bytes_written", bytesWritten_.load(std::memory_order_relaxed));

        out += "# TYPE mcpp_rate_limited_total counter\n";
        line("mcpp_rate_limited_total", rateLimitedTotal_.load(std::memory_order_relaxed));

        out += "# TYPE mcpp_auth_rejected_total counter\n";
        line("mcpp_auth_rejected_total", authRejectedTotal_.load(std::memory_order_relaxed));

        return out;
    }

    // ── 重置（仅供测试使用）─────────────────────────────────────
    void reset() {
        connectionsTotal_.store(0, std::memory_order_relaxed);
        requestsTotal_.store(0, std::memory_order_relaxed);
        responses2xx_.store(0, std::memory_order_relaxed);
        responses4xx_.store(0, std::memory_order_relaxed);
        responses5xx_.store(0, std::memory_order_relaxed);
        requestDurationUsSum_.store(0, std::memory_order_relaxed);
        maxDurationUs_.store(0, std::memory_order_relaxed);
        for (int i = 0; i < kNumBuckets; ++i)
            latencyBuckets_[i].store(0, std::memory_order_relaxed);
        bytesRead_.store(0, std::memory_order_relaxed);
        bytesWritten_.store(0, std::memory_order_relaxed);
        rateLimitedTotal_.store(0, std::memory_order_relaxed);
        authRejectedTotal_.store(0, std::memory_order_relaxed);
    }

  private:
    ServerMetrics() = default;

    static constexpr int kNumBuckets = 8;

    std::atomic<int64_t> connectionsTotal_{0};
    std::atomic<int64_t> requestsTotal_{0};
    std::atomic<int64_t> responses2xx_{0};
    std::atomic<int64_t> responses4xx_{0};
    std::atomic<int64_t> responses5xx_{0};
    std::atomic<int64_t> requestDurationUsSum_{0};
    std::atomic<int64_t> maxDurationUs_{0};
    std::atomic<int64_t> latencyBuckets_[kNumBuckets]{};
    std::atomic<int64_t> bytesRead_{0};
    std::atomic<int64_t> bytesWritten_{0};
    std::atomic<int64_t> rateLimitedTotal_{0};
    std::atomic<int64_t> authRejectedTotal_{0};
};
