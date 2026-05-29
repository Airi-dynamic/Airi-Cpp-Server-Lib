#pragma once
//
// RaftNode —— 基于本项目 EventLoop 的 Raft 节点（全异步 RPC + C++20 协程版）
//
// 设计要点：
//   1. 每个 RaftNode 持有一个自己的 Eventloop（loop_），跑在独立线程 loopThread_。
//      所有 Raft 状态（currentTerm_/votedFor_/log_/state_/...）只在 loop_ 线程
//      读写 → 完全无锁。
//
//   2. 选举定时器 = loop_->runAfter(timeoutSec, lambda{epoch})。
//      "取消"通过 epoch 版本号实现：每次 reset 时 ++electionEpoch_，
//      旧 lambda 触发时发现 epoch 不匹配自行 return。
//
//   3. 心跳 = loop_->runEvery(0.05, ...)，回调内自检 state_ == Leader 才广播。
//
//   4. 入站 RPC（fire-and-forget）：
//      RpcServer 在自己的 sub-reactor 线程回调 handler(req, done)。
//      handler 立刻把"处理 + done(payload)"投递到 loop_ 线程并返回，
//      sub-reactor 永不被同步阻塞。
//
//   5. 出站 RPC（协程版）：
//      collectVote / sendHeartbeat 是 FireAndForget 协程，在 loop_ 线程启动后
//      立刻挂起于 co_await callAsyncCo(...)，等待网络响应/超时后在 loop_ 线程
//      恢复执行。彻底消除旧版"startElection + onVoteReply 两段式跳板"。
//
// 外部只读访问（getCurrentTerm / getState / isLeader）通过 std::atomic 字段
// 暴露，调用方无需进入 loop_ 线程也能拿到一致快照。
//
#include "EventLoop.h"
#include "coro/Task.h"
#include "raft/RaftTypes.h"
#include "rpc/AsyncRpcClient.h"
#include "rpc/RpcServer.h"
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace raft {

struct Peer {
    int         id;
    std::string ip;
    uint16_t    port;
};

class RaftNode {
  public:
    RaftNode(int id, std::vector<Peer> peers, uint16_t rpcPort);
    ~RaftNode();

    // 非阻塞：启动 loop_ 线程 + RPC server 线程。
    void start();
    // 幂等：停止所有线程并 join。
    void stop();

    // 外部线程可调用的只读快照（基于 atomic，无锁）
    State    getState() const { return state_.load(); }
    uint64_t getCurrentTerm() const { return currentTerm_.load(); }
    bool     isLeader() const { return state_.load() == State::Leader; }
    int      getId() const { return id_; }

  private:
    // ── RPC server 回调（由 sub-reactor 线程调用，内部异步投递到 loop_）──
    void handleRequestVote(const std::string &reqJson, RpcServer::Done done);
    void handleAppendEntries(const std::string &reqJson, RpcServer::Done done);

    // ── 以下所有方法都必须在 loop_ 线程执行 ─────────────────────────
    void becomeFollower(uint64_t term);
    void becomeCandidate();
    void becomeLeader();

    void resetElectionTimer();
    void electionTimerFired(uint64_t epoch);

    // 选举主流程：为每个 peer 发射一个 collectVote 协程（并发收票）
    void runElection();
    // 心跳主循环：为每个 peer 发射一个 sendHeartbeat 协程
    void heartbeatTick();

    // ── 协程方法（FireAndForget，在 loop_ 线程启动并挂起）─────────────
    // collectVote: 向 peer 发送 RequestVote RPC，处理回包并更新投票计数。
    // 等效于旧版 startElection 中的 callAsync lambda + onVoteReply()。
    FireAndForget collectVote(uint64_t electionTerm, Peer peer);

    // sendHeartbeat: 向 peer 发送 AppendEntries(心跳) RPC，处理 term 退位。
    // 等效于旧版 heartbeatTick 中的 callAsync lambda + onHeartbeatReply()。
    FireAndForget sendHeartbeat(Peer peer);

    uint64_t lastLogIndex() const;
    uint64_t lastLogTerm() const;

    // 在 loop_ 线程内 lazy 创建 peer 对应的 AsyncRpcClient
    AsyncRpcClient *getOrCreateClient(const Peer &peer);

    // ── 配置 ────────────────────────────────────────────────────────────
    int               id_;
    std::vector<Peer> peers_;
    int               quorum_;

    // ── Raft 状态（只在 loop_ 线程读写；外部只读字段额外用 atomic 暴露）──
    std::atomic<uint64_t> currentTerm_{0};
    int                   votedFor_{-1};
    std::vector<LogEntry> log_;
    std::atomic<State>    state_{State::Follower};
    int                   leaderId_{-1};
    int                   currentElectionVotes_{0};
    uint64_t              electionEpoch_{0};

    // ── 基础设施 ────────────────────────────────────────────────────────
    Eventloop         loop_;
    std::thread       loopThread_;
    std::atomic<bool> running_{false};
    std::mt19937      rng_;

    RpcServer   rpcServer_;
    std::thread rpcServerThread_;

    // 出站 RPC：每个 peer 一个长连接异步客户端，复用 loop_ 的 IO。
    // 仅在 loop_ 线程访问。
    std::unordered_map<int, std::unique_ptr<AsyncRpcClient>> peerClients_;
};

} // namespace raft

