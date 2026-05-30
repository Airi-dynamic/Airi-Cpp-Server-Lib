# Day 33 — Raft 日志复制 / commitIndex 推进 / 复制状态机

## 0. 阅读指南

本文覆盖 Raft 日志复制（Log Replication）的完整实现，是 Day32（Leader 选举）的自然延续。  
推荐阅读顺序：

1. §1 引言 — 为什么仅有选举还不够
2. §2 AppendEntries 完整语义 — 一致性检查与日志截断
3. §3 nextIndex / matchIndex — Leader 侧的追踪状态
4. §4 commitIndex vs lastApplied — 提交与应用的分层
5. §5 replicateLog 协程 — 心跳与复制合一
6. §6 Figure 8 规则 — 只直接提交当前 term 的条目
7. §7 propose() — 线程安全的外部写入接口
8. §8 验证 — 冒烟测试输出分析

---

## 本日变更文件一览

| 文件 | 变更类型 | 主要改动 |
|------|----------|---------|
| `src/include/raft/RaftTypes.h` | 修改 | `AppendEntriesArgs` 增加 `prevLogIndex / prevLogTerm / entries[] / leaderCommit`；`AppendEntriesReply` 增加 `conflictIndex / conflictTerm`（冲突加速回退） |
| `src/include/raft/RaftNode.h` | 修改 | 新增 `nextIndex_ / matchIndex_`（Leader 追踪）；新增 `commitIndex_ / lastApplied_`（提交与应用）；新增 `applyCallback_`；移除 `sendHeartbeat`，新增 `replicateLog / advanceCommitIndex / applyCommitted`；新增 `propose() / setApplyCallback() / getCommitIndex() / getLastApplied() / getLeaderId() / getLastLogIndex()` 公开接口 |
| `src/common/raft/RaftNode.cpp` | 修改 | `becomeLeader` 初始化 `nextIndex/matchIndex`；`handleAppendEntries` 实现完整四规则一致性检查；`sendHeartbeat` → `replicateLog` 协程（心跳与复制合一）；新增 `advanceCommitIndex`（Figure 8 过滤）、`applyCommitted`、`propose`（线程安全写入） |
| `examples/src/raft_demo.cpp` | 修改 | 新增 `--propose-interval` 参数；状态行增加 `logSize / commit / applied / leaderId`；Leader 自动定期 propose；注册 `applyCallback` 打印已应用命令 |

---

## 1. 引言

Day32 实现了 Leader 选举：节点能在宕机后重选 Leader。  
但光有 Leader 还不够——Raft 的核心价值是**一致性日志**：任意数量的节点故障，集群对外的「已提交命令序列」不会出现分叉。

日志复制要解决三个问题：

| 问题 | Raft 解法 |
|------|-----------|
| 如何把命令广播给所有 Follower？ | AppendEntries RPC（带 prevLogIndex/prevLogTerm 一致性检查） |
| 何时认为一条命令「安全了」？ | 多数派确认 → commitIndex 推进 |
| Figure 8：新 Leader 能直接提交旧任期的条目吗？ | 不能，必须先提交至少一条当前 term 的条目 |

---

## 2. 改进 A — AppendEntries 完整语义

### 2.1 业务场景

Day32 的 `AppendEntries` 是占位心跳：只发 `{term, leaderId}`，Follower 只做"重置选举计时器"。  
Day33 需要真正的日志追加：Leader 发送 `[prevLogIndex, prevLogTerm, entries[], leaderCommit]`，Follower 在确认「历史一致」后才追加新条目。

### 2.2 改进思路

**前缀匹配不变量（Raft 论文 §5.3）**

Leader 向 Follower 发送：
```
AppendEntries(prevLogIndex=4, prevLogTerm=2, entries=[{term:3, cmd:"x"}, ...])
```
含义：「我在 index=4、term=2 处有一条日志，如果你那儿也是这样，我们的历史才相同。」

Follower 的一致性检查（4 步）：
1. `args.term < currentTerm` → 拒绝（过时 Leader）
2. `prevLogIndex > log.size()-1` → 拒绝（Follower 日志太短）
3. `log[prevLogIndex].term != prevLogTerm` → 拒绝（历史分叉）
4. 逐条幂等追加，遇 term 冲突则截断

**冲突提示优化（Conflict Hint）**

