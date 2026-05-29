#pragma once
#include "net/TcpServer.h"
#include "rpc/RpcMessage.h"
#include <functional>
#include <string>
#include <unordered_map>

// RpcServer：基于 TcpServer 的 RPC 监听端（全异步 handler 版）
//
// Handler 协议：
//   sub-reactor 解出一帧请求 → 调 handler(req, done)。
//   handler **不返回响应**，而是通过捕获的 done 回调把响应交给框架——
//   done 可在任意时刻、任意线程被触发，框架内部会自动 runInLoop 回到
//   对应连接的 owning loop 再写出响应。
//
//   这避免了旧版"handler 同步返回 → sub-reactor 必须阻塞"的反 reactor 模式。
//
// 用法：
//   RpcServer srv("0.0.0.0", 18901);
//   srv.addHandler("RequestVote",
//       [this](const std::string& req, RpcServer::Done done) {
//           loop_.runInLoop([this, req, done = std::move(done)]() mutable {
//               // ...处理 Raft 状态...
//               done("{\"voteGranted\":true}");
//           });
//       });
//   srv.start();
//
class RpcServer {
  public:
    using Done    = std::function<void(std::string responseJson)>;
    using Handler = std::function<void(const std::string &req, Done done)>;

    RpcServer(const std::string &ip, uint16_t port, int ioThreads = 1);

    void addHandler(const std::string &method, Handler handler);
    void start();
    void stop();

  private:
    void onMessage(Connection *conn);
    void onNewConn(Connection *conn);

    TcpServer                                server_;
    std::unordered_map<std::string, Handler> handlers_;
};
