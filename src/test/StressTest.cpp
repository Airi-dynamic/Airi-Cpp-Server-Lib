#include "Buffer.h"
#include "InetAddress.h"
#include "Socket.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#else
#error "Unsupported platform: only macOS (kqueue) and Linux (epoll) are supported."
#endif

using namespace std;

// 统计变量
int success_count = 0;     // 成功完成所有消息的客户端数
int failed_msgs = 0;       // 消息内容校验失败数
int interrupted_count = 0; // 因错误/服务器断开而中断的客户端数
int total_msgs_sent = 0;   // 实际发送成功的消息总数

// 每一个客户端连接的状态机上下文
struct ClientContext {
    int id;
    Socket *sock;
    InetAddress *addr;
    Buffer *readBuffer;

    int target_msgs;        // 总共需要发送的消息数
    int current_msg_idx;    // 当前正在处理第几条消息
    string current_msg_str; // 当前正在收发的消息（WRITE 时设置，READ 时校验）

    bool connected; // 是否已经成功建立连接
};

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 释放客户端资源。
// 不能在此处显式调用 close(sock->getFd())，Socket::~Socket() 会负责关闭，
// 重复 close 会导致 double-close，可能误关一个恰好复用了相同 fd 的新连接。
void cleanupClient(ClientContext *ctx) {
    delete ctx->sock; // Socket 析构函数内调用 close(fd_)
    delete ctx->addr;
    delete ctx->readBuffer;
    delete ctx;
}

int main(int argc, char *argv[]) {
    int clients_num = 100;
    int msgs_num = 10000;
    int wait_seconds = 0;

    if (argc > 1)
        clients_num = atoi(argv[1]);
    if (argc > 2)
        msgs_num = atoi(argv[2]);
    if (argc > 3)
        wait_seconds = atoi(argv[3]);

    // 忽略 SIGPIPE：当服务器关闭连接后客户端仍尝试写入时，
    // 默认行为是终止进程，这会干扰测试结果统计
    signal(SIGPIPE, SIG_IGN);

    // ── 创建 IO 复用实例 ────────────────────────────────────────────────────
#ifdef __APPLE__
    int poller_fd = kqueue();
    if (poller_fd == -1) {
        perror("kqueue");
        return -1;
    }

    // 注册/切换兴趣的辅助 lambda（kqueue 版）

    // 初始注册：监听 WRITE（等待 connect 完成，非阻塞 connect 成功后 socket 变可写）
    auto pollRegisterWrite = [&](int fd, ClientContext *ctx) {
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, ctx);
        kevent(poller_fd, &ev, 1, nullptr, 0, nullptr);
    };
    // 发送完毕，切换到等待服务器回显：停写、开读
    auto pollWaitRead = [&](int fd, ClientContext *ctx) {
        struct kevent evs[2];
        EV_SET(&evs[0], fd, EVFILT_WRITE, EV_DISABLE, 0, 0, ctx);
        // EV_ADD：若 EVFILT_READ 尚不存在则添加，已存在则更新（幂等）
        EV_SET(&evs[1], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, ctx);
        kevent(poller_fd, evs, 2, nullptr, 0, nullptr);
    };
    // 回显收齐，准备发下一条：停读、开写
    auto pollWaitWrite = [&](int fd, ClientContext *ctx) {
        struct kevent evs[2];
        EV_SET(&evs[0], fd, EVFILT_READ, EV_DISABLE, 0, 0, ctx);
        EV_SET(&evs[1], fd, EVFILT_WRITE, EV_ENABLE, 0, 0, ctx);
        kevent(poller_fd, evs, 2, nullptr, 0, nullptr);
    };

#elif defined(__linux__)
    int poller_fd = epoll_create1(0);
    if (poller_fd == -1) {
        perror("epoll_create1");
        return -1;
    }

    // 注册/切换兴趣的辅助 lambda（epoll 版）
    // epoll 没有 EV_DISABLE，通过 EPOLL_CTL_MOD 调整 events 掩码来实现单向监听
    auto pollRegisterWrite = [&](int fd, ClientContext *ctx) {
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.ptr = ctx;
        epoll_ctl(poller_fd, EPOLL_CTL_ADD, fd, &ev);
    };
    auto pollWaitRead = [&](int fd, ClientContext *ctx) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = ctx;
        epoll_ctl(poller_fd, EPOLL_CTL_MOD, fd, &ev);
    };
    auto pollWaitWrite = [&](int fd, ClientContext *ctx) {
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.ptr = ctx;
        epoll_ctl(poller_fd, EPOLL_CTL_MOD, fd, &ev);
    };
