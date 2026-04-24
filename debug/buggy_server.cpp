#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// ============================================================
//  高性能网络编程调试实验 —— 充满 Bug 的服务器
//  配合 README.md 实验手册使用
// ============================================================

struct Connection {
    int fd;
    int bytes_processed = 0;

    Connection(int f) : fd(f) {
        std::cout << "[Connection fd=" << fd << "] Created\n";
    }
    ~Connection() {
        std::cout << "[Connection fd=" << fd << "] Destroyed\n";
    }

    void handleMessage() {
        bytes_processed += 1024;
        std::cout << "[Connection fd=" << fd << "] Processed: "
                  << bytes_processed << " bytes\n";
    }
};

// ============================================================
// 实验 1.1: Use-After-Free (工具: ASan)
// 主线程提前释放 Connection, 工作线程仍持有悬空指针
// ============================================================
void simulate_uaf() {
    std::cout << "\n=== 实验 1.1: Use-After-Free ===\n";
    Connection* conn = new Connection(5);

    std::thread worker([conn]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        conn->handleMessage();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "[Main] Deleting connection prematurely...\n";
    delete conn;

    worker.join();
    std::cout << "[Main] Worker joined. (UAF already happened)\n";
}

// ============================================================
// 实验 1.2: 内存泄漏 + FD 泄漏 (工具: ASan LeakSanitizer, lsof)
// 创建大量 socket 和 Connection 对象, 全部泄漏
// ============================================================
void simulate_leak() {
    std::cout << "\n=== 实验 1.2: Memory Leak + FD Leak ===\n";
    std::cout << "[Main] PID = " << getpid() << "\n";

    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); break; }
        Connection* c = new Connection(fd);
        c->handleMessage();
    }

    std::cout << "\n[Main] 100 connections created and leaked.\n";
    std::cout << "[Main] Run in another terminal:\n";
    std::cout << "  lsof -p " << getpid() << " | grep -c sock\n";
    std::cout << "Press Enter to exit (ASan leak report appears on exit)...\n";
    std::cin.get();
}

// ============================================================
// 实验 2.1: 数据竞争 (工具: TSan)
// 两个线程同时读写同一个 int, 无任何同步
// ============================================================
void simulate_race() {
    std::cout << "\n=== 实验 2.1: Data Race ===\n";
    Connection conn(5);

    std::thread t1([&conn]() {
        for (int i = 0; i < 100000; i++)
            conn.bytes_processed++;
    });
    std::thread t2([&conn]() {
        for (int i = 0; i < 100000; i++)
            conn.bytes_processed++;
    });

    t1.join();
    t2.join();
    std::cout << "[Main] bytes_processed = " << conn.bytes_processed
              << " (expected 200000)\n";
}

// ============================================================
// 实验 2.2: 死锁 (工具: TSan 检测锁序, GDB/LLDB 分析挂起)
// 经典 AB-BA 锁序反转
// ============================================================
std::mutex mtx_connMgr;
std::mutex mtx_logger;

