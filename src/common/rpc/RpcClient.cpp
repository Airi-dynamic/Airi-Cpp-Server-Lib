#include "rpc/RpcClient.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static uint32_t g_reqId{0};
uint32_t RpcClient::nextReqId() { return ++g_reqId; }

RpcClient::RpcClient(const std::string &ip, uint16_t port, int timeoutMs)
    : ip_(ip), port_(port), timeoutMs_(timeoutMs) {}

bool RpcClient::call(const std::string &method, const std::string &requestJson,
                     std::string &responseJson) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    // 设置发送/接收超时，防止 Raft 选举被阻塞
    struct timeval tv;
    tv.tv_sec = timeoutMs_ / 1000;
    tv.tv_usec = (timeoutMs_ % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    ::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    RpcMessage req;
    req.type = RpcMessage::Type::kRequest;
    req.reqId = nextReqId();
    req.method = method;
    req.payload = requestJson;
    std::string frame = req.encode();

    // ── 1. 发送完整帧（循环 write 直到所有字节发完）──────────────────────────
    const char *p = frame.data();
    int rem = static_cast<int>(frame.size());
    while (rem > 0) {
        int n = static_cast<int>(::write(fd, p, rem));
        if (n <= 0) { ::close(fd); return false; }
        p += n;
        rem -= n;
    }

    // ── 2. 接收响应帧（循环 read 直到能解析出一条完整消息）──────────────────
    std::string recvBuf;
    char tmp[4096];
    while (true) {
        int n = static_cast<int>(::read(fd, tmp, sizeof(tmp)));
        if (n <= 0) { ::close(fd); return false; }
        recvBuf.append(tmp, n);

        RpcMessage resp;
        int consumed = 0;
        if (RpcMessage::decode(recvBuf.data(),
                               static_cast<int>(recvBuf.size()),
                               &resp, &consumed)) {
            if (resp.reqId == req.reqId) {
                responseJson = resp.payload;
                ::close(fd);
                return true;
            }
        }
    }
}