朴素退避：失败后 nextIndex-- 直到匹配，最差 O(log.length) 轮。  
优化：Follower 随回包带 `{conflictIndex, conflictTerm}`：
- `conflictTerm == 0`：日志太短，`conflictIndex = len(log)`
- `conflictTerm != 0`：在该 term 的第一条 index，Leader 直接跳过整个冲突 term

### 2.3 编码实现步骤

来自 [src/include/raft/RaftTypes.h](src/include/raft/RaftTypes.h)：

```cpp
struct AppendEntriesArgs {
    uint64_t              term{0};
    int                   leaderId{-1};
    uint64_t              prevLogIndex{0};
    uint64_t              prevLogTerm{0};
    std::vector<LogEntry> entries{};
    uint64_t              leaderCommit{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesArgs,
    term, leaderId, prevLogIndex, prevLogTerm, entries, leaderCommit)

// conflictTerm==0：Follower 日志太短，conflictIndex = len(log)
// conflictTerm!=0：conflictIndex = 该 term 在 Follower 日志中的第一条 index
struct AppendEntriesReply {
    uint64_t term{0};
    bool     success{false};
    uint64_t conflictIndex{0};
    uint64_t conflictTerm{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesReply, term, success, conflictIndex, conflictTerm)
```

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（`handleAppendEntries` 完整实现）：

```cpp
void RaftNode::handleAppendEntries(const std::string &reqJson, RpcServer::Done done) {
    AppendEntriesArgs args;
    try {
        args = json::parse(reqJson).get<AppendEntriesArgs>();
    } catch (...) {
        done(R"({"term":0,"success":false,"conflictIndex":0,"conflictTerm":0})");
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        AppendEntriesReply reply{currentTerm_.load(), false, 0, 0};

        // 规则①：过时 term 的 RPC 直接拒绝
        if (args.term < currentTerm_.load()) {
            done(json(reply).dump());
            return;
        }

        // 收到有效 Leader 的消息：确认自己是 Follower
        state_.store(State::Follower);
        leaderId_ = args.leaderId;
        resetElectionTimer();

        // 规则②（一致性检查）：是否存在 prevLogIndex 处的条目，且 term 匹配？
        if (args.prevLogIndex > lastLogIndex()) {
            reply.conflictIndex = lastLogIndex() + 1;
            reply.conflictTerm  = 0;
            done(json(reply).dump()); return;
        }
        if (log_[args.prevLogIndex].term != args.prevLogTerm) {
            uint64_t ct  = log_[args.prevLogIndex].term;
            uint64_t ci  = args.prevLogIndex;
            while (ci > 0 && log_[ci - 1].term == ct) --ci;
            reply.conflictIndex = ci;
            reply.conflictTerm  = ct;
            done(json(reply).dump()); return;
        }

        // 规则③：幂等追加（term 冲突则截断）
        uint64_t insertAt = args.prevLogIndex + 1;
        for (size_t i = 0; i < args.entries.size(); ++i) {
            uint64_t logIdx = insertAt + (uint64_t)i;
            if (logIdx < log_.size()) {
                if (log_[logIdx].term != args.entries[i].term) {
                    log_.resize(logIdx);
                    log_.push_back(args.entries[i]);
                }
            } else {
                log_.push_back(args.entries[i]);
            }
        }

        // 规则④：推进 commitIndex
        if (args.leaderCommit > commitIndex_.load()) {
            commitIndex_.store(std::min(args.leaderCommit, lastLogIndex()));
            applyCommitted();
        }

        reply.success = true;
        done(json(reply).dump());
    });
}
```

### 2.4 嵌入执行路径

```
(外部线程) TCP 读到完整帧 → RpcServer 解帧 → 分发给 "AppendEntries" handler
                                   ↓
handleAppendEntries(reqJson, done)   // sub-reactor 线程，立刻投递
                                   ↓
loop_.runInLoop(lambda)              // 投到 loop_ 线程队列
                                   ↓
lambda 在 loop_ 线程执行:
  检查 term → 一致性检查 → 截断/追加 → 更新 commitIndex → done(reply)
```

`runInLoop` 保证了所有 Raft 状态读写都在 loop_ 线程，完全无锁。

### 2.5 全流程追踪（从 propose 到 apply）

