#pragma once
#include "rpc/RpcMessage.h"
#include "net/TcpServer.h"
#include <functional>
#include <string>
#include <unordered_map>

// RpcServer：基于 TcpServer 的 RPC 监听端
//   用法：
//     RpcServer srv(loop, "0.0.0.0", 18901);
//     srv.addHandler("RequestVote", [](const std::string& req) -> std::string {
//         // req 是 JSON 字符串；返回 JSON 字符串作为响应
//         return "{\"voteGranted\":true}";
//     });
//     srv.start();

class RpcServer {
    public:
    // json 请求体处理函数
    using Handler = std::function<std::string(const std::string&)>;
    RpcServer(const std::string& ip, uint16_t port, int ioThreads = 1);

    void addHandler(const std::string& method, Handler handler);
    void start();
    void stop();

    private:
    void onMessage(Connection* conn);
    void onNewConn(Connection* conn);
    TcpServer server_;
    std::unordered_map<std::string, Handler> handlers_;
};