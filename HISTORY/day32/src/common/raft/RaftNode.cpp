// RaftNode.cpp —— 与本项目 EventLoop 融合的 Raft 选举实现（全异步 RPC 版）
//
// 阅读顺序：
//   §1 构造 / 析构 / start / stop      —— 线程编排与生命周期
//   §2 RPC server 回调（fire-and-forget）—— sub-reactor 不再被阻塞
//   §3 角色切换 + 选举定时器           —— 全部在 loop_ 线程，无锁
//   §4 选举主流程 startElection         —— 出站走 AsyncRpcClient
//   §5 心跳                             —— runEvery 回调
//
#include "raft/RaftNode.h"
#include "log/Logger.h"
#include <chrono>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace raft {

// ════════════════════════════════════════════════════════════════════════════
// §1  构造 / 析构 / start / stop
// ════════════════════════════════════════════════════════════════════════════

RaftNode::RaftNode(int id, std::vector<Peer> peers, uint16_t rpcPort)
    : id_(id),
      peers_(std::move(peers)),
      // Raft 安全性要求：超过半数节点同意才能做决定（选举/提交日志）。
      // 3 节点 → quorum=2；5 节点 → quorum=3。即使一个节点宕机仍可运作。
      quorum_(static_cast<int>(peers_.size()) / 2 + 1),
      rng_(std::random_device{}()),
      rpcServer_("0.0.0.0", rpcPort, /*ioThreads=*/1) {
    // 哨兵条目：让 lastLogIndex() 和 lastLogTerm() 在日志为空时也能安全返回。
    // log_[0].term = 0，任何真实日志条目的 term >= 1，不会与哨兵混淆。
    log_.push_back(LogEntry{0, ""});

    // 注册异步 handler：handler 立刻 return，由 done 异步回写响应
    rpcServer_.addHandler(
        "RequestVote",
        [this](const std::string &req, RpcServer::Done done) {
            handleRequestVote(req, std::move(done));
        });
    rpcServer_.addHandler(
        "AppendEntries",
        [this](const std::string &req, RpcServer::Done done) {
            handleAppendEntries(req, std::move(done));
        });
}

RaftNode::~RaftNode() { stop(); }

void RaftNode::start() {
    if (running_.exchange(true)) return; // 幂等

    // (a) Eventloop 线程：所有 Raft 状态变迁 + 出站 RPC IO 都在这条线程上
    loopThread_ = std::thread([this] {
        loop_.runInLoop([this] { resetElectionTimer(); });
        loop_.runEvery(0.05, [this] { heartbeatTick(); });
        loop_.loop();
    });

    // (b) RPC server 线程：rpcServer_.start() 阻塞，跑在自己的 std::thread
    rpcServerThread_ = std::thread([this] { rpcServer_.start(); });

    LOG_INFO << "[Node " << id_ << "] 已在端口 " << peers_[id_].port
             << " 启动（peers=" << peers_.size() << "）";
}

void RaftNode::stop() {
    if (!running_.exchange(false)) return;

    // 1. 停 RPC server（让 rpcServerThread_ 退出）
    rpcServer_.stop();
    if (rpcServerThread_.joinable()) rpcServerThread_.join();

    // 2. 先把所有 AsyncRpcClient 的析构投递到 loop_ 线程：
    //    它们持有的 Connection 必须在 loop_ 线程析构（poller 操作约束）。
    loop_.queueInLoop([this] { peerClients_.clear(); });

    // 3. 停 loop_（让 loopThread_ 退出）。setQuit + wakeup 后，
    //    loop_ 退出前会先 doPendingFunctors()，把上一步的 clear 跑掉。
    loop_.setQuit();
    loop_.wakeup();
    if (loopThread_.joinable()) loopThread_.join();

    LOG_INFO << "[Node " << id_ << "] 已停止";
}