```
[Leader-loop_线程]
  propose("set x 1")
  → log_.push_back({term=1, cmd="set x 1"})      // index=1
  → replicateLog(peer0)  replicateLog(peer2)

[Follower0-loop_线程]  接收 AppendEntries(prev=0,term=0, entries=[{1,"set x 1"}])
  → log_[0].term==0 ✓ → 追加 log_[1]
  → commitIndex_:=min(0,1)=0（leaderCommit 首轮还是 0）
  → reply{success=true, ...}

[Leader-loop_线程]  replicateLog 协程恢复
  → matchIndex[0]=1, nextIndex[0]=2
  → advanceCommitIndex: N=1, log[1].term==currentTerm_, count=2 >= quorum=2
  → commitIndex_:=1 → applyCommitted → apply("set x 1")
  → 下次心跳 leaderCommit=1 发给 Follower → Follower commit+apply
```

---

## 3. 改进 B — nextIndex / matchIndex Leader 状态

### 3.1 业务场景

Leader 需要知道「每个 Follower 目前同步到哪里了」，才能决定下次发多少条目：
- **nextIndex[i]**：乐观估计，下次要发给 peer_i 的起始 index（初值 = lastLogIndex+1）
- **matchIndex[i]**：保守确认，peer_i 已复制到的最高 index（初值 = 0）

### 3.2 改进思路

`becomeLeader` 时初始化（乐观策略）：

```
nextIndex[i]  = lastLogIndex + 1   // 假设对方和我一样新
matchIndex[i] = 0                   // 尚未确认任何条目
```

**成功时**（一致性检查通过，Follower 追加了）：
```
newMatch = prevLogIndex + len(entries)
if newMatch > matchIndex[i]:
    matchIndex[i] = newMatch
    nextIndex[i]  = newMatch + 1
```
取 max 是为了安全处理并发 replicateLog 协程同时成功回包的情况。

**失败时**（一致性检查不通过）：
```
newNext = conflict_hint_based_calculation
nextIndex[i] = max(1, min(newNext, nextIndex[i]))  // 只能减小，不能增大
```
「只减不增」是 pipeline 安全的关键——若另一协程已成功更新了 nextIndex，不能把它覆盖回更小的值。

### 3.3 编码实现步骤

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（`becomeLeader` 初始化段）：

```cpp
void RaftNode::becomeLeader() {
    state_.store(State::Leader);
    leaderId_ = id_;
    LOG_INFO << "[Node " << id_ << "] *** 成为 LEADER，term=" << currentTerm_.load() << " ***";
    ++electionEpoch_;

    uint64_t nextIdx = lastLogIndex() + 1;
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        nextIndex_[peer.id]  = nextIdx;
        matchIndex_[peer.id] = 0;
    }

    heartbeatTick(); // 立刻广播一次复制（空 entries = 心跳）
}
```

---

## 4. 改进 C — commitIndex vs lastApplied

### 4.1 业务场景

Raft 把「多数派确认」和「应用到状态机」故意分成两步：

- **commitIndex**：已被多数派持久化，永远不会丢失（即使集群重启）
- **lastApplied**：已应用到本机状态机（KV 存储等），允许比 commitIndex 落后

分层的好处：commitIndex 推进（IO 内）和应用（业务处理）解耦，状态机崩溃重启后可以从持久化的快照重放到 lastApplied。

### 4.2 改进思路

`advanceCommitIndex` 找到满足「多数 matchIndex[i] >= N 且 log[N].term == currentTerm」的最大 N，然后：

```
commitIndex = N
applyCommitted()   // 把 (lastApplied, commitIndex] 逐条 apply
```

### 4.3 编码实现步骤

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)：

```cpp
void RaftNode::advanceCommitIndex() {
    uint64_t lastIdx = lastLogIndex();
    for (uint64_t n = lastIdx; n > commitIndex_.load(); --n) {
        if (log_[n].term != currentTerm_.load()) continue; // Figure 8 过滤
        int count = 1; // 算上自己
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            if (matchIndex_.count(peer.id) && matchIndex_[peer.id] >= n) ++count;
        }
        if (count >= quorum_) {
            commitIndex_.store(n);
            LOG_INFO << "[Node " << id_ << "] commitIndex 推进到 " << n
                     << "（term=" << log_[n].term << " cmd=" << log_[n].cmd << ")";
            applyCommitted();
            break;
        }
    }
}

void RaftNode::applyCommitted() {
    while (lastApplied_.load() < commitIndex_.load()) {
        uint64_t idx = lastApplied_.load() + 1;
        if (idx >= log_.size()) break;
        lastApplied_.store(idx);
        LOG_INFO << "[Node " << id_ << "] apply index=" << idx
                 << " cmd=" << log_[idx].cmd;
        if (applyCallback_) applyCallback_(idx, log_[idx].cmd);
    }
}
```

