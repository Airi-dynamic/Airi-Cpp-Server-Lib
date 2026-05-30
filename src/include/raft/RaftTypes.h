#pragma once
//

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace raft {

enum class State { Follower, Candidate, Leader };

struct LogEntry {
    uint64_t    term{0};
    std::string cmd;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LogEntry, term, cmd)

struct RequestVoteArgs {
    uint64_t term{0};
    int      candidateId{-1};
    uint64_t lastLogIndex{0};
    uint64_t lastLogTerm{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RequestVoteArgs, term, candidateId, lastLogIndex, lastLogTerm)

struct RequestVoteReply {
    uint64_t term{0};
    bool     voteGranted{false};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RequestVoteReply, term, voteGranted)

// AppendEntries：日志复制 + 心跳合一（空 entries = 纯心跳）
struct AppendEntriesArgs {
    uint64_t              term{0};
    int                   leaderId{-1};
    // 一致性检查字段：「我发的这批条目之前紧接着哪一条？」
    uint64_t              prevLogIndex{0};
    uint64_t              prevLogTerm{0};
    // 要复制的日志条目（心跳时为空）
    std::vector<LogEntry> entries{};
    // Leader 当前已提交到的位置，Follower 用它来推进自己的 commitIndex
    uint64_t              leaderCommit{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesArgs,
    term, leaderId, prevLogIndex, prevLogTerm, entries, leaderCommit)

// 冲突提示（success=false 时有效）：让 Leader 快速定位应该回退到哪里
//   conflictTerm==0：Follower 日志太短，conflictIndex = len(log)
//   conflictTerm!=0：Follower 在 prevLogIndex 处的 term = conflictTerm，
//                    conflictIndex = 该 term 在 Follower 日志中的第一条 index
struct AppendEntriesReply {
    uint64_t term{0};
    bool     success{false};
    uint64_t conflictIndex{0};
    uint64_t conflictTerm{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesReply, term, success, conflictIndex, conflictTerm)

inline const char* stateName(State s) {
    switch (s) {
        case State::Follower:  return "Follower";
        case State::Candidate: return "Candidate";
        case State::Leader:    return "Leader";
    }
    return "?";
}

} // namespace raft
