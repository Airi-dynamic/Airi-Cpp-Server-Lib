// RpcMessage —— 二进制帧协议实现
//
// 帧格式（12 字节定长头 + JSON payload）：
//   [4B length BE] [4B msgType BE] [4B reqId BE] [JSON body ...]
//
// JSON body 形如：{"method": "echo", "body": <任意 JSON 值>}

#include "rpc/RpcMessage.h"
#include <arpa/inet.h>
#include <cstring>
#include <nlohmann/json.hpp>

using nlohmann::json;

std::string RpcMessage::encode() const {
    // 1. 把 payload（调用方保证是合法 JSON 文本）解析成 JSON 值，再嵌入外层。
    //    payload 为空时 body 设为 null。
    json body = payload.empty() ? json(nullptr) : json::parse(payload);
    json envelope = {
        {"method", method},
        {"body",   body},
    };
    std::string jsonText = envelope.dump();
    uint32_t payloadLen = static_cast<uint32_t>(jsonText.size());

    // 2. 12 字节头（全部网络字节序）
    uint32_t netLen   = htonl(payloadLen);
    uint32_t netType  = htonl(static_cast<uint32_t>(type));
    uint32_t netReqId = htonl(reqId);

    // 3. 拼装完整帧
    std::string frame;
    frame.resize(12 + payloadLen);
    std::memcpy(frame.data() + 0, &netLen,   4);
    std::memcpy(frame.data() + 4, &netType,  4);
    std::memcpy(frame.data() + 8, &netReqId, 4);
    std::memcpy(frame.data() + 12, jsonText.data(), payloadLen);
    return frame;
}

bool RpcMessage::decode(const char *data, int len,
                        RpcMessage *out, int *consumed) {
    if (len < 12) return false;

    uint32_t netLen, netType, netReqId;
    std::memcpy(&netLen,   data + 0, 4);
    std::memcpy(&netType,  data + 4, 4);
    std::memcpy(&netReqId, data + 8, 4);

    uint32_t payloadLen = ntohl(netLen);
    if (len < 12 + static_cast<int>(payloadLen)) return false;

    out->type  = static_cast<RpcMessage::Type>(ntohl(netType));
    out->reqId = ntohl(netReqId);

    // 用 nlohmann::json 安全解析；任何异常都视为协议错误，丢弃这条帧
    try {
        json envelope = json::parse(data + 12, data + 12 + payloadLen);
        out->method   = envelope.value("method", std::string{});
        if (envelope.contains("body") && !envelope["body"].is_null()) {
            out->payload = envelope["body"].dump();
        } else {
            out->payload.clear();
        }
    } catch (const json::exception &) {
        return false;
    }

    *consumed = 12 + static_cast<int>(payloadLen);
    return true;
}
