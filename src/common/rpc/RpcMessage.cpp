#include "rpc/RpcMessage.h"
#include <arpa/inet.h>
#include <cstring>

// encode 把 method + payload 打包成 JSON，再加上 12 字节二进制头
std::string RpcMessage::encode() const {
    // 1. 构造 JSON body
    //    格式：{"method":"<method>","body":<payload>}
    //    payload 本身已经是合法 JSON 字符串（调用方保证）
    std::string json;
    json += "{\"method\":\"";
    json += method;
    json += "\",\"body\":";
    json += payload;
    json += "}";

    // 2. 计算总长度
    uint32_t payloadLen = static_cast<uint32_t>(json.size());

    // 3. 构造 12 字节定长头（全部转网络字节序）
    uint32_t netLen     = htonl(payloadLen);
    uint32_t netType    = htonl(static_cast<uint32_t>(type));
    uint32_t netReqId   = htonl(reqId);

    // 4. 拼装完整帧
    std::string frame;
    frame.resize(12 + payloadLen);
    memcpy(frame.data() + 0, &netLen,   4);
    memcpy(frame.data() + 4, &netType,  4);
    memcpy(frame.data() + 8, &netReqId, 4);
    memcpy(frame.data() + 12, json.data(), payloadLen);
    return frame;
}

// decode 尝试从字节流里解析出一条完整 RpcMessage
// 返回 false = 数据不足或格式错误
bool RpcMessage::decode(const char *data, int len,
                        RpcMessage *out, int *consumed) {
    // 1. 至少要有 12 字节头
    if (len < 12) return false;

    uint32_t netLen, netType, netReqId;
    memcpy(&netLen,   data + 0, 4);
    memcpy(&netType,  data + 4, 4);
    memcpy(&netReqId, data + 8, 4);

    uint32_t payloadLen = ntohl(netLen);
    out->type   = static_cast<RpcMessage::Type>(ntohl(netType));
    out->reqId  = ntohl(netReqId);

    // 2. 检查 payload 是否到齐（粘包处理核心）
    if (len < 12 + static_cast<int>(payloadLen)) return false;

    // 3. 解析 JSON：只提取 method 和 body
    //    格式：{"method":"<method>","body":<payload>}
    std::string json(data + 12, payloadLen);

    // 简单字符串解析：找 "method":"..." 和 "body":...
    auto findStr = [&](const std::string &key) -> std::string {
        std::string needle = "\"" + key + "\":\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    };

    out->method = findStr("method");

    // body 的值是从 "body": 之后到最后一个 } 之前的内容
    std::string bodyKey = "\"body\":";
    auto bodyPos = json.find(bodyKey);
    if (bodyPos != std::string::npos) {
        out->payload = json.substr(bodyPos + bodyKey.size(),
                                   json.size() - (bodyPos + bodyKey.size()) - 1);
    }

    *consumed = 12 + static_cast<int>(payloadLen);
    return true;
}