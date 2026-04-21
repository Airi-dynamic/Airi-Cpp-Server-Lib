#include "log/Logger.h"
#include "timer/TimeStamp.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

// ── 全局状态 ──────────────────────────────────────────────────────────────────

// 每个线程的顺序编号（可读性优于 std::this_thread::get_id() 的哈希值）
static std::atomic<int> g_nextTid{1};
static thread_local int tl_tid = g_nextTid++;

Logger::LogLevel g_logLevel = Logger::INFO; // 默认级别：忽略 DEBUG

static void defaultOutput(const char *data, int len) { fwrite(data, 1, len, stdout); }
static void defaultFlush() { fflush(stdout); }

static Logger::OutputFunc g_output = defaultOutput;
static Logger::FlushFunc g_flush = defaultFlush;

// ── 级别字符串（与 LogLevel 枚举严格对应，宽度对齐方便阅读）─────────────────
static const char *kLevelStr[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL",
};

// ── Logger::Impl ──────────────────────────────────────────────────────────────

Logger::Impl::Impl(LogLevel level, const SourceFile &file, int line)
    : stream_(), level_(level), file_(file), line_(line) {
    // 日志行头部：时间戳 + 线程编号 + 级别
    stream_ << TimeStamp::now().toString();
    stream_ << " T" << tl_tid << ' ';
    stream_ << kLevelStr[level] << ' ';
    // 用户内容在此之后通过 LOG_* << "..." 追加
}

void Logger::Impl::finish() {
    // 追加来源位置，结尾换行
    stream_ << " - " << file_.data_ << ':' << line_ << '\n';
}

// ── Logger ───────────────────────────────────────────────────────────────────

Logger::Logger(SourceFile file, int line, LogLevel level) : impl_(level, file, line) {}

Logger::~Logger() {
    impl_.finish();
    const LogStream::Buffer &buf = impl_.stream_.buffer();
    g_output(buf.data(), buf.len());
    if (impl_.level_ == FATAL) {
        // FATAL 表示不可恢复错误：强制刷新后终止进程
        g_flush();
        abort();
    }
}

LogStream &Logger::stream() { return impl_.stream_; }

void Logger::setLogLevel(LogLevel level) { g_logLevel = level; }
void Logger::setOutput(OutputFunc fn) { g_output = fn ? std::move(fn) : OutputFunc(defaultOutput); }
void Logger::setFlush(FlushFunc fn) { g_flush = fn ? std::move(fn) : FlushFunc(defaultFlush); }