void simulate_deadlock() {
    std::cout << "\n=== 实验 2.2: Deadlock (AB-BA) ===\n";
    std::cout << "[Main] PID = " << getpid() << "\n";
    std::cout << "[Main] If the program hangs, the deadlock has occurred.\n";
    std::cout << "[Main] Attach GDB/LLDB from another terminal to analyze.\n\n";

    std::thread t1([]() {
        std::lock_guard<std::mutex> lk1(mtx_connMgr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk2(mtx_logger);
        std::cout << "[T1] Got both locks.\n";
    });

    std::thread t2([]() {
        std::lock_guard<std::mutex> lk1(mtx_logger);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk2(mtx_connMgr);
        std::cout << "[T2] Got both locks.\n";
    });

    t1.join();
    t2.join();
    std::cout << "[Main] Both threads finished.\n";
}

// ============================================================
// 实验 3: 未定义行为 (工具: UBSan)
// 有符号整数溢出 + C 数组越界访问
// ============================================================
void simulate_ubsan() {
    std::cout << "\n=== 实验 3: Undefined Behavior ===\n";

    int packet_size = 0x7FFFFFFF;
    packet_size += 1;
    std::cout << "[UB] packet_size after overflow = " << packet_size << "\n";

    int buffer[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int idx = 4;
    std::cout << "[UB] buffer[4] = " << buffer[idx] << "\n";
}

// ============================================================
// 实验 4: 段错误崩溃分析 (工具: GDB/LLDB + core dump)
// 多层调用栈中的空指针解引用
// ============================================================
void parse_header(char* buf, int len) {
    std::memcpy(buf, "HTTP/1.1 200 OK\r\n", 17);
}

void process_request(char* buf, int len) {
    parse_header(buf, len);
}

void handle_client(int fd) {
    char* response_buf = nullptr;
    process_request(response_buf, 256);
    std::cout << "[Client fd=" << fd << "] Response sent\n";
}

void simulate_crash() {
    std::cout << "\n=== 实验 4: Segfault Crash ===\n";
    std::cout << "About to crash... analyze with GDB/LLDB.\n";
    handle_client(42);
}

// ============================================================
// 实验 5: CPU 性能热点 (工具: perf + FlameGraph)
// HTTP 解析热路径中的低效字符串操作 (按值传参导致大量拷贝)
// ============================================================
std::string slow_find_header(std::string request, std::string header_name) {
    size_t pos = request.find(header_name);
    if (pos == std::string::npos) return "";
    size_t end = request.find("\r\n", pos);
    return request.substr(pos, end - pos);
}

void simulate_cpu_hog() {
    std::cout << "\n=== 实验 5: CPU Hotspot ===\n";
    std::string http_request =
        "GET /api/v1/data HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: Mozilla/5.0\r\n"
        "Accept: text/html,application/json\r\n"
        "Accept-Encoding: gzip, deflate, br\r\n"
        "Connection: keep-alive\r\n"
        "X-Request-ID: abcdef-123456-ghijkl-789012\r\n"
        "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJ1c2VyIjoiYWRtaW4ifQ\r\n"
        "\r\n";

    auto start = std::chrono::steady_clock::now();
    int found = 0;

    for (int i = 0; i < 2000000; i++) {
        if (!slow_find_header(http_request, "Host").empty())
            found++;
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "[CPU] " << found << " matches in " << ms << " ms\n";
}

// ============================================================
// 实验 6 & 7: Echo Server (工具: strace, tcpdump, lsof, ss)
// 一个简单的 echo 服务器, 含 FD 泄漏: accept 后从不 close
// ============================================================
void simulate_network() {
    std::cout << "\n=== 实验 6 & 7: Network Echo Server ===\n";

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9527);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return;
    }
    if (listen(server_fd, 128) < 0) {
        perror("listen"); close(server_fd); return;
    }

    std::cout << "[Server] Listening on 0.0.0.0:9527 (PID: " << getpid() << ")\n\n";
    std::cout << "Observation commands (run in other terminals):\n";
    std::cout << "  lsof -i :9527\n";
    std::cout << "  ss -tlnp | grep 9527                 # Linux\n";
    std::cout << "  sudo tcpdump -i lo port 9527 -X      # Linux\n";
    std::cout << "  sudo tcpdump -i lo0 port 9527 -X     # macOS\n";
    std::cout << "  strace -e trace=network -p " << getpid() << "  # Linux\n";
    std::cout << "  echo 'Hello' | nc localhost 9527\n";
    std::cout << "Press Ctrl+C to stop.\n\n";

    while (true) {
        struct sockaddr_in cli{};
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(server_fd, (struct sockaddr*)&cli, &cli_len);
        if (cfd < 0) { perror("accept"); continue; }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        std::cout << "[Server] Client " << ip << ":" << ntohs(cli.sin_port)
                  << " connected (fd=" << cfd << ")\n";

        char buf[1024];
        ssize_t n = read(cfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << "[Server] Received: " << buf;
            write(cfd, buf, n);
        }

        // BUG: never close(cfd) -> FD leak
        std::cout << "[Server] fd=" << cfd << " NOT closed (fd leak!)\n\n";
    }
}

// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: ./buggy_server <mode>\n\n"
                  << "Available modes:\n"
                  << "  uaf       Exp 1.1  Use-After-Free        (ASan)\n"
                  << "  leak      Exp 1.2  Memory/FD Leak        (ASan + lsof)\n"
                  << "  race      Exp 2.1  Data Race             (TSan)\n"
                  << "  deadlock  Exp 2.2  Deadlock              (TSan + GDB)\n"
                  << "  ubsan     Exp 3    Undefined Behavior    (UBSan)\n"
                  << "  crash     Exp 4    Segfault              (GDB/LLDB)\n"
                  << "  cpu       Exp 5    CPU Hotspot           (perf)\n"
                  << "  network   Exp 6&7  Echo Server           (strace+tcpdump+lsof)\n";
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
