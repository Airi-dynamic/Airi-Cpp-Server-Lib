#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ============================================================
//  高性能网络编程调试实验 —— 修复版服务器
//  请跟随 README.md 实验手册, 将修复代码逐步写入此文件
// ============================================================

struct Connection {
    int fd;
    // TODO [实验 2.1]: 将下面这行改为 std::atomic<int> bytes_processed{0};
    int bytes_processed = 0;

    Connection(int f) : fd(f) {
        std::cout << "[Connection fd=" << fd << "] Created\n";
    }
    ~Connection() {
        // TODO [实验 1.2]: 添加 if (fd >= 0) { close(fd); fd = -1; }
        std::cout << "[Connection fd=" << fd << "] Destroyed\n";
    }

    void handleMessage() {
        bytes_processed += 1024;
        std::cout << "[Connection fd=" << fd << "] Processed: "
                  << bytes_processed << " bytes\n";
    }
};

// ============================================================
// 实验 1.1: Use-After-Free —— 修复区域
// 提示: std::shared_ptr + 值捕获到 lambda
// ============================================================
void simulate_uaf() {
    std::cout << "\n=== [FIXED] 实验 1.1: Use-After-Free ===\n";
    // TODO: 参照 README 实验 1.1 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
// 实验 1.2: Memory Leak + FD Leak —— 修复区域
// 提示: std::unique_ptr + RAII (析构函数关闭 fd)
// ============================================================
void simulate_leak() {
    std::cout << "\n=== [FIXED] 实验 1.2: Memory Leak + FD Leak ===\n";
    // TODO: 参照 README 实验 1.2 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
// 实验 2.1: Data Race —— 修复区域
// 提示: 先修改上方 Connection 的 bytes_processed 为 atomic
// ============================================================
void simulate_race() {
    std::cout << "\n=== [FIXED] 实验 2.1: Data Race ===\n";
    // TODO: 参照 README 实验 2.1 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
// 实验 2.2: Deadlock —— 修复区域
// 提示: std::scoped_lock 同时获取多把锁
// ============================================================
std::mutex mtx_connMgr;
std::mutex mtx_logger;

void simulate_deadlock() {
    std::cout << "\n=== [FIXED] 实验 2.2: Deadlock ===\n";
    // TODO: 参照 README 实验 2.2 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
// 实验 3: Undefined Behavior —— 修复区域
// 提示: 边界检查 + std::vector::at()
// ============================================================
void simulate_ubsan() {
    std::cout << "\n=== [FIXED] 实验 3: Undefined Behavior ===\n";
    // TODO: 参照 README 实验 3 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
// 实验 4: Segfault —— 修复区域
// 提示: 正确分配内存 + 空指针防御
// ============================================================
void simulate_crash() {
    std::cout << "\n=== [FIXED] 实验 4: Segfault ===\n";
    // TODO: 参照 README 实验 4 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
// 实验 5: CPU Hotspot —— 修复区域
// 提示: std::string_view 避免按值传参的拷贝开销
// ============================================================
void simulate_cpu_hog() {
    std::cout << "\n=== [FIXED] 实验 5: CPU Hotspot ===\n";
    // TODO: 参照 README 实验 5 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
// 实验 6 & 7: Network Echo Server —— 修复区域
// 提示: 每次 accept 后的 client fd 必须 close
// ============================================================
void simulate_network() {
    std::cout << "\n=== [FIXED] 实验 6 & 7: Network Echo Server ===\n";
    // TODO: 参照 README 实验 6&7 修复方案, 在此写入完整修复代码
    std::cout << "[TODO] Please implement the fix.\n";
}

// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ./debugged_server <mode>\n\n"
                  << "  uaf|leak|race|deadlock|ubsan|crash|cpu|network\n";
        return 1;
    }

    std::string mode = argv[1];
    if      (mode == "uaf")      simulate_uaf();
    else if (mode == "leak")     simulate_leak();
    else if (mode == "race")     simulate_race();
    else if (mode == "deadlock") simulate_deadlock();
    else if (mode == "ubsan")    simulate_ubsan();
    else if (mode == "crash")    simulate_crash();
    else if (mode == "cpu")      simulate_cpu_hog();
    else if (mode == "network")  simulate_network();
    else std::cout << "Unknown mode: " << mode << "\n";

    return 0;
}