---

## 5. 改进 D — replicateLog 协程（心跳与复制合一）

### 5.1 业务场景

Day32 的 `sendHeartbeat` 只发空 `AppendEntries`——它是心跳但不复制日志。  
Day33 用 `replicateLog` 完全替代它：发的还是 `AppendEntries`，但附带 `[nextIndex, lastLogIndex]` 范围的日志段。如果范围为空，就退化为纯心跳。

### 5.2 改进思路

一条协程处理整个生命周期：
```
读 nextIndex → 构造 AppendEntriesArgs → co_await RPC → 处理回包
```
co_await 是挂起点，等待网络返回期间 loop_ 线程处理其他事件（其他 peer 的心跳、新的 propose 等）。

### 5.3 编码实现步骤

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)：

```cpp
FireAndForget RaftNode::replicateLog(Peer peer) {
    if (state_.load() != State::Leader) co_return;

    uint64_t ni = nextIndex_.count(peer.id) ? nextIndex_[peer.id] : lastLogIndex() + 1;
    if (ni < 1) ni = 1;

    uint64_t prevIdx  = ni - 1;
    uint64_t prevTerm = (prevIdx < log_.size()) ? log_[prevIdx].term : 0;

    AppendEntriesArgs args;
    args.term         = currentTerm_.load();
    args.leaderId     = id_;
    args.prevLogIndex = prevIdx;
    args.prevLogTerm  = prevTerm;
    args.leaderCommit = commitIndex_.load();
    for (uint64_t i = ni; i <= lastLogIndex(); ++i)
        args.entries.push_back(log_[i]);

    // ── 挂起点：发出 AppendEntries RPC ───────────────────────────────
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "AppendEntries", json(args).dump(), /*timeoutMs=*/100);

    // ── 恢复点（loop_ 线程）──────────────────────────────────────────
    if (!ok) co_return;
    if (state_.load() != State::Leader) co_return;

    AppendEntriesReply reply{};
    try { reply = json::parse(respJson).get<AppendEntriesReply>(); }
    catch (...) { co_return; }

    if (reply.term > currentTerm_.load()) { becomeFollower(reply.term); co_return; }

    if (reply.success) {
        uint64_t newMatch = args.prevLogIndex + (uint64_t)args.entries.size();
        if (newMatch > matchIndex_[peer.id]) {
            matchIndex_[peer.id] = newMatch;
            nextIndex_[peer.id]  = newMatch + 1;
        }
        advanceCommitIndex();
    } else {
        uint64_t newNext;
        if (reply.conflictTerm == 0) {
            newNext = reply.conflictIndex;
        } else {
            int64_t found = -1;
            for (int64_t i = (int64_t)lastLogIndex(); i >= 1; --i) {
                if (log_[i].term == reply.conflictTerm) { found = i + 1; break; }
            }
            newNext = (found >= 0) ? (uint64_t)found : reply.conflictIndex;
        }
        if (newNext < nextIndex_[peer.id])
            nextIndex_[peer.id] = std::max(newNext, uint64_t{1});
    }
}
```

### 5.4 协程并发安全分析

`replicateLog` 对同一个 peer 可能同时有多个实例在飞（heartbeatTick 每 50ms 触发，propose 也会触发）。两个实例可能都从 co_await 恢复并尝试更新 `matchIndex_/nextIndex_`：

- **成功路径**：只取更大的 `newMatch`（`if (newMatch > matchIndex_[peer.id])`），先回来的覆盖，后回来的被条件过滤——幂等。
- **失败路径**：只取更小的 `newNext`（`if (newNext < nextIndex_[peer.id])`），避免回退已经前进的 nextIndex。

两个方向都只做单调操作，不需要额外锁。

---

## 6. 改进 E — Figure 8 规则

### 6.1 业务场景

考虑下图场景（Raft 论文 Figure 8）：

```
时间线：
  t1: S1 当 Leader(term=2)，把 index=2 复制给了 S2（2/5 节点有）
  t2: S1 宕机，S5 当 Leader(term=3)，覆盖了 index=2
  t3: S1 重启当 Leader(term=4)，尝试提交 S3/S4/S5 都有的 index=2（term=2）
  t4: S1 宕机，S5 再当 Leader，用 term=3 的 index=2 覆盖掉 S3/S4 的 index=2(term=2)
```

