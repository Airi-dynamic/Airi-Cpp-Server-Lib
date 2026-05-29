/**
 * conn_scale_test.cpp — 海量长连接规模测试
 *
 * 功能：
 *   - 建立大量 TCP 长连接到服务器，保持空闲
 *   - 测量服务器进程 RSS（驻留内存）增长
 *   - 在所有连接建立后发送 HTTP 请求验证服务器仍可正常响应
 *
 * 用法：
 *   ./conn_scale_test [host] [port] [connections] [server_pid]
 *   ./conn_scale_test 127.0.0.1 9090 10000 12345
 */

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static long getRssKB(pid_t pid) {
#ifdef __APPLE__
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ps -o rss= -p %d", pid);
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;
    long rss = 0;
    if (fscanf(fp, "%ld", &rss) != 1)
        rss = -1;
    pclose(fp);
    return rss;
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    long rss = -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(fp);
    return rss;
#endif
}

static int makeConnection(const std::string &host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    int port = 9090;
    int targetConns = 10000;
    pid_t serverPid = 0;

    if (argc > 1)
        host = argv[1];
    if (argc > 2)
        port = std::stoi(argv[2]);
    if (argc > 3)
        targetConns = std::stoi(argv[3]);
    if (argc > 4)
        serverPid = std::stoi(argv[4]);

    std::cout << "=== Connection Scale Test ===" << std::endl;
    std::cout << "  Target : " << host << ":" << port << std::endl;
    std::cout << "  Connections: " << targetConns << std::endl;
    if (serverPid > 0)
        std::cout << "  Server PID : " << serverPid << std::endl;
    std::cout << std::endl;

    // 测量基线 RSS
    long rssBaseline = serverPid > 0 ? getRssKB(serverPid) : -1;
    if (rssBaseline > 0)
        std::cout << "  Baseline RSS: " << rssBaseline << " KB (" << rssBaseline / 1024.0 << " MB)"
                  << std::endl;

    // 建立连接
    std::vector<int> fds;
    fds.reserve(targetConns);
    auto t0 = std::chrono::steady_clock::now();

    int failCount = 0;
    for (int i = 0; i < targetConns; ++i) {
        int fd = makeConnection(host, port);
        if (fd < 0) {
            ++failCount;
            if (failCount > 100) {
                std::cout << "  [STOP] Too many failures at connection " << i << ", stopping."
                          << std::endl;
                break;
            }
            continue;
        }
        fds.push_back(fd);

        // 每 1000 个连接打印进度
        if ((fds.size() % 1000) == 0) {
            long rssNow = serverPid > 0 ? getRssKB(serverPid) : -1;
            std::cout << "  " << fds.size() << " connections established";
            if (rssNow > 0)
                std::cout << " | RSS: " << rssNow << " KB (" << rssNow / 1024.0 << " MB)";
            std::cout << std::endl;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << std::endl;
    std::cout << "  Total established: " << fds.size() << " / " << targetConns << std::endl;
    std::cout << "  Failed: " << failCount << std::endl;
    std::cout << "  Time: " << elapsedMs << " ms" << std::endl;

    // 测量峰值 RSS
    long rssPeak = serverPid > 0 ? getRssKB(serverPid) : -1;
    if (rssPeak > 0 && rssBaseline > 0) {
        long delta = rssPeak - rssBaseline;
        double perConn = fds.size() > 0 ? static_cast<double>(delta * 1024) / fds.size() : 0;
        std::cout << std::endl;
        std::cout << "  === Memory Analysis ===" << std::endl;
        std::cout << "  Baseline RSS : " << rssBaseline / 1024.0 << " MB" << std::endl;
        std::cout << "  Peak RSS     : " << rssPeak / 1024.0 << " MB" << std::endl;
        std::cout << "  Delta        : " << delta / 1024.0 << " MB" << std::endl;
        std::cout << "  Per-connection: " << perConn << " bytes (" << perConn / 1024.0 << " KB)"
                  << std::endl;
    }

    // 验证服务器仍可响应
    std::cout << std::endl << "  === Liveness Check ===" << std::endl;
    int testFd = makeConnection(host, port);
    if (testFd >= 0) {
        std::string req = "GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        write(testFd, req.data(), req.size());
        char buf[512];
        int n = read(testFd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            // 检查是否有 HTTP 200
            if (strstr(buf, "200"))
                std::cout << "  Server responsive: HTTP 200 OK ✓" << std::endl;
            else
                std::cout << "  Server responded but not 200" << std::endl;
        } else {
            std::cout << "  Server unresponsive" << std::endl;
        }
        close(testFd);
    } else {
        std::cout << "  Cannot connect (server may have hit connection limit)" << std::endl;
    }

    // 保持连接 2 秒让工具测量 CPU
    std::cout << std::endl << "  Holding connections for 2 seconds..." << std::endl;
    sleep(2);

    // 关闭所有连接
    std::cout << "  Closing " << fds.size() << " connections..." << std::endl;
    for (int fd : fds)
        close(fd);

    // 等待服务器回收
    sleep(1);
    long rssAfter = serverPid > 0 ? getRssKB(serverPid) : -1;
    if (rssAfter > 0)
        std::cout << "  RSS after cleanup: " << rssAfter / 1024.0 << " MB" << std::endl;

    std::cout << std::endl << "=== Done ===" << std::endl;
    return 0;
}
