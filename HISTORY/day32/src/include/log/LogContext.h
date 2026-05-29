#pragma once
#include <atomic>
#include <string>

// LogContext：线程局部结构化日志上下文
//
// 设计理念：
//   在请求处理的入口处调用 LogContext::set() 设置当前请求的上下文信息（请求 ID、方法、URL），
//   后续同线程内的所有 LOG_* 日志会自动携带该上下文，无需手动传参。
//   请求处理结束后调用 LogContext::clear() 清除（或使用 RAII guard 自动管理）。
//
// 线程安全：
//   使用 thread_local 存储，每个 sub-reactor 线程独立，无锁无竞态。
//
// 输出示例：
//   2024-01-01 12:00:00.000000 12345 INFO [req-42 GET /api/users] handler called - main.cpp:88
class LogContext {
  public:
    // 设置当前线程的日志上下文
    static void set(const std::string &requestId, const std::string &method,
                    const std::string &url) {
        auto &ctx = threadCtx();
        ctx.active = true;
        ctx.requestId = requestId;
        ctx.method = method;
        ctx.url = url;
    }

    // 设置简单上下文（仅请求 ID）
    static void set(const std::string &requestId) {
        auto &ctx = threadCtx();
        ctx.active = true;
        ctx.requestId = requestId;
        ctx.method.clear();
        ctx.url.clear();
    }

    // 清除当前线程的日志上下文
    static void clear() { threadCtx().active = false; }

    // 是否有活跃的上下文
    static bool hasContext() { return threadCtx().active; }

    // 格式化为日志前缀字符串：[req-42 GET /api/users]
    static std::string format() {
        auto &ctx = threadCtx();
        if (!ctx.active)
            return {};

        std::string result = "[";
        result += ctx.requestId;
        if (!ctx.method.empty()) {
            result += ' ';
            result += ctx.method;
        }
        if (!ctx.url.empty()) {
            result += ' ';
            result += ctx.url;
        }
        result += "] ";
        return result;
    }

    // 生成全局唯一请求 ID（原子递增，轻量无锁）
    static std::string nextRequestId() {
        uint64_t id = counter_.fetch_add(1, std::memory_order_relaxed);
        return "req-" + std::to_string(id);
    }

    // RAII guard：作用域结束自动清除上下文
    class Guard {
      public:
        Guard(const std::string &requestId, const std::string &method, const std::string &url) {
            LogContext::set(requestId, method, url);
        }
        explicit Guard(const std::string &requestId) { LogContext::set(requestId); }
        ~Guard() { LogContext::clear(); }

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
    };

  private:
    struct ThreadContext {
        bool active{false};
        std::string requestId;
        std::string method;
        std::string url;
    };

    static ThreadContext &threadCtx() {
        thread_local ThreadContext ctx;
        return ctx;
    }

    inline static std::atomic<uint64_t> counter_{1};
};