// ════════════════════════════════════════════════════════════════════════════
// §2  RPC server 回调：fire-and-forget actor 模式
//
// RpcServer 在自己的 sub-reactor 线程上回调 handle*()。我们把"读写 Raft 状态
// + 生成响应"封装成 lambda 投递到 loop_，**不等结果**直接返回。
// 响应通过 done() 异步写回——sub-reactor 线程立刻可以处理下一个请求，
// 永远不会被同步阻塞，单 IO 线程也能处理任意并发的 inbound RPC。
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::handleRequestVote(const std::string &reqJson, RpcServer::Done done) {
    RequestVoteArgs args;
    try {
        args = json::parse(reqJson).get<RequestVoteArgs>();
    } catch (...) {
        done(R"({"term":0,"voteGranted":false})");
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        // 【Raft 规则 §5.1】任何 RPC 只要携带更高 term，接收方必须立刻退回 Follower。
        // 这保证了任期单调递增——旧 Leader 看到新 term 后不再自以为是 Leader。
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        bool grant = false;
        // 投票需同时满足三个条件（全部满足才投，任一不满足就拒绝）：
        if (
            // 条件①：候选人的 term 不小于我的 term。
            // term 相等（= 是候选人已在本 term 发起选举，我在同 term 还没投票）也可以。
            // 若 term < currentTerm_，说明候选人信息过时，直接拒绝。
            args.term >= currentTerm_.load() &&

            // 条件②：本任期内我还没投过票，或者我之前就已经投给了这个候选人。
            // 保证同一 term 内每个节点最多只投一票，防止选出两个 Leader。
            (votedFor_ == -1 || votedFor_ == args.candidateId) &&

            // 条件③：候选人的日志至少和我一样「新」（Raft 的选举安全性）。
            // 比较规则：先比最后一条日志的 term，term 更大的更新；
            //           term 相同时，日志更长的（index 更大）更新。
            // 目的：确保当选的 Leader 一定包含所有已提交的日志，不会丢数据。
            (args.lastLogTerm > lastLogTerm() ||
             (args.lastLogTerm == lastLogTerm() && args.lastLogIndex >= lastLogIndex()))
        ) {
            grant     = true;
            votedFor_ = args.candidateId;  // 记录投票，本 term 内不再投给其他人
            // 收到合法投票请求后重置自己的选举计时器：
            // 既然已经有候选人在运作，就不要再发起竞争选举，让它先跑完。
            resetElectionTimer();
            LOG_INFO << "[Node " << id_ << "] 已投票给节点 " << args.candidateId
                     << "，term=" << args.term;
        }

        RequestVoteReply reply{currentTerm_.load(), grant};
        done(json(reply).dump());
    });
}

void RaftNode::handleAppendEntries(const std::string &reqJson, RpcServer::Done done) {
    AppendEntriesArgs args;
    try {
        args = json::parse(reqJson).get<AppendEntriesArgs>();
    } catch (...) {
        done(R"({"term":0,"success":false})");
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        // 同 RequestVote：看到更高 term 立刻退位。
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        bool success = false;
        // 【Raft 规则 §5.2】只接受 term >= 当前 term 的 AppendEntries。
        // term 过时的心跳说明发送方是被淘汰的旧 Leader，直接拒绝（success=false）。
        // 拒绝时回包里携带 currentTerm_，让旧 Leader 知道自己已落后，从而退位。
        if (args.term >= currentTerm_.load()) {
            // 收到合法 Leader 的心跳，强制确认自己是 Follower。
            // 即使我是 Candidate（正在拉票），也必须承认已有合法 Leader 存在，放弃选举。
            state_.store(State::Follower);
            leaderId_ = args.leaderId;  // 记录当前 Leader，方便客户端重定向
            success   = true;
            // 【关键】重置选举计时器：收到 Leader 心跳 = Leader 还活着。
            // 只要心跳不断，Follower 的计时器就一直被压制，不会触发新选举。
            // 这就是 Leader 用心跳「维持统治」的核心机制。
            resetElectionTimer();
        }
        AppendEntriesReply reply{currentTerm_.load(), success};
        done(json(reply).dump());
    });
}

// ════════════════════════════════════════════════════════════════════════════
// §3  角色切换 + 选举定时器（必须在 loop_ 线程调用）
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::becomeFollower(uint64_t term) {
    if (state_.load() != State::Follower)
        LOG_INFO << "[Node " << id_ << "] " << stateName(state_.load()) << " → Follower（term="
                 << term << "）";
    state_.store(State::Follower);
    currentTerm_.store(term);
    votedFor_ = -1;
    resetElectionTimer();
}

