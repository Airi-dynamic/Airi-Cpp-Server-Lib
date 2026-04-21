#pragma once
#include "Macros.h"
#include "log/LogStream.h"
#include <cstring>
#include <functional>

// Logger：同步日志前端。
//
// 使用方式（通过 LOG_* 宏）：
//   LOG_INFO << "connected fd=" << fd;
//   LOG_ERROR << "read failed: " << strerror(errno);
//
// 原理：每个 LOG_* 宏临时构造一个 Logger 对象，
// 调用 .stream() 返回内部 LogStream 引用，用户通过 << 链式写入内容，
// 语句结束时临时对象析构 → ~Logger() 追加文件名:行号 → g_output(data, len) 输出。
//
// 输出目标由全局函数指针 g_output 控制，默认写 stdout，
// 可通过 Logger::setOutput() 替换为 AsyncLogging::append（文件异步日志）。
class Logger {
  public:
    DISALLOW_COPY_AND_MOVE(Logger)

    enum LogLevel {
        DEBUG = 0,
        INFO,
        WARN,
        ERROR,
        FATAL,
        NUM_LOG_LEVELS,
    };

    // 编译期从 __FILE__ 中提取文件名（去掉路径前缀，减少日志体积）
    class SourceFile {
      public:
        SourceFile(const char *path) : data_(path) {
            const char *slash = strrchr(path, '/');
            if (slash)
                data_ = slash + 1;
            size_ = static_cast<int>(strlen(data_));
        }
        const char *data_;
        int size_;
    };

    Logger(SourceFile file, int line, LogLevel level);
    ~Logger(); // 触发输出：追加 " - file:line\n"，调用 g_output

    LogStream &stream(); // 返回内部流，供 << 链式调用

    // ── 全局配置 ──────────────────────────────────────────────────────────────
    static LogLevel logLevel();
    static void setLogLevel(LogLevel level);

    using OutputFunc = std::function<void(const char *data, int len)>;
    using FlushFunc = std::function<void()>;
    static void setOutput(OutputFunc fn); // fn 为空则恢复默认（stdout）
    static void setFlush(FlushFunc fn);   // fn 为空则恢复默认（fflush）

  private:
    // Impl：组装一行完整日志的内部类
    // 格式：YYYY-MM-DD HH:MM:SS.uuuuuu <tid> <LEVEL> <用户内容> - <file>:<line>\n
    class Impl {
      public:
        DISALLOW_COPY_AND_MOVE(Impl)
        Impl(LogLevel level, const SourceFile &file, int line);
        void finish(); // 追加 " - file:line\n"

        LogStream stream_;
        LogLevel level_;
        SourceFile file_;
        int line_;
    };

    Impl impl_;
};

// ── 全局日志级别（extern，在 Logger.cpp 中定义）──────────────────────────────
extern Logger::LogLevel g_logLevel;
inline Logger::LogLevel Logger::logLevel() { return g_logLevel; }

// ── LOG_* 宏 ─────────────────────────────────────────────────────────────────
// 注意：LOG_DEBUG 用 if 守卫，运行时跳过时不构造 Logger，用户消息不被求值（零开销）。
// 其余级别无守卫，直接构造临时 Logger。
#define LOG_DEBUG                                                                                   \
    if (Logger::logLevel() <= Logger::DEBUG)                                                       \
    Logger(__FILE__, __LINE__, Logger::DEBUG).stream()

#define LOG_INFO  Logger(__FILE__, __LINE__, Logger::INFO).stream()
#define LOG_WARN  Logger(__FILE__, __LINE__, Logger::WARN).stream()
#define LOG_ERROR Logger(__FILE__, __LINE__, Logger::ERROR).stream()
#define LOG_FATAL Logger(__FILE__, __LINE__, Logger::FATAL).stream()
