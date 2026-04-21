#include "http/HttpContext.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "[HttpRequestLimitsTest] 失败: " << msg << "\n";
    std::exit(1);
}

void check(bool cond, const std::string &msg) {
    if (!cond)
        fail(msg);
}

void testRequestLineLimit() {
    std::cout << "[HttpRequestLimitsTest] 用例1：请求行长度限制\n";

    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 16;
    limits.maxHeaderBytes = 1024;
    limits.maxBodyBytes = 1024;

    HttpContext ctx(limits);
    const std::string req = "GET /0123456789abcdef HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    check(!ok, "超长请求行应解析失败");
    check(ctx.isInvalid(), "超长请求行后状态应为 Invalid");
    check(ctx.payloadTooLarge(), "超长请求行应标记 payloadTooLarge");
    check(consumed > 0, "失败请求应推进消费字节，避免上层死循环");
}

void testHeaderLimit() {
    std::cout << "[HttpRequestLimitsTest] 用例2：请求头长度限制\n";

    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 1024;
    limits.maxHeaderBytes = 24;
    limits.maxBodyBytes = 1024;

    HttpContext ctx(limits);
    const std::string req = "GET / HTTP/1.1\r\n"
                            "X-Long-Header: 12345678901234567890\r\n"
                            "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    check(!ok, "超长请求头应解析失败");
    check(ctx.isInvalid(), "超长请求头后状态应为 Invalid");
    check(ctx.payloadTooLarge(), "超长请求头应标记 payloadTooLarge");
}

void testBodyLimit() {
    std::cout << "[HttpRequestLimitsTest] 用例3：请求体长度限制\n";

    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 1024;
    limits.maxHeaderBytes = 1024;
    limits.maxBodyBytes = 4;

    HttpContext ctx(limits);
    const std::string req = "POST /upload HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "Content-Length: 10\r\n"
                            "\r\n"
                            "abcdefghij";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    check(!ok, "超长请求体应解析失败");
    check(ctx.isInvalid(), "超长请求体后状态应为 Invalid");
    check(ctx.payloadTooLarge(), "超长请求体应标记 payloadTooLarge");
}

void testValidRequestWithinLimit() {
    std::cout << "[HttpRequestLimitsTest] 用例4：限制内请求正常解析\n";

    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 128;
    limits.maxHeaderBytes = 256;
    limits.maxBodyBytes = 16;

    HttpContext ctx(limits);
    const std::string req = "POST /ok HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "Content-Length: 4\r\n"
                            "\r\n"
                            "ping";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    check(ok, "限制内请求应解析成功");
    check(ctx.isComplete(), "限制内请求应完成解析");
    check(ctx.request().url() == "/ok", "URL 应为 /ok");
    check(ctx.request().body() == "ping", "Body 应为 ping");
}

} // namespace

int main() {
    std::cout << "[HttpRequestLimitsTest] 开始执行\n";

    testRequestLineLimit();
    testHeaderLimit();
    testBodyLimit();
    testValidRequestWithinLimit();

    std::cout << "[HttpRequestLimitsTest] 全部通过\n";
    return 0;
}
