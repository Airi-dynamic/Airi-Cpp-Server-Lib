#pragma once

#include "Macros.h"
#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

class Buffer {
    DISALLOW_COPY(Buffer)
  private:
    //     +-------------------+------------------+------------------+
    //     | Prependable Bytes |  Readable Bytes  |  Writable Bytes  |
    //     +-------------------+------------------+------------------+
    //     |                   |                  |                  |
    //     0      <=      readerIndex   <=   writerIndex    <=     size
    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;

    // 内部直接获取 vector 底层指针，类型为char*
    char *begin() { return &*buffer_.begin(); }
    // 内部直接获取 vector 底层指针，类型为const char*，且该函数承诺不修改任何数据
    const char *begin() const { return &*buffer_.begin(); }
    // 获取可写空间开头第一个字符的指针，类型为char*
    char *beginWrite() { return begin() + writerIndex_; }
    // 获取可写空间开头第一个字符的指针，类型为const char*，且该函数承诺不修改任何数据
    const char *beginWrite() const { return begin() + writerIndex_; }

    static const size_t kCheapPrepand = 8;   // 头预留空间
    static const size_t kInitialSize = 1024; // 初始缓冲区大小

    // 自动容量调整
    // 构造一个大小为 len 的空间，可能是内部数据移动，也可能新申请内存
    void makeSpace(size_t len);
    // 检查现在的可写空间是否够 len，不够就调用makeSpace
    void ensureWritableBytes(size_t len);

  public:
    // 获取可读空间开头第一个字符的指针，类型为const char*，且该函数承诺不修改任何数据
    const char *peek() const { return begin() + readerIndex_; }

    // 查询状态
    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    // 构造函数，初始化 vector的大小，将读写指针放在预留区后
    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepand + initialSize), readerIndex_(kCheapPrepand),
          writerIndex_(kCheapPrepand) {}

    // 操作接口（取数据）
    // 读取数据后调用此函数，通知缓冲区多少已经用完可以释放了，作为 outputBuffer 时用
    void retrieve(size_t len);
    // 先要求返回被读区内容再释放，作为 inputBuffer 时用
    std::string retrieveAsString(size_t len);
    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

    // 操作接口（写数据）
    // 确保空间够写，将数据拷入，后移写指针
    void append(const char *data, size_t len);
    void append(const std::string &str) { append(str.data(), str.size()); }

    // 对外暴露的核心接口
    // 承诺从给定的 fd 读取出数据放在缓冲区，返回读出字节数
    ssize_t readFd(int fd, int *savedErrno);
};