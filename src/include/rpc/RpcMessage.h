#pragma once
#include <cstdint>
#include <string>

struct RpcMessage {
    enum class Type : uint32_t {
        kRequest = 0,
        kResponse = 1,
        kOneWay = 2,
    };

    Type type{Type::kRequest};
    uint32_t reqId{0};
    std::string method;
    std::string payload;

    std::string encode() const;
    static bool decode(const char *data, int len, RpcMessage *out, int *consumed);
};