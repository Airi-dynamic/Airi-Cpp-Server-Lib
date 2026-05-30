#pragma once
//
// RaftNode —— 基于本项目 EventLoop 的 Raft 节点（全异步 RPC + C++20 协程版）
//
// Day33 新增：
//   - AppendEntries 携带真实日志条目（prevLogIndex/prevLogTerm/entries/leaderCommit）
//   - Leader 维护 nextIndex_[]/matchIndex_[] per-peer 追踪状态
//   - commitIndex / lastApplied + applyCallback（复制状态机）
//   - sendHeartbeat → replicateLog（心跳与复制合一）
//   - propose(cmd)：外部写入接口（线程安全，内部 runInLoop）
//
#include "EventLoop.h"
#include "coro/Task.h"
#include "raft/RaftTypes.h"
#include "rpc/AsyncRpcClient.h"
#include "rpc/RpcServer.h"
#include <atomic>
#include <functional>
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

    // ── 外部只读快照（基于 atomic，无锁，可从任意线程调用）────────────
    State    getState() const { return state_.load(); }
    uint64_t getCurrentTerm() const { return currentTerm_.load(); }
    bool     isLeader() const { return state_.load() == State::Leader; }
    int      getId() const { return id_; }
    uint64_t getCommitIndex() const { return commitIndex_.load(); }
    uint64_t getLastApplied() const { return lastApplied_.load(); }
    int      getLeaderId() const { return leaderId_; }  // -1 = 未知
    uint64_t getLastLogIndex() const {
        // 仅近似值（loop_ 线程外读 log_ 可能不精确），仅供展示用
        return static_cast<uint64_t>(log_.size()) - 1;
    }

    // ── 写入接口（线程安全：内部 runInLoop 投递到 loop_ 线程）─────────
    // 若当前节点不是 Leader，命令被丢弃（真实系统应转发给 Leader）。
    // 返回时命令已入队（异步执行），不等待提交完成。
    void propose(const std::string &cmd);

    // ── 状态机回调（必须在 start() 之前注册）────────────────────────────
    // 当 lastApplied 推进时，在 loop_ 线程回调 cb(index, cmd)。
    void setApplyCallback(std::function<void(uint64_t, const std::string &)> cb) {
        applyCallback_ = std::move(cb);
    }

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

    // 选举：为每个 peer 发射 collectVote 协程（并发收票）
    void          runElection();
    FireAndForget collectVote(uint64_t electionTerm, Peer peer);

    // 日志复制 + 心跳合一：每次心跳 = 一次 replicateLog
    // 无新条目时 entries=[] 作为心跳；有新条目时附带日志段
    void          heartbeatTick();
    FireAndForget replicateLog(Peer peer);

    // 提交推进：Leader 在 matchIndex 更新后调用
    // 找到满足「多数 matchIndex[i] >= N 且 log[N].term == currentTerm」的最大 N
    void advanceCommitIndex();
    // 应用已提交但尚未 apply 的条目（lastApplied → commitIndex）
    void applyCommitted();

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
    std::vector<LogEntry> log_;           // log_[0] 是哨兵条目（term=0）
    std::atomic<State>    state_{State::Follower};
    int                   leaderId_{-1};
    int                   currentElectionVotes_{0};
    uint64_t              electionEpoch_{0};

    // ── 日志复制状态 ────────────────────────────────────────────────────
    std::atomic<uint64_t> commitIndex_{0};  // 已提交的最高 index
    std::atomic<uint64_t> lastApplied_{0};  // 已应用到状态机的最高 index

    // Leader 专属（仅在 loop_ 线程访问，becomeLeader 初始化，角色切换后可能过时）
    std::unordered_map<int, uint64_t> nextIndex_;   // peer.id → 下次发送的 index
    std::unordered_map<int, uint64_t> matchIndex_;  // peer.id → 已确认复制的最高 index

    // 状态机回调：commit 推进时在 loop_ 线程回调
    std::function<void(uint64_t, const std::string &)> applyCallback_;

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