void RaftNode::becomeCandidate() {
    // 【Raft 规则 §5.2】发起新选举时必须先递增自己的 term。
    // 这防止旧的投票响应（来自网络延迟）污染新一轮选举：
    // 旧回包的 term 不等于新 term，会在 onVoteReply 里被过滤掉。
    currentTerm_.store(currentTerm_.load() + 1);
    state_.store(State::Candidate);
    votedFor_             = id_;  // 候选人给自己投一票（Raft 允许自投）
    currentElectionVotes_ = 1;    // 票数从 1 开始（已含自己那票）
    LOG_INFO << "[Node " << id_ << "] 选举超时 → 候选人，term=" << currentTerm_.load();
    // 重置选举计时器：如果这一轮在随机超时内没选出 Leader（平票/网络分区），
    // 计时器到期后会自动发起下一轮选举（term 再递增）。随机超时使平票概率极低。
    resetElectionTimer();
}

void RaftNode::becomeLeader() {
    state_.store(State::Leader);
    leaderId_ = id_;
    LOG_INFO << "[Node " << id_ << "] *** 成为 LEADER，term=" << currentTerm_.load() << " ***";
    // 成为 Leader 后不再需要选举计时器（Leader 不会发起选举）。
    // 用 ++epoch 让所有已安排但尚未触发的 electionTimerFired 回调全部失效，
    // 避免 Leader 自己触发选举把自己选下去。
    ++electionEpoch_;
    // 立刻广播一次心跳，而不等 50ms runEvery 的下一次触发。
    // 目的：尽快通知所有 Follower「我是新 Leader」，压制它们的选举计时器，
    // 防止在第一个 50ms 窗口内 Follower 超时发起竞争选举。
    heartbeatTick();
}

void RaftNode::resetElectionTimer() {
    // EventLoop 的定时器无法主动取消，用「版本号」实现软取消：
    // 每次 reset 都递增 epoch，旧定时器触发时发现 epoch 不匹配就自动放弃。
    ++electionEpoch_;
    uint64_t myEpoch = electionEpoch_;  // 捕获当前 epoch 进 lambda
    // 随机化超时（150~300ms）是 Raft 避免同时选举的关键机制：
    // 如果所有节点超时相同，它们会在同一时刻都发起选举，导致无休止的平票。
    // 随机化后，最快超时的节点抢先发起，其他节点收到它的 RequestVote 后重置计时器，
    // 大概率只有一个节点真正进入 Candidate 状态。
    int timeoutMs = std::uniform_int_distribution<int>(150, 300)(rng_);
    loop_.runAfter(timeoutMs / 1000.0,
                   [this, myEpoch] { electionTimerFired(myEpoch); });
}

void RaftNode::electionTimerFired(uint64_t epoch) {
    // 守卫①：epoch 不匹配 = 这个 timer 已被 resetElectionTimer 「覆盖」，是旧的，直接丢弃。
    // 常见场景：收到 Leader 心跳后 resetElectionTimer，旧 timer 延迟触发 → 无效。
    if (epoch != electionEpoch_) return;
    // 守卫②：如果自己已经是 Leader，不应再发起选举（正常不会走到这里，双重保险）。
    if (state_.load() == State::Leader) return;
    // 选举超时 = Leader 失联（可能宕机或网络分区）。转为候选人，发起新一轮选举。
    becomeCandidate();
    startElection();
}

// ════════════════════════════════════════════════════════════════════════════
// §4  选举主流程：完全异步的 RPC 发射
//
// 旧版：把 RpcClient::call() 扔到 worker 线程，完成后 queueInLoop 回填。
// 新版：直接 asyncClient->callAsync(...)，IO + 超时全部在 loop_ 上完成，
//       回调本就在 loop_ 线程触发，无需手动跨线程切换。
// ════════════════════════════════════════════════════════════════════════════

