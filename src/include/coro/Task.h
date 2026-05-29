#pragma once
//
// coro/Task.h —— C++20 协程基础设施（为本项目量身定制的最小化实现）
//
// 提供两个工具：
//
//   1. FireAndForget — "点火即忘"协程返回类型。
//      - initial_suspend = suspend_never → 被调用时立刻开始执行，不挂起。
//      - final_suspend   = suspend_never → 到达 co_return 后自动销毁协程帧。
//      - 调用方无需 co_await，也无需保存返回值；适合 selectVote / sendHeartbeat
//        这类「发射后不管」的异步工作单元。
//
//   2. ResumeOnLoop<Loop> — 把协程恢复切换到指定 EventLoop 线程的 awaitable。
//      用法：
//          co_await ResumeOnLoop<Eventloop>{&loop_};
//          // 此后代码运行在 loop_ 线程
//      await_suspend 将 h.resume() 以 lambda 的形式投入 loop_->queueInLoop，
//      保证协程在 loop_ 线程恢复，维持 single-thread invariant。
//
// ── 与 RpcCallAwaiter 的关系 ────────────────────────────────────────────────
// RpcCallAwaiter（用于 co_await RPC 调用）定义在 rpc/AsyncRpcClient.h，
// 放在那里是为了避免循环包含（它调用 AsyncRpcClient::callAsync）。
//
// ── 线程安全说明 ─────────────────────────────────────────────────────────────
// 所有 FireAndForget 协程均假定从 loop_ 线程调用（callAsync 的回调约定）。
// 协程内无共享状态写操作，恢复点全在 loop_ 线程，天然无锁。
//
#include <coroutine>
#include <exception>

// ─── FireAndForget ────────────────────────────────────────────────────────────

struct FireAndForget {
    struct promise_type {
        // 被调用时立刻开始运行协程
        FireAndForget get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        // 完成后自动销毁帧，不需要 co_await 等待
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        // 未捕获异常终止进程而不是静默吞掉
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

// ─── ResumeOnLoop<Loop> ───────────────────────────────────────────────────────
//
// 泛型设计避免 Task.h 直接依赖 EventLoop.h，减少包含层次。
// Loop 需要提供 queueInLoop(std::function<void()>) 成员函数。
//
template<typename Loop>
struct ResumeOnLoop {
    Loop *loop;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) const {
        // 将 resume 投入目标 loop 的 pending functors 队列；
        // 调用方线程立刻得到控制权，协程在 loop 线程被唤醒。
        loop->queueInLoop([h]() mutable { h.resume(); });
    }

    void await_resume() const noexcept {}
};
