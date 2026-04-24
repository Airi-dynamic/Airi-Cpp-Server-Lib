#pragma once
#include "Macros.h"
#include <cstdio>
#include <ctime>
#include <string>

// LogFile：日志文件写入器。
//
// 功能：
//   - 以 "<basename>.<YYYYMMDD_HHMMSS>.log" 命名自动创建日志文件；
//   - append() 追加字节流；
//   - flush() 刷新文件缓冲区；
//   - 当累计写入字节数超过 rollSizeBytes 时自动滚动（roll）到新文件。
//
// 线程安全：此类本身不加锁，由 AsyncLogging 的后端单线程独占调用，无需锁保护。
class LogFile {
  public:
    DISALLOW_COPY_AND_MOVE(LogFile)

    // basename    : 日志文件名前缀（不含路径和后缀），如 "server"
    // rollSizeBytes: 文件达到此字节数后自动滚动，默认 50MB
    explicit LogFile(const std::string &basename, size_t rollSizeBytes = 50 * 1024 * 1024);
    ~LogFile();

    void append(const char *data, int len); // 写入数据（写满则先 roll）
    void flush();                            // 刷新 FILE 缓冲区

  private:
    void rollFile(); // 关闭当前文件，用新时间戳打开下一个文件
    std::string getFileName() const; // 生成带时间戳的文件名

    std::string basename_;
    size_t rollSizeBytes_;
    size_t writtenBytes_; // 当前文件已写字节数
    FILE *fp_;
};