#endif

    // ── 批量发起非阻塞连接 ──────────────────────────────────────────────────
    int active_clients = 0;
    cout << "Initializing " << clients_num << " concurrent connections..." << endl;
    cout << "Each client will send " << msgs_num << " messages." << endl;
    cout << "Total expected messages: " << (clients_num * msgs_num) << endl << endl;

    auto start_time = chrono::steady_clock::now();

    for (int i = 0; i < clients_num; ++i) {
        ClientContext *ctx = new ClientContext();
        ctx->id = i;
        ctx->sock = new Socket();
        ctx->addr = new InetAddress("127.0.0.1", 8888);
        ctx->readBuffer = new Buffer();
        ctx->target_msgs = msgs_num;
        ctx->current_msg_idx = 0;
        ctx->connected = false;

        int fd = ctx->sock->getFd();
        setNonBlocking(fd);

        // 发起非阻塞连接（通常立即返回 -1 / EINPROGRESS）
        connect(fd, reinterpret_cast<sockaddr *>(&ctx->addr->addr), ctx->addr->addr_len);

        // 监听 WRITE 事件：连接建立完成时 socket 会变为可写
        pollRegisterWrite(fd, ctx);
        active_clients++;

        if (wait_seconds > 0)
            sleep(wait_seconds);
    }

    cout << "All connections initiated. Starting event loop..." << endl;

    // ── 核心事件循环 ────────────────────────────────────────────────────────
    // 两个平台的事件处理逻辑完全相同，只有"从事件中取 ctx 和判断事件类型"的写法不同。
    // 用统一的 bool 标志变量屏蔽平台差异，循环体只写一份。

#ifdef __APPLE__
    struct kevent events[1024];
#elif defined(__linux__)
    struct epoll_event events[1024];
