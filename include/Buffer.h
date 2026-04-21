#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

class Buffer {
  private:
    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;

    // 内部直接获取 vector 底层指针
    char *begin() { return &*buffer_.begin(); }
    const char *begin() const { return &*buffer_.begin(); }
    char *beginWrite() { return begin() + writerIndex_; }
    const char *beginWrite() const { return begin() + writerIndex_; }

    // 自动容量调整
    void makeSpace(size_t len);
    void ensureWritableBytes(size_t len);

  public:
    // 内部取数据
    void retrieveAll();
    void retrieve(size_t len);

    // 查询状态
    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char *peek() const { return begin() + readerIndex_; }
    static const size_t kCheapPrepand = 8;   // 头预留空间
    static const size_t kInitialSize = 1024; // 初始缓冲区大小

    // 构造函数，初始化 vector的大小，将读写指针放在预留区后
    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepand + initialSize), readerIndex_(kCheapPrepand),
          writerIndex_(kCheapPrepand) {}

    // 操作接口（取数据）
    std::string retrieveAsString(size_t len);
    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

    // 操作接口（写数据）
    void append(const char *data, size_t len);
    void append(const std::string &str) { append(str.data(), str.size()); }

    // 对外暴露的核心接口
    ssize_t readFd(int fd, int *savedErrno);
};