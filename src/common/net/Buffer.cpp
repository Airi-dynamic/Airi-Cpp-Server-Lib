#include "Buffer.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <sys/types.h>
#include <sys/uio.h>

void Buffer::makeSpace(size_t len) {
    // 情况 1: 头部挪出来的空闲 + 尾部空闲 < 需要的长度 + 必须保留的 prepend
    // 只能重新分配内存 (resize)
    if (writableBytes() + prependableBytes() < len + kCheapPrepand) {
        buffer_.resize(writerIndex_ + len);
    }
    // 情况 2: 空间其实够，只是被读指针占住了头部
    else {
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepand);
        readerIndex_ = kCheapPrepand;
        writerIndex_ = readerIndex_ + readable;
    }
}

void Buffer::ensureWritableBytes(size_t len) {
    if (writableBytes() < len)
        makeSpace(len);
}

void Buffer::retrieve(size_t len) {
    if (len < readableBytes()) {
        readerIndex_ += len;
    } else {
        readerIndex_ = kCheapPrepand;
        writerIndex_ = kCheapPrepand;
    }
}

void Buffer::append(const char *data, size_t len) {
    ensureWritableBytes(len);
    std::copy(data, data + len, beginWrite());
    writerIndex_ += len;
}

std::string Buffer::retrieveAsString(size_t len) {
    // 从 peek() 开始，拷贝 len 个字符
    std::string result(peek(), len);
    retrieve(len);
    return result;
}

ssize_t Buffer::readFd(int fd, int *savedErrno) {
    char extrabuf[65536];

    struct iovec vec[2];
    const size_t writable = writableBytes();

    // 缓冲区1：使用Buffer
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;

    // 缓冲区2：栈缓冲
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) < writable) {
        writerIndex_ += n;
    } else {
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
}