AsyncRpcClient *RaftNode::getOrCreateClient(const Peer &peer) {
    auto it = peerClients_.find(peer.id);
    if (it != peerClients_.end()) return it->second.get();
    auto client = std::make_unique<AsyncRpcClient>(&loop_, peer.ip, peer.port);
    auto *raw   = client.get();
    peerClients_.emplace(peer.id, std::move(client));
    return raw;
}

void RaftNode::startElection() {
    uint64_t term     = currentTerm_.load();
    uint64_t lastIdx  = lastLogIndex();
    uint64_t lastTerm = lastLogTerm();

    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        RequestVoteArgs args{term, id_, lastIdx, lastTerm};
        std::string     reqJson = json(args).dump();
        int             peerId  = peer.id;

        AsyncRpcClient *client = getOrCreateClient(peer);
        client->callAsync("RequestVote", reqJson,
                          [this, term, peerId](bool ok, std::string respJson) {
                              RequestVoteReply reply{};
                              if (ok) {
                                  try {
                                      reply = json::parse(respJson).get<RequestVoteReply>();
                                  } catch (...) { ok = false; }
                              }
                              onVoteReply(term, peerId, ok, reply);
                          },
                          /*timeoutMs=*/150);
    }
}

void RaftNode::onVoteReply(uint64_t electionTerm, int peerId, bool ok, RequestVoteReply reply) {
    // 守卫①：过期回包过滤。
    // state_ != Candidate：在等待投票期间收到了更高 term 的消息，已退回 Follower。
    // currentTerm_ != electionTerm：网络延迟导致的旧选举回包（term 已更新），
    //   直接丢弃，否则旧票数会累积到新一轮选举里，破坏投票计数的正确性。
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) return;
    // ok=false：RPC 超时或网络故障，该节点的投票视为未到，不计入票数。
    if (!ok) return;
    // 【Raft 规则 §5.1】回包里的 term 比自己大，说明自己的 term 已过时。
    // 即使对方投了反对票，也要立刻退位——集群里已经有更新的任期在运行。
    if (reply.term > currentTerm_.load()) {
        becomeFollower(reply.term);
        return;
    }
    if (reply.voteGranted) {
        ++currentElectionVotes_;
        LOG_INFO << "[Node " << id_ << "] 收到节点 " << peerId
                 << " 的投票（已得票=" << currentElectionVotes_ << "/" << quorum_ << "）";
        // 票数达到法定多数（quorum），立刻成为 Leader。
        // 不等待所有节点回包——Raft 不需要全票，只需多数票即可保证安全性。
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §5  心跳：每 50ms 一次，仅 Leader 实际广播
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::heartbeatTick() {
    // runEvery 每 50ms 无条件触发，但只有 Leader 才实际广播。
    // Follower/Candidate 走到这里直接返回，零开销。
    if (state_.load() != State::Leader) return;
    uint64_t term = currentTerm_.load();

    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        AppendEntriesArgs args{term, id_};
        std::string       reqJson = json(args).dump();
        int               peerId  = peer.id;

        AsyncRpcClient *client = getOrCreateClient(peer);
        client->callAsync("AppendEntries", reqJson,
                          [this, peerId](bool ok, std::string respJson) {
                              AppendEntriesReply reply{};
                              if (ok) {
                                  try {
                                      reply = json::parse(respJson).get<AppendEntriesReply>();
                                  } catch (...) { ok = false; }
                              }
                              onHeartbeatReply(peerId, ok, reply);
                          },
                          /*timeoutMs=*/100);
    }
}

void RaftNode::onHeartbeatReply(int /*peerId*/, bool ok, AppendEntriesReply reply) {
    // 超时或网络故障：忽略。Leader 会在下一个 50ms 周期重发心跳，容错处理。
    if (!ok) return;
    // 回包 term 更大：说明集群已选出更新任期的 Leader，自己是「僵尸 Leader」。
    // 立刻退位，避免出现两个 Leader 同时运作（脑裂）。
    if (reply.term > currentTerm_.load()) becomeFollower(reply.term);
}

uint64_t RaftNode::lastLogIndex() const { return static_cast<uint64_t>(log_.size() - 1); }
uint64_t RaftNode::lastLogTerm() const { return log_.back().term; }

} // namespace raft
