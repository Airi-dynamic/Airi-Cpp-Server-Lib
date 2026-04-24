#pragma once
#include "Macros.h"
#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>

static const int kSmallBuffer = 4096;         // 前端：每条日志最大 4KB
static const int kLargeBuffer = 4096 * 1024;  // 后端：每个异步缓冲区 4MB
static const int kMaxNumericSize = 48;         // 数字格式化的最大字符数

// FixedBuffer：固定大小的字节缓冲区（栈 / 成员变量分配，零堆操作）
// SIZE 由模板参数决定，前端用 kSmallBuffer，后端用 kLargeBuffer
template <int SIZE>
class FixedBuffer {
  public:
    FixedBuffer() : cur_(data_) {}

    void append(const char *buf, int len) {
        if (avail() > len) {
            memcpy(cur_, buf, len);
            cur_ += len;
        }
    }

    const char *data() const { return data_; }
    int len() const { return static_cast<int>(cur_ - data_); }
    char *current() { return cur_; }
    int avail() const { return static_cast<int>(end() - cur_); }
    void add(int n) { cur_ += n; }
    void reset() { cur_ = data_; }
    void bzero() {
        memset(data_, 0, sizeof(data_));
        cur_ = data_;
    }

  private:
    const char *end() const { return data_ + sizeof(data_); }
    char data_[SIZE];
    char *cur_;
};

// LogStream：流式日志缓冲区，提供 operator<< 接口。
// 内部持有 FixedBuffer<kSmallBuffer>，用于积累一条日志的所有字段。
// 不涉及 IO——只负责格式化和暂存，由 Logger::~Logger() 调用 g_output 输出。
class LogStream {
    DISALLOW_COPY_AND_MOVE(LogStream)
  public:
    using Buffer = FixedBuffer<kSmallBuffer>;

    LogStream() = default;
    ~LogStream() = default;

    void append(const char *data, int len) { buffer_.append(data, len); }
    const Buffer &buffer() const { return buffer_; }
    void resetBuffer() { buffer_.reset(); }

    LogStream &operator<<(bool v);
    LogStream &operator<<(short n);
    LogStream &operator<<(unsigned short n);
    LogStream &operator<<(int n);
    LogStream &operator<<(unsigned int n);
    LogStream &operator<<(long n);
    LogStream &operator<<(unsigned long n);
    LogStream &operator<<(long long n);
    LogStream &operator<<(unsigned long long n);
    LogStream &operator<<(float n);
    LogStream &operator<<(double n);
    LogStream &operator<<(char c);
    LogStream &operator<<(const char *str);
    LogStream &operator<<(const std::string &str);
    LogStream &operator<<(const void *ptr); // 指针地址（十六进制）

  private:
    // snprintf 写入 buffer_.current()，用于所有数值类型
    template <typename T>
    void formatNum(const char *fmt, T value) {
        if (buffer_.avail() >= kMaxNumericSize) {
            int n = snprintf(buffer_.current(), kMaxNumericSize, fmt, value);
            buffer_.add(n);
        }
    }

    Buffer buffer_;
};

// Fmt：printf 风格的格式化辅助，用于 << 链中插入格式化数值
// 示例：LOG_INFO << Fmt("%.2f", ratio) << " complete";
class Fmt {
  public:
    template <typename T>
    Fmt(const char *fmt, T val) {
        static_assert(std::is_arithmetic<T>::value, "Fmt only accepts arithmetic types");
        length_ = snprintf(buf_, sizeof(buf_), fmt, val);
        assert(static_cast<size_t>(length_) < sizeof(buf_));
    }
    const char *data() const { return buf_; }
    int length() const { return length_; }

  private:
    char buf_[32];
    int length_;
};

inline LogStream &operator<<(LogStream &s, const Fmt &fmt) {
    s.append(fmt.data(), fmt.length());
    return s;
}
