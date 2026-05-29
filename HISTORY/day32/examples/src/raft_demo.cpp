// raft_demo.cpp —— Raft 选举演示（最多 10 节点，基于 EventLoop 重构版）
//
// 用法（N 节点集群，默认 3）：
//   ./raft_demo --id 0 [--nodes N]   # 第 1 个终端
//   ./raft_demo --id 1 [--nodes N]   # 第 2 个终端
//   ...
//   ./raft_demo --id N-1 [--nodes N] # 第 N 个终端
//
//   N 的范围：2 ~ 10（奇数集群推荐：3 / 5 / 7）
//   端口分配：节点 i 使用 18901 + i
//
// 预期：
//   - 启动后 150~300ms 内有一个节点打印 "*** 成为 LEADER ***"
//   - 其余节点的选举超时被 Leader 心跳重置，保持 Follower
//   - Ctrl+C 杀掉 Leader 后约 300ms 剩余节点重新选出新 Leader
//   - 逐一启动节点时，未就绪的对端连接拒绝属于正常现象（有退避机制）

#include "log/Logger.h"
#include "net/SignalHandler.h"
#include "raft/RaftNode.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    int myId  = -1;
    int nodes = 3; // 默认 3 节点集群

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--id") == 0)
            myId = std::stoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--nodes") == 0)
            nodes = std::stoi(argv[i + 1]);
    }

    if (nodes < 2 || nodes > 10) {
        std::cerr << "错误：--nodes 必须在 2~10 范围内\n";
        return 1;
    }
    if (myId < 0 || myId >= nodes) {
        std::cerr << "用法：raft_demo --id <0.." << (nodes - 1)
                  << "> [--nodes " << nodes << "]\n";
        return 1;
    }

    // 屏蔽 DEBUG 级别的连接生命周期日志，只看 INFO 及以上
    Logger::setLogLevel(Logger::INFO);

    // 预定义 10 个节点的地址与端口（端口 18901~18910）
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

    // 只取前 nodes 个节点组成本次集群
    std::vector<raft::Peer> peers(allPeers.begin(), allPeers.begin() + nodes);
    uint16_t myPort = peers[myId].port;

    raft::RaftNode node(myId, peers, myPort);

    static std::atomic<bool> stopFlag{false};
    Signal::signal(SIGINT,  [] { stopFlag.store(true); });
    Signal::signal(SIGTERM, [] { stopFlag.store(true); });

    node.start();

    // 主线程空转等待信号：node 的所有工作都在它自己的线程里跑
    while (!stopFlag.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    node.stop();
    return 0;
}
