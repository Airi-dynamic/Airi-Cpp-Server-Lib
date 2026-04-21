#include "Connection.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include "SignalHandler.h"
#include "TcpServer.h"
#include "timer/TimeStamp.h"
#include <atomic>
#include <iostream>
#include <thread>

int main() {
    TcpServer server;

    Signal::signal(SIGINT, [&] {
        // 用 atomic_flag 保证信号处理函数幂等：
        // 多线程进程中 SIGINT 可能被任意线程接收，防止重入导致二次析构
        static std::atomic_flag fired = ATOMIC_FLAG_INIT;
        if (fired.test_and_set())
            return;
        std::cout << "[server] Caught SIGINT, shutting down." << std::endl;
        // 只调 stop()，令所有 Reactor 退出循环，Start() 随后返回
        // 不在信号处理函数里调 delete / exit，避免异步信号安全问题
        server.stop();
    });

    server.newConnect([](Connection *conn) {
        std::cout << "[server] New client connected, fd=" << conn->getSocket()->getFd() << " at "
                  << TimeStamp::now().toString() << std::endl;

        // Phase 2 演示：为每条新连接在其归属的 sub-reactor 上添加一个空闲超时定时器。
        // 若 60 秒内连接没有被关闭（业务层未调用 conn->close()），则主动断开。
        // 注意：因为 Connection 目前用 unique_ptr 管理（不是 shared_ptr），
        // 这里直接持有裸指针 conn 存在生命周期风险——如果连接已在定时器触发前被
        // 正常关闭删除，回调执行时 conn 就是野指针。
        // 完整的安全实现需要 Phase 4 将 Connection 改为 shared_ptr + weak_ptr 模式。
        // 以下演示仅用于说明 runAfter 的调用方式，生产代码应配合 weak_ptr 使用。
        //
        // conn->getLoop()->runAfter(60.0, [conn]() {
        //     if (conn->getState() == Connection::State::kConnected)
        //         conn->close();
        // });
    });

    server.onMessage([](Connection *conn) {
        if (conn->getState() != Connection::State::kConnected)
            return;
        std::string msg = conn->getInputBuffer()->retrieveAllAsString();
        if (!msg.empty()) {
            std::cout << "[Thread " << std::this_thread::get_id() << "] recv: " << msg << std::endl;
            conn->send(msg);
        }
    });

    server.Start(); // 阻塞，直到 stop() 被调用

    // Start() 返回后 server 在此析构（栈对象，RAII 自动清理）
    return 0;
}