#endif

    while (active_clients > 0) {

#ifdef __APPLE__
        int nfds = kevent(poller_fd, nullptr, 0, events, 1024, nullptr);
#elif defined(__linux__)
        int nfds = epoll_wait(poller_fd, events, 1024, -1);
#endif
        if (nfds == -1) {
            if (errno == EINTR)
                continue; // 信号打断，重试
            perror("poll wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {

            // 从平台特定的事件结构中提取 ctx 和事件类型标志
#ifdef __APPLE__
            ClientContext *ctx = static_cast<ClientContext *>(events[i].udata);
            bool isError = (events[i].flags & EV_ERROR) != 0;
            bool isEOF = (events[i].flags & EV_EOF) != 0;
            bool isWrite = (events[i].filter == EVFILT_WRITE);
            bool isRead = (events[i].filter == EVFILT_READ);
#elif defined(__linux__)
            ClientContext *ctx = static_cast<ClientContext *>(events[i].data.ptr);
            uint32_t ev = events[i].events;
            bool isError = (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
            bool isEOF = false; // Linux 通过 read()==0 或 EPOLLHUP 检测对端关闭
            bool isWrite = (ev & EPOLLOUT) != 0;
            bool isRead = (ev & EPOLLIN) != 0;
#endif
            int fd = ctx->sock->getFd();

            // ── 异常 / 对端关闭 ──
            if (isError || isEOF) {
                cerr << "[Client " << ctx->id << "] Error or server closed connection (sent "
                     << ctx->current_msg_idx << "/" << ctx->target_msgs << " msgs)" << endl;
                interrupted_count++;
                cleanupClient(ctx);
                active_clients--;
                continue;
            }

            // ── WRITE 事件：连接刚建立 或 可以发送下一条消息 ──
            if (isWrite) {
                if (!ctx->connected) {
                    // 非阻塞 connect 完成后，通过 SO_ERROR 确认是否真的成功
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0) {
                        cerr << "[Client " << ctx->id << "] Connect failed: " << strerror(err)
                             << endl;
                        interrupted_count++;
                        cleanupClient(ctx);
                        active_clients--;
                        continue;
                    }
                    ctx->connected = true;
                }

                // 构造并发送当前消息
                ctx->current_msg_str =
                    "Client " + to_string(ctx->id) + " msg " + to_string(ctx->current_msg_idx);
                ssize_t nw = write(fd, ctx->current_msg_str.c_str(), ctx->current_msg_str.size());
                if (nw > 0) {
                    // 发送成功，等待服务器回显
                    total_msgs_sent++;
                    pollWaitRead(fd, ctx);
                } else if (nw == -1 && errno != EAGAIN && errno != EINTR) {
                    cerr << "[Client " << ctx->id << "] Write error: " << strerror(errno)
                         << " (sent " << ctx->current_msg_idx << "/" << ctx->target_msgs << " msgs)"
                         << endl;
                    interrupted_count++;
                    cleanupClient(ctx);
                    active_clients--;
                }
                // nw == -1 && EAGAIN：发送缓冲区暂满，保持 WRITE 监听，等下次通知重试
            }

            // ── READ 事件：收到服务器回显 ──
            else if (isRead) {
                char buf[4096];
                ssize_t nr = read(fd, buf, sizeof(buf));

                if (nr > 0) {
                    ctx->readBuffer->append(buf, nr);

                    // 以 readBuffer 里实际堆积的字节数判断回显是否收齐，
                    // 而非用单独的计数器，避免多次 read 累加不准确的问题
                    int target_len = static_cast<int>(ctx->current_msg_str.size());
                    if (static_cast<int>(ctx->readBuffer->readableBytes()) >= target_len) {
                        string received = ctx->readBuffer->retrieveAsString(target_len);
                        if (received != ctx->current_msg_str) {
                            failed_msgs++;
                            cerr << "[Client " << ctx->id << "] Message " << ctx->current_msg_idx
                                 << " verify FAILED! Expected: '" << ctx->current_msg_str
                                 << "', Got: '" << received << "'" << endl;
                        }

                        ctx->current_msg_idx++;
                        if (ctx->current_msg_idx < ctx->target_msgs) {
                            // 还有消息要发，切换回写模式
                            pollWaitWrite(fd, ctx);
                        } else {
                            // 所有消息处理完毕，关闭此客户端
                            success_count++;
                            cleanupClient(ctx);
                            active_clients--;
                        }
                    }
                    // 未收齐，继续等待下次 READ 事件，数据继续累积在 readBuffer 中
                } else if (nr == 0) {
                    cerr << "[Client " << ctx->id << "] Server disconnected unexpectedly (sent "
                         << ctx->current_msg_idx << "/" << ctx->target_msgs << " msgs)" << endl;
                    interrupted_count++;
                    cleanupClient(ctx);
                    active_clients--;
                } else if (errno != EAGAIN && errno != EINTR) {
                    cerr << "[Client " << ctx->id << "] Read error: " << strerror(errno)
                         << " (sent " << ctx->current_msg_idx << "/" << ctx->target_msgs << " msgs)"
                         << endl;
                    interrupted_count++;
                    cleanupClient(ctx);
                    active_clients--;
                }
                // errno == EAGAIN/EINTR：数据尚未到达，继续等待 READ 事件
            }
        }
    }

    close(poller_fd);

    auto end_time = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - start_time);
    double seconds = duration.count() / 1000.0;

    // ========== 测试结果报告 ==========
    cout << "\n" << string(60, '=') << endl;
    cout << "           STRESS TEST RESULTS" << endl;
    cout << string(60, '=') << endl << endl;

    cout << "Test Configuration:" << endl;
    cout << "  Concurrent Clients:     " << clients_num << endl;
    cout << "  Messages per Client:    " << msgs_num << endl;
    cout << "  Total Expected Messages: " << (clients_num * msgs_num) << endl << endl;

    cout << "Client Statistics:" << endl;
    cout << "  Successfully Completed: " << success_count << " / " << clients_num;
    if (success_count == clients_num) {
        cout << " ✓";
    }
    cout << endl;
    cout << "  Interrupted/Failed:     " << interrupted_count << " / " << clients_num << endl;
    cout << "  Total:                  " << (success_count + interrupted_count) << endl << endl;

    cout << "Message Statistics:" << endl;
    cout << "  Total Sent:             " << total_msgs_sent << " / " << (clients_num * msgs_num)
         << endl;
    cout << "  Content Verify Failed:  " << failed_msgs << endl;
    int successful_msgs = total_msgs_sent - failed_msgs;
    cout << "  Successfully Verified:  " << successful_msgs << endl << endl;

    cout << "Performance:" << endl;
    cout << "  Total Time:             " << fixed << setprecision(2) << seconds << " seconds"
         << endl;
    if (seconds > 0) {
        cout << "  Throughput:             " << fixed << setprecision(0)
             << (total_msgs_sent / seconds) << " msgs/sec" << endl;
        cout << "  Avg Latency per Msg:    " << fixed << setprecision(3)
             << (seconds * 1000.0 / total_msgs_sent) << " ms" << endl;
    }
    cout << endl;

    cout << "Test Result: ";
    if (success_count == clients_num && failed_msgs == 0) {
        cout << "[PASSED] (All clients completed, all messages verified)" << endl;
    } else if (interrupted_count > 0) {
        cout << "[FAILED] (" << interrupted_count << " clients interrupted)" << endl;
    } else if (failed_msgs > 0) {
        cout << "[FAILED] (" << failed_msgs << " messages verification failed)" << endl;
    } else {
        cout << "[FAILED] (Unknown reason)" << endl;
    }

    cout << string(60, '=') << endl;

    return (success_count == clients_num && failed_msgs == 0) ? 0 : 1;
}
