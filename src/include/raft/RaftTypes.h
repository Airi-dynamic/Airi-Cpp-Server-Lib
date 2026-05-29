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

// AppendEntries（Day32 只用心跳；Day33 会补 prevLogIndex / entries 等字段）
struct AppendEntriesArgs {
    uint64_t term{0};
    int      leaderId{-1};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesArgs, term, leaderId)

struct AppendEntriesReply {
    uint64_t term{0};
    bool     success{false};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesReply, term, success)

inline const char* stateName(State s) {
    switch (s) {
        case State::Follower:  return "Follower";
        case State::Candidate: return "Candidate";
        case State::Leader:    return "Leader";
    }
    return "?";
}

} // namespace raft
