// raft_demo.cpp —— Raft 选举 + 日志复制演示（最多 10 节点）
//
// 用法（N 节点集群，默认 3）：
//   ./raft_demo --id 0 [--nodes N] [--propose-interval <ms>]
//   ./raft_demo --id 1 [--nodes N]
//   ...
//
// Day33 新增：
//   --propose-interval <ms>   Leader 自动 propose 命令的间隔（默认 2000ms，0=不自动提交）
//
// 预期观察：
//   - 启动后约 150~300ms 内有一个节点打印 "*** 成为 LEADER ***"
//   - Leader 每隔 propose-interval 自动提交一条 "cmd-<n>" 命令
//   - Follower 的 commitIndex/lastApplied 跟随 Leader 推进
//   - Ctrl+C 杀掉 Leader 后约 300ms 剩余节点重新选出新 Leader

#include "log/Logger.h"
#include "net/SignalHandler.h"
#include "raft/RaftNode.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    int myId           = -1;
    int nodes          = 3;
    int proposeIntervalMs = 2000; // 默认每 2s propose 一条命令

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--id") == 0)
            myId = std::stoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--nodes") == 0)
            nodes = std::stoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--propose-interval") == 0)
            proposeIntervalMs = std::stoi(argv[i + 1]);
    }

    if (nodes < 2 || nodes > 10) {
        std::cerr << "错误：--nodes 必须在 2~10 范围内\n";
        return 1;
    }
    if (myId < 0 || myId >= nodes) {
        std::cerr << "用法：raft_demo --id <0.." << (nodes - 1)
                  << "> [--nodes " << nodes << "] [--propose-interval <ms>]\n";
        return 1;
    }

    Logger::setLogLevel(Logger::INFO);

    const std::vector<raft::Peer> allPeers = {
        {0, "127.0.0.1", 18901},
        {1, "127.0.0.1", 18902},
        {2, "127.0.0.1", 18903},
        {3, "127.0.0.1", 18904},
        {4, "127.0.0.1", 18905},
        {5, "127.0.0.1", 18906},
        {6, "127.0.0.1", 18907},
        {7, "127.0.0.1", 18908},
        {8, "127.0.0.1", 18909},
        {9, "127.0.0.1", 18910},
    };

    std::vector<raft::Peer> peers(allPeers.begin(), allPeers.begin() + nodes);
    uint16_t myPort = peers[myId].port;

    raft::RaftNode node(myId, peers, myPort);

    // 注册状态机回调：每条提交的命令都打印一行（可换成真正的 KV 应用）
    node.setApplyCallback([myId](uint64_t index, const std::string &cmd) {
        std::cout << "[Node " << myId << "] ✓ APPLIED  index=" << index
                  << "  cmd=" << cmd << "\n" << std::flush;
    });

    static std::atomic<bool> stopFlag{false};
    Signal::signal(SIGINT,  [] { stopFlag.store(true); });
    Signal::signal(SIGTERM, [] { stopFlag.store(true); });

    node.start();

    // ── 主循环：状态展示 + 自动 propose ────────────────────────────────
    int proposeCounter = 0;
    auto lastPropose   = std::chrono::steady_clock::now();

    while (!stopFlag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 状态行：每 500ms 打印一次
        const char *stateStr = "Follower";
        if (node.getState() == raft::State::Leader)   stateStr = "LEADER";
        if (node.getState() == raft::State::Candidate) stateStr = "Candidate";

        std::cout << "[Node " << myId << "] "
                  << stateStr
                  << "  term=" << node.getCurrentTerm()
                  << "  logSize=" << (node.getLastLogIndex() + 1)
                  << "  commit=" << node.getCommitIndex()
                  << "  applied=" << node.getLastApplied()
                  << "  leaderId=" << node.getLeaderId()
                  << "\n" << std::flush;

        // 自动 propose：仅当自己是 Leader 且开启了 propose
        if (proposeIntervalMs > 0 && node.isLeader()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastPropose).count();
            if (elapsed >= proposeIntervalMs) {
                lastPropose = now;
                std::string cmd = "cmd-" + std::to_string(proposeCounter++);
                std::cout << "[Node " << myId << "] → propose \"" << cmd << "\"\n"
                          << std::flush;
                node.propose(cmd);
            }
        }
    }

    node.stop();
    return 0;
}

