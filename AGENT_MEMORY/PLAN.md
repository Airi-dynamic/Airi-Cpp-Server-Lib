# Airi-Cpp-Server-Lib · 分布式存储速成计划（Day31–Day37）

## 目标
1 周内基于现有 C++ NetLib 构建：
- **Raft 共识层**（Leader Election + Log Replication + Persistence + Snapshot）
- **自研 LSM-Tree 存储引擎**（MemTable + WAL + SSTable + Compaction + BloomFilter）
- **分布式 KV 服务**（Raft + LSM 集成 + HTTP /kv/* 端点 + 故障注入演示）

面试叙事覆盖：CAP/BASE、共识协议、线性一致性、写读空间放大、网络分区。

---

## 七天主线

| Day | 主题 | 理论锚点 | 核心工程交付 |
|-----|------|----------|------------|
| 31 | RPC 基础层 | CAP、quorum、leader-based | RpcMessage / RpcServer / RpcClient，基于 TcpServer |
| 32 | Raft 选举 | term、随机超时、投票规则、split vote | RaftNode 三角色状态机、RequestVote RPC |
| 33 | Raft 日志复制 | log 一致性检查、commitIndex、Figure 8 | AppendEntries、log[]、nextIndex/matchIndex |
| 34 | Raft 持久化 + Snapshot | currentTerm/votedFor/log 持久化、log compaction | Storage 模块、InstallSnapshot RPC |
| 35 | LSM-Tree 单机引擎 | 写/读/空间放大、WAL、Bloom、Compaction | MemTable + WAL + SSTable + LsmEngine |
| 36 | Raft + LSM 集成 | 复制状态机、ReadIndex 线性一致读 | dist_kv_server / client、HTTP /kv/* |
| 37 | 故障注入 + 监控 + 面试 | 脑裂、慢盘、Compaction 阻塞 | chaos.sh、Prometheus raft/lsm 指标、INTERVIEW_NOTES |

---

## 进度追踪

- [x] Day31 — RPC 基础层
- [x] Day32 — Raft 选举（含 Day32.5 C++20 协程重构）
- [x] Day33 — Raft 日志复制
- [ ] Day34 — Raft 持久化 + Snapshot
- [ ] Day35 — LSM-Tree 单机引擎
- [ ] Day36 — Raft + LSM 集成
- [ ] Day37 — 故障注入 + 监控 + 面试冲刺

---

## 新增文件结构

```
src/
  include/
    rpc/         RpcMessage.h, RpcServer.h, RpcClient.h
    raft/        RaftTypes.h, RaftNode.h, Storage.h, Snapshot.h
    storage/     MemTable.h, WAL.h, SSTable.h, BloomFilter.h, LsmEngine.h
  common/
    rpc/         RpcServer.cpp, RpcClient.cpp
    raft/        RaftNode.cpp, Storage.cpp, Snapshot.cpp
    storage/     MemTable.cpp, WAL.cpp, SSTable.cpp, BloomFilter.cpp, LsmEngine.cpp
examples/src/
  rpc_demo.cpp
  dist_kv_server.cpp
  dist_kv_client.cpp
src/test/
  RpcTest.cpp
  RaftElectionTest.cpp
  RaftReplicationTest.cpp
  LsmEngineTest.cpp
  DistKvIntegrationTest.cpp
scripts/
  test_rpc.sh
  test_election.sh
  test_replication.sh
  test_persistence.sh
  test_dist_kv.sh
  chaos.sh
AGENT_MEMORY/
  PLAN.md           ← 本文件
  INTERVIEW_NOTES.md
dev-log/
  day37-RPC基础.md
  day38-Raft选举.md
  day39-Raft日志复制.md
  day40-Raft持久化与Snapshot.md
  day41-LSM-Tree单机引擎.md
  day42-Raft与LSM集成.md
  day43-故障注入与面试冲刺.md
```

---

## 端口约定（单机 3 进程）

| 节点 | Raft RPC 端口 | HTTP KV 端口 |
|------|--------------|--------------|
| node-0 | 18901 | 28901 |
| node-1 | 18902 | 28902 |
| node-2 | 18903 | 28903 |

---

## 决策记录

- **语言**：C++（复用 NetLib/Reactor/Buffer/TimerQueue/Logger/ServerMetrics）
- **KV 引擎**：自实现 LSM-Tree（深度学习存储内核 > 直接用 RocksDB）
- **序列化**：JSON 纯手写（可读可调试 > protobuf，不引入新依赖）
- **范围**：单机 3 进程演示；跨机器、分片、副本组管理列为"可扩展点"谈资
- **共识**：Raft，不实现 Multi-Paxos
- **读一致性**：Day42 实现 ReadIndex（强一致），Lease Read 列为谈资

---

## 风险与回退

1. Raft Day38 写不完 → 仅做 Election，Log Replication 顺延到 Day39 第一步
2. LSM Compaction 过复杂 → 先只做 minor (memtable→L0)，major compaction 作为面试谈资
3. Snapshot 工作量大 → Day40 先保证 log 持久化，InstallSnapshot 可推至 Day43

---

## 旧规划说明

dev-log/day31-36（WebSocket/协程/io_uring/无锁队列/内存池/muduo 对比）为未实施的旧规划，
HISTORY/day31-36/ 保留作为"曾考虑路线"。自 Day37 起项目转向分布式存储方向。
