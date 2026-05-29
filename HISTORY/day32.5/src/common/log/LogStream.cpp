#include "log/LogStream.h"
#include <cstring>

LogStream &LogStream::operator<<(bool v) {
    buffer_.append(v ? "true" : "false", v ? 4 : 5);
    return *this;
}

LogStream &LogStream::operator<<(short n) {
    formatNum("%hd", n);
    return *this;
}
LogStream &LogStream::operator<<(unsigned short n) {
    formatNum("%hu", n);
    return *this;
}
LogStream &LogStream::operator<<(int n) {
    formatNum("%d", n);
    return *this;
}
LogStream &LogStream::operator<<(unsigned int n) {
    formatNum("%u", n);
    return *this;
}
LogStream &LogStream::operator<<(long n) {
    formatNum("%ld", n);
    return *this;
}
LogStream &LogStream::operator<<(unsigned long n) {
    formatNum("%lu", n);
    return *this;
}
LogStream &LogStream::operator<<(long long n) {
    formatNum("%lld", n);
    return *this;
}
LogStream &LogStream::operator<<(unsigned long long n) {
    formatNum("%llu", n);
    return *this;
}
LogStream &LogStream::operator<<(float n) {
    formatNum("%g", n);
    return *this;
}
LogStream &LogStream::operator<<(double n) {
    formatNum("%.6g", n);
    return *this;
}

LogStream &LogStream::operator<<(char c) {
    buffer_.append(&c, 1);
    return *this;
}

LogStream &LogStream::operator<<(const char *str) {
    if (str)
        buffer_.append(str, static_cast<int>(strlen(str)));
    else
        buffer_.append("(null)", 6);
    return *this;
}

LogStream &LogStream::operator<<(const std::string &str) {
    buffer_.append(str.data(), static_cast<int>(str.size()));
    return *this;
}

LogStream &LogStream::operator<<(const void *ptr) {
    formatNum("%p", ptr);
    return *this;
}
