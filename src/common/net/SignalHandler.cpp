// SignalHandler.cpp —— self-pipe trick 实现
//
// 详见头文件设计说明。
//
#include "net/SignalHandler.h"

#include "log/Logger.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

constexpr int kInvalidFd = -1;

int                                            g_pipeFd[2]    = {kInvalidFd, kInvalidFd};
std::once_flag                                 g_initFlag;
std::mutex                                     g_handlersMu;
std::map<int, std::function<void()>>           g_handlers; // dispatcher 线程在 g_handlersMu 保护下读
std::thread                                    g_dispatcher;
std::atomic<bool>                              g_running{false};

// 真正的 OS 信号处理函数。只做 async-signal-safe 的事：
//   POSIX 明确保证 write() 在信号上下文是 async-signal-safe 的。
// 我们写入一个字节（信号编号低 8 位）即可。
extern "C" void mcpp_signal_writer(int sig) {
    if (g_pipeFd[1] == kInvalidFd) return;
    uint8_t s = static_cast<uint8_t>(sig);
    // EINTR 时重试；EAGAIN 直接丢弃（pipe 满说明前面积压未处理，丢一个无关紧要）
    ssize_t n;
    do {
        n = ::write(g_pipeFd[1], &s, 1);
    } while (n == -1 && errno == EINTR);
    // 不能在这里 log / printf；保持 async-signal-safe 纯净
}

void dispatcherLoop() {
    while (g_running.load(std::memory_order_acquire)) {
        uint8_t s = 0;
        ssize_t n = ::read(g_pipeFd[0], &s, 1);
        if (n == 0) {
            // pipe 写端关闭（进程退出场景）
            return;
        }
        if (n == -1) {
            if (errno == EINTR) continue;
            LOG_ERROR << "[SignalHandler] dispatcher read 失败 errno=" << errno;
            return;
        }

        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lk(g_handlersMu);
            auto it = g_handlers.find(static_cast<int>(s));
            if (it != g_handlers.end()) cb = it->second;
        }
        if (cb) {
            try {
                cb();
            } catch (const std::exception &e) {
                LOG_ERROR << "[SignalHandler] handler 异常 sig=" << static_cast<int>(s)
                          << " what=" << e.what();
            } catch (...) {
                LOG_ERROR << "[SignalHandler] handler 未知异常 sig=" << static_cast<int>(s);
            }
        }
        // 若未注册：静默丢弃（避免旧版 bad_function_call 崩溃）
    }
}

void initOnce() {
    std::call_once(g_initFlag, [] {
        if (::pipe(g_pipeFd) == -1) {
            LOG_ERROR << "[SignalHandler] pipe 创建失败 errno=" << errno;
            return;
        }
        // 写端 O_NONBLOCK：信号处理函数不能阻塞
        int wflags = ::fcntl(g_pipeFd[1], F_GETFL, 0);
        ::fcntl(g_pipeFd[1], F_SETFL, wflags | O_NONBLOCK);
        // 读端关闭 O_NONBLOCK：dispatcher 线程阻塞 read 即可省 CPU
        // FD_CLOEXEC：fork+exec 时不泄露
        ::fcntl(g_pipeFd[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(g_pipeFd[1], F_SETFD, FD_CLOEXEC);

        g_running.store(true, std::memory_order_release);
        g_dispatcher = std::thread(dispatcherLoop);
        // 故意 detach：进程退出时随着 main 一同被回收，避免静态析构序问题。
        g_dispatcher.detach();
    });
}

} // namespace

void Signal::signal(int sig, std::function<void()> handler) {
    initOnce();

    {
        std::lock_guard<std::mutex> lk(g_handlersMu);
        g_handlers[sig] = std::move(handler);
    }

    // 安装 OS-level 信号处理函数。使用 sigaction 以获得 SA_RESTART 行为，
    // 避免信号中断 IO 调用导致 EINTR 频发。
    struct sigaction sa{};
    sa.sa_handler = &mcpp_signal_writer;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (::sigaction(sig, &sa, nullptr) == -1) {
        LOG_ERROR << "[SignalHandler] sigaction 安装失败 sig=" << sig << " errno=" << errno;
    }
}
