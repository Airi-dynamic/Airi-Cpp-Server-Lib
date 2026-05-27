// rpc_demo.cpp —— Day37 端到端验证
//
//   服务端：./rpc_demo --server [--port 18901]
//   客户端：./rpc_demo --client [--port 18901] [--n 5]
//
//   客户端每次调用 "echo" 方法，服务端原样返回请求正文。
//   连续 n 次调用后打印统计并退出。

#include "net/SignalHandler.h"
#include "rpc/RpcClient.h"
#include "rpc/RpcServer.h"
#include <cstring>
#include <iostream>
#include <string>

// ── 解析命令行 ─────────────────────────────────────────────
static bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}
static int getIntArg(int argc, char** argv, const char* key, int def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::stoi(argv[i + 1]);
    return def;
}

// ── 服务端模式 ─────────────────────────────────────────────
static void runServer(uint16_t port) {
    RpcServer srv("0.0.0.0", port, /*ioThreads=*/1);

    // "echo"：原样返回请求正文
    srv.addHandler("echo", [](const std::string& req) -> std::string {
        return req;
    });
    // "add"：解析 {"a":x,"b":y}，返回 {"result":z}
    srv.addHandler("add", [](const std::string& req) -> std::string {
        // 极简解析，仅用于演示
        auto av = req.find("\"a\":"); auto bv = req.find("\"b\":");
        if (av == std::string::npos || bv == std::string::npos)
            return "{\"error\":\"bad input\"}";
        int a = std::stoi(req.substr(av + 4));
        int b = std::stoi(req.substr(bv + 4));
        return "{\"result\":" + std::to_string(a + b) + "}";
    });

    Signal::signal(SIGINT,  [&srv]() { srv.stop(); });
    Signal::signal(SIGTERM, [&srv]() { srv.stop(); });

    std::cout << "[server] RPC server listening on 0.0.0.0:" << port << "\n";
    srv.start(); // 阻塞，直到 stop()
    std::cout << "[server] stopped.\n";
}

// ── 客户端模式 ─────────────────────────────────────────────
static void runClient(uint16_t port, int n) {
    RpcClient client("127.0.0.1", port, /*timeoutMs=*/500);

    int ok = 0;
    for (int i = 0; i < n; ++i) {
        std::string req  = "{\"msg\":\"hello-" + std::to_string(i) + "\"}";
        std::string resp;
        if (client.call("echo", req, resp)) {
            std::cout << "[client] echo #" << i << " ✓  resp=" << resp << "\n";
            ++ok;
        } else {
            std::cout << "[client] echo #" << i << " ✗  (timeout/refused)\n";
        }
    }
    // 顺便测一次 add
    std::string addResp;
    if (client.call("add", "{\"a\":3,\"b\":4}", addResp))
        std::cout << "[client] add(3,4) => " << addResp << "\n";

    std::cout << "[client] done: " << ok << "/" << n << " success\n";
}

// ── main ───────────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t port = static_cast<uint16_t>(getIntArg(argc, argv, "--port", 18901));
    int      n    = getIntArg(argc, argv, "--n", 5);

    if (hasFlag(argc, argv, "--server")) {
        runServer(port);
    } else if (hasFlag(argc, argv, "--client")) {
        runClient(port, n);
    } else {
        std::cerr << "Usage:\n"
                  << "  rpc_demo --server [--port 18901]\n"
                  << "  rpc_demo --client [--port 18901] [--n 5]\n";
        return 1;
    }
    return 0;
}