如果 Leader 在 t3 直接提交「旧 term」的条目，t4 就会违反「已提交不会丢」的安全性。

**解法**：Leader 只直接提交「当前 term」的条目。旧 term 的条目作为「附带品」随着 commitIndex 单调推进而被顺带提交。

### 6.2 代码体现

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（`advanceCommitIndex` 中的过滤）：

```cpp
for (uint64_t n = lastIdx; n > commitIndex_.load(); --n) {
    if (log_[n].term != currentTerm_.load()) continue; // ← Figure 8 安全过滤
    ...
}
```

一行过滤，避开了论文中最难的安全性陷阱。

---

## 7. 改进 F — propose() 线程安全写入接口

### 7.1 业务场景

外部代码（HTTP handler、gRPC 前端、测试框架）运行在不同线程。直接操作 `log_` 会破坏 loop_ 线程的单线程假设。

### 7.2 改进思路

`propose` 只做一件事：把真正的操作投递到 loop_ 线程。调用方立即返回（Fire-and-forget 语义）。

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)：

```cpp
void RaftNode::propose(const std::string &cmd) {
    loop_.runInLoop([this, cmd] {
        if (state_.load() != State::Leader) {
            LOG_WARN << "[Node " << id_ << "] propose rejected: not leader"
                     << "（leaderId=" << leaderId_ << ")";
            return;
        }
        log_.push_back(LogEntry{currentTerm_.load(), cmd});
        LOG_INFO << "[Node " << id_ << "] append entry index=" << lastLogIndex()
                 << " cmd=" << cmd;
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            replicateLog(peer);
        }
    });
}
```

---

## 8. 验证

### 8.1 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target raft_demo -j
```

### 8.2 冒烟测试（3 节点，自动 propose）

```bash
./build/examples/raft_demo --id 0 --nodes 3 &
./build/examples/raft_demo --id 1 --nodes 3 &
./build/examples/raft_demo --id 2 --nodes 3 &
```

**实际输出（2026-05-30）**：
```
[Node 1] *** 成为 LEADER，term=1 ***          ← 215ms 内选出
[Node 1] → propose "cmd-0"
[Node 1] append entry index=1 cmd=cmd-0
[Node 1] commitIndex 推进到 1（term=1 cmd=cmd-0)
[Node 1] ✓ APPLIED  index=1  cmd=cmd-0
[Node 0] ✓ APPLIED  index=1  cmd=cmd-0        ← Follower 跟随
[Node 2] ✓ APPLIED  index=1  cmd=cmd-0

[Node 2] Follower  term=1  logSize=2  commit=1  applied=1  leaderId=1
[Node 0] Follower  term=1  logSize=2  commit=1  applied=1  leaderId=1
[Node 1] LEADER    term=1  logSize=2  commit=1  applied=1  leaderId=1
```

所有节点 commit=applied=1，日志一致。

---

## 9. 工程化

- **`RaftNode.h`**：新增 `propose()`、`setApplyCallback()`、`getCommitIndex()`、`getLastApplied()`、`getLeaderId()`、`getLastLogIndex()` 公开接口。
- **`raft_demo.cpp`**：新增 `--propose-interval <ms>` 参数，状态行包含 logSize/commit/applied/leaderId。
- **`RaftTypes.h`**：`AppendEntriesArgs`/`AppendEntriesReply` 完整字段 + NLOHMANN 宏注册。

---

## 10. 局限与下一步

| 局限 | 说明 | Day34 解法 |
|------|------|-----------|
| 无持久化 | currentTerm/votedFor/log 重启丢失，违反 Raft 安全性 | Storage 模块（WAL 实现）|
| 无快照 | 日志无限增长，大集群追赶慢 | InstallSnapshot RPC + 日志压缩 |
| propose 无返回 | 客户端不知道 index，无法实现线性一致读 | 返回 `future<uint64_t> index` |
| 无 ReadIndex | `linearizable read` 需要 Leader 确认自己仍是 Leader | ReadIndex 协议 |

**Day34 主题**：Raft 持久化（currentTerm/votedFor/log 写磁盘）+ Snapshot（InstallSnapshot RPC + 日志压缩），实现重启后状态恢复与慢节点追赶。
