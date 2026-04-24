# Day 32: C++20 协程 I/O——把异步代码写得像同步代码

> **⚠️ 实验分支说明**　本日的内容是在 day30 生产发布之后的技术实验，未合入主代码树。
>
> 主仓库的可编译代码停留在 **day30** 状态；day31–day36 的可运行快照仅作为参考完整保留在 `HISTORY/day31` … `HISTORY/day36` 中。
>
> 这篇日志的所有代码路径与命令，默认均应补上 `HISTORY/day3X/` 前缀后阅读 / 运行。


> **今日目标**：从零搭起一个 header-only、可选启用、与现有 EventLoop 松耦合的协程层。最终业务能用 `co_await` 替代多层嵌套的 `onRead/onWrite` 回调，让异步代码读起来像同步代码。
> **基于**：Day 31（WebSocket 协议层）。**进入**：C++20 协程探索期。

---

## 0. 今日构建目标

C++20 协程不是「打开开关就能用」的特性——它要求你同时实现 `promise_type`、`coroutine_handle`、`Awaitable` 三个相互咬合的抽象，少一个角，编译器就会给你 50 行模板报错。今天将按以下顺序逐步搭起每一块，每一步都可以独立编译并写一个 GTest 用例验证。

**构建清单（按顺序）：**

1. **§2** — 画出协程层的模块全景与所有权树（先想清楚再下笔）
2. **§3** — 厘清协程的初始化顺序（编译器替你做了什么）
3. **§4** — 走通三种典型调用链（纯计算 / 异步回调 / 异常传播）
4. **§5** — 写 `TaskPromise` 状态容器：用 `std::variant` 承载未完成 / 有值 / 有异常三态
5. **§6** — 写 `final_suspend` 与 `FinalAwaiter`：用 symmetric transfer 把控制权零开销交还父协程
6. **§7** — 写 `Task::operator co_await`：把父协程登记到子 promise 的 `continuation_`
7. **§8** — 想清楚 `Task<T>` 与 `AsyncAwaitable` 的析构与生命周期边界
8. **§9** — 用 19 个 GTest 用例钉死正常、异常、嵌套、移动、批量五条路径

**说明**：代码块前的「来自 `HISTORY/day32/...`」标注意为「将以下代码写入该文件的对应位置」，跟着每步动手输入即可。

---

## 1. 今天要解决的几个问题

### 1.1 痛点：回调式异步在多步业务里彻底失控

day1-31 我们一直靠**回调**来串联异步 IO：`Connection::send()` 写不完时注册 `EPOLLOUT`，写完成回调里继续推进业务……一切都依赖 `EventLoop` 把 fd 事件分派到 `Channel`，再分派到 user-defined 函数对象。这种风格在 day14 实现 HTTP 解析时就已经显出疲态：

```cpp
// 典型的回调地狱——day14 当时如果要做"先读 header，再按 Content-Length 读 body，再写响应"
conn->onRead([conn](Buffer& buf) {
    parseHeader(buf, [conn](HttpHeader h) {
        readBody(conn, h.contentLength, [conn](std::string body) {
            doBusiness(body, [conn](std::string resp) {
                conn->send(resp, [conn]() {
                    LOG_INFO << "done";
                });
            });
        });
    });
});
```

当业务流变成"读→读→读→写→读……"的多步异步链时，回调嵌套 + 状态在 `shared_ptr` 闭包之间到处传递，调试堆栈断成几十段，线性阅读完全不可能。

### 1.2 根因：异步链路的「状态」散落在闭包之间

上面那段代码的本质问题不是「嵌套深」，而是**业务的状态机被打碎在 N 个 lambda 闭包里**。每一层 lambda 都通过 `[conn]` 捕获共享指针，把局部变量（解析出的 header、读到的 body）藏进闭包栈。要排查一个 bug，得先在脑子里把这 5 层闭包重新拼回一个状态机——这个心智负担是回调风格不可消除的硬开销。

更严重的是：

- **异常无法跨回调传播**：每层都得手写 `try/catch` + 把异常对象传给下一个回调。
- **栈追溯断成几十段**：调试器的 backtrace 从 IO 线程的 `epoll_wait` 开始，看不到「业务上一步在做什么」。
- **资源生命周期靠 shared_ptr**：每层闭包都要 `[self = shared_from_this()]`，代码里到处是延寿的环。

协程把这些状态**塞回函数局部变量**——本来就该在的地方。

### 1.3 解法：用编译器生成的状态机替代手写回调

C++20 协程（Coroutine）首次让我们能用**编译器生成的状态机**替代手写回调，关键是：

- **同步式书写**：`co_await` / `co_return` 之间的代码读起来像顺序执行
- **零开销抽象**：promise/handle/awaitable 全部在编译期生成，运行时只有一次堆分配协程帧
- **可组合**：`Task<T>` 可以作为另一个协程的 `co_await` 操作数，自然形成 DAG
- **异常透传**：协程内 throw 自动传播到调用端的 `co_await` 表达式

day31 完成 WebSocket 后，下一步显然是协程化的异步 IO，让 day37+ 的 HTTP/2 与 RPC 框架能直接写线性逻辑。

### 1.4 业界方案对比

| 方案 | 类型 | 关键 API | 调度集成 | 取舍 |
|------|------|---------|---------|------|
| **Boost.Asio (1.80+)** | `awaitable<T>` | `co_spawn` / `use_awaitable` | 与 `io_context` 深度耦合 | 生态最成熟，但与已有 EventLoop 不兼容 |
| **folly::coro** | `Task<T>` / `AsyncGenerator<T>` | `coro::blockingWait` | folly executor | Facebook 生产验证，依赖庞大 |
| **cppcoro** | `task<T>` | `sync_wait` / `when_all` | 自带 thread_pool | 参考实现，作者已停更 |
| **stdexec (P2300)** | sender/receiver | 不基于协程的延续模型 | 自带 scheduler | 标准化提案，未稳定 |

我们选择**从 `Task<T>` + `Awaitable` 起步**，绑定方式选择 **PMR/无依赖**，这样既贴近 cppcoro 教科书实现、便于未来阅读 folly 源码迁移，又不引入任何第三方依赖。

### 1.5 今日方案概述

实现一套**header-only、可选启用、与现有 EventLoop 松耦合**的协程层：

1. **特性检测**：通过 `__cpp_impl_coroutine` 宏决定是否暴露 `MCPP_HAS_COROUTINES=1`，防止用 C++17 编译时报错
2. **`Task<T>`**：懒执行（lazy）—— 协程构造后不立即运行，由外层 `co_await` 或 `syncWait()` 启动；带 `void` 偏特化
3. **三类 Awaitable**：`ReadyAwaitable<T>`（立即就绪）、`AsyncAwaitable<T>`（回调驱动）、`SleepAwaitable`（基于定时器）
4. **`Scheduler` 抽象**：纯接口（`schedule(handle)` / `scheduleAfter(delay, handle)`），EventLoopScheduler 是其唯一实现
5. **不做的事**：本日不实现协程化的 socket read/write（留给 day33 io_uring）；不实现协程取消（cancellation token）；不实现 `when_all` / `when_any` 组合子

### 1.6 今日文件变更全图

```
src/include/async/
├── Coroutine.h           [新增 454L]   Task<T> / TaskPromise / Awaitable 三件套 / Scheduler 接口
└── AsyncIO.h             [新增 221L]   EventLoopScheduler / SleepAwaitable / IO awaitables 占位

src/common/async/
└── Coroutine.cpp         [新增 160L]   macOS accept4_compat 兜底 + 调度器具体方法

src/test/
└── CoroutineTest.cpp     [新增 301L]   19 个 GTest 用例

CMakeLists.txt            [修改 +12]    MCPP_ENABLE_COROUTINES 选项 + 条件编译
```

下一节起，我们正式按建造顺序动手。

---

## 2. 第 1 步 — 画出协程层模块全景与所有权树

```
mcpp::coro 命名空间
├── Task<T>                                       ← 协程返回类型（值类型，独占 handle）
│    ├── promise_type = TaskPromise<T>            ← 编译器要求的关联类型
│    └── std::coroutine_handle<TaskPromise<T>> handle_   ← 唯一持有协程帧
│
├── TaskPromise<T>（detail 命名空间）
│    ├── std::optional<T> value_                   ← return_value 写入处
│    ├── std::exception_ptr exception_             ← unhandled_exception 写入处
│    └── std::coroutine_handle<> continuation_     ← 父协程，FinalAwaitable 唤醒时用
│       └── 观察者：当前协程是叶子节点时为空
│
├── Awaitable 三件套
│    ├── ReadyAwaitable<T>     { T value_; }       ← 直接在 await_resume 返回，不挂起
│    ├── AsyncAwaitable<T>     { Initiator init_; std::optional<T> result_; }
│    │    └── await_suspend 中调用 init_(callback)，回调 resume(handle)
│    └── SleepAwaitable        { std::chrono::milliseconds dur_; Scheduler* sched_; }
│         └── await_suspend 中调用 sched_->scheduleAfter(dur_, handle)
│
├── Scheduler（抽象）                              ← 解耦协程与 EventLoop
│    └── EventLoopScheduler : Scheduler           ← AsyncIO.h 提供，将 handle.resume() 投递到 loop
│         └── 持有 EventLoop*（裸观察者，loop 生命周期早于 scheduler）
│
└── 平台兼容层（仅 macOS）
     └── accept4_compat(fd, addr, addrlen, flags) ← Linux accept4 不存在的兜底，
          内部 = accept + fcntl(O_NONBLOCK) + fcntl(FD_CLOEXEC)
```

**所有权规则**：

- `Task<T>` **独占**协程 `handle_`，析构时调用 `handle_.destroy()` 释放协程帧
- `TaskPromise::continuation_` 是**观察者**，由 `Task<T> operator co_await` 在挂起前赋值，父协程的 `Task<T>` 持有 handle 所有权
- `AsyncAwaitable::initiator_` 是 `std::function`，**值持有**回调闭包；闭包中捕获 `this` 指针，因此 awaitable 必须存活到回调触发——由外层 `co_await` 表达式的临时对象生命周期保证
- `Scheduler*` 是裸指针**观察者**，调用方负责确保 EventLoop 比 scheduler 先于注销

**为什么先画图**：协程层的所有权关系比一般类要绕——`coroutine_handle` 是非拥有句柄、`promise` 在帧内但被 handle 引用、`continuation_` 又是观察者。先把图画清楚，写代码时少踩 90% 的坑。

---

## 3. 第 2 步 — 厘清协程的初始化顺序

协程没有"启动"的概念，但每次创建 `Task<T>` 时编译器会展开如下序列：

```
[调用 someAsyncFunc() 的线程]

① 编译器生成调用 ::operator new(sizeof(coroutine_frame)) 分配协程帧（栈上）
   ── frame 内部包含：promise_type 子对象、参数副本、内部状态机变量、返回点 PC

② TaskPromise<T> 子对象就地构造
   ── value_、exception_、continuation_ 全部默认初始化（continuation 为空）

③ 编译器调用 promise.get_return_object()
   ── 返回 Task<T>{ coroutine_handle<TaskPromise<T>>::from_promise(promise) }

④ 编译器调用 promise.initial_suspend()
   ── 我们返回 std::suspend_always{} → 协程立刻挂起，控制流回到调用者
   ── 这就是"懒执行"的实现：调用 someAsyncFunc() 不会执行函数体一行代码

⑤ 调用者拿到 Task<T>，可选择 co_await task 或 task.syncWait()
```

`syncWait()` 的内部启动顺序：

```
Task<T>::syncWait()
└── handle_.resume()           ← 触发协程体首次执行
    └── 协程体内的代码顺序运行，遇到 co_await 才再次挂起
    └── 全部完成后到达 final_suspend，把 promise.value_ 填好
└── return std::move(*promise_.value_)
```

**关键收获**：理解了「调用 `someAsyncFunc()` 不会执行函数体一行」之后，整个 lazy task 模型就通了。`initial_suspend` 返回 `suspend_always` 是这一切的开关。

---

## 4. 第 3 步 — 走通三种典型调用链

### 4.1 场景 A：纯计算协程（无 IO）

```cpp
Task<int> compute(int x) {
    int doubled = co_await makeReady(x * 2);
    co_return doubled + 1;
}
int r = compute(10).syncWait();   // r = 21
```

调用链：

```
① [主线程] compute(10)
   编译器分配协程帧 → 构造 promise → get_return_object() → initial_suspend (suspend_always)
   返回 Task<int>，函数体未执行

② [主线程] .syncWait()
   handle_.resume() → 协程体开始

③ [协程体] co_await makeReady(20)
   makeReady 返回 ReadyAwaitable<int>{20}
   await_ready() == true → 不挂起，直接 await_resume() 返回 20
   doubled = 20

④ [协程体] co_return 21
   编译器调用 promise.return_value(21) → value_ = 21
   编译器调用 promise.final_suspend() → 我们返回 FinalAwaitable
   FinalAwaitable::await_suspend(handle) → 检查 continuation_，为空则不唤醒任何人

⑤ [主线程] syncWait 检查 promise_.value_ → 返回 21
```

### 4.2 场景 B：异步回调驱动协程

```cpp
Task<std::string> fetchAsync(EventLoop* loop) {
    std::string body = co_await AsyncAwaitable<std::string>([loop](auto cb){
        loop->queueInLoop([cb = std::move(cb)](){ cb("hello"); });
    });
    co_return body + " world";
}
```

```
① [主线程] fetchAsync(loop) → Task<string>，未执行

② [主线程] co_await task / task.syncWait()
   resume() → 进入协程体
   构造 AsyncAwaitable，await_ready() == false
   await_suspend(handle):
       initiator_([this, handle](string r){ result_ = r; handle.resume(); })
       initiator 内部 → loop->queueInLoop(lambda)
   await_suspend 返回，控制流让出

③ [IO 线程] EventLoop::doPendingFunctors()
   执行 [cb = ...](){ cb("hello"); }
   即 awaitable 内部回调：result_ = "hello"; handle.resume()

④ [IO 线程] 协程体恢复，body = await_resume() = "hello"
   co_return body + " world" → promise.value_ = "hello world"
   final_suspend → 唤醒 continuation_（如果有外层 co_await，则继续执行外层）

⑤ syncWait 唤醒（通过条件变量等待 promise 完成），返回 "hello world"
```

**关键点**：handle.resume() 在 IO 线程执行，因此协程体内的代码也运行在 IO 线程。如果业务代码需要回主线程，应在 awaitable 中显式切换调度器。

### 4.3 场景 C：异常传播

```cpp
Task<int> failing() { throw std::runtime_error("boom"); co_return 0; }
Task<int> outer()   { int v = co_await failing(); co_return v; }
try { outer().syncWait(); } catch (std::exception& e) { /* "boom" */ }
```

```
① outer() 协程帧构造，懒执行
② syncWait → resume outer
③ outer 协程体：co_await failing()
   failing() 协程帧构造，被 co_await → 设置 continuation = outer 的 handle
   outer 挂起，failing 开始 resume
④ failing 协程体抛出 runtime_error
   编译器调用 promise.unhandled_exception() → exception_ = current_exception()
   final_suspend → 检查 continuation == outer.handle
   FinalAwaitable::await_suspend 返回 outer.handle，编译器自动 resume outer
⑤ outer 恢复，await_resume() 检查 promise.exception_ → throw it
   outer 自身的协程体也抛出 → outer.promise.unhandled_exception() 捕获
⑥ syncWait 检查 outer.promise.exception_ → std::rethrow_exception
   异常逐层透传到 try-catch
```

异常传播完全不依赖回调链，是协程相对回调最显著的可读性收益。

**三个场景的共同点**：所有「跨线程恢复」「异常透传」「父子链接」都不需要业务写一行代码——编译器会按 promise + handle + awaitable 三个接口的契约自动展开状态机。

---

## 5. 第 4 步 — 写 TaskPromise 状态容器（用 variant 表达三态）

这段代码论证了：**为什么 promise 不需要在「异常路径」与「正常路径」上写两套返回逻辑**——variant 把三态（未完成 / 有值 / 有异常）合成单一对象，编译器替我们做穷尽性检查。

```cpp
// src/include/async/Coroutine.h
template <typename T> struct TaskPromise {
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    using ResultType = std::variant<std::monostate, T, std::exception_ptr>;

    ResultType result_;
    std::coroutine_handle<> continuation_{nullptr};

    Task<T> get_return_object() noexcept;
    std::suspend_always initial_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept {
        result_.template emplace<2>(std::current_exception());
    }
    template <typename U> void return_value(U &&val) {
        result_.template emplace<1>(std::forward<U>(val));
    }
    T getResult() {
        if (result_.index() == 2) std::rethrow_exception(std::get<2>(result_));
        return std::move(std::get<1>(result_));
    }
};
```

关键点：`result_.index()` 三态分别对应 `monostate`(0)、`T`(1)、`exception_ptr`(2)，`getResult()` 在唯一一处分支即可处理异常透传。`initial_suspend` 返回 `suspend_always` 是「lazy task」的核心——协程**不在被调用时就开始执行**，这让我们能在 `co_await` 处再决定执行时机。

**为什么不用 `optional<T> + exception_ptr` 两个字段**：那样要在每个读路径上写「先看 exception 再看 value」的两次判空，分支爆炸；而且语义上「同时有值又有异常」是不可能的，用 variant 的「互斥三态」最符合事实。

---

## 6. 第 5 步 — 写 final_suspend 与 FinalAwaiter（symmetric transfer 零开销跳转）

这段代码论证了：**协程结束时如何把控制权「无栈跳转」回等待它的 awaiter**，避免回调链上的栈深度增长。

```cpp
// src/include/async/Coroutine.h —— TaskPromise<T>::final_suspend
auto final_suspend() noexcept {
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(Handle h) noexcept {
            if (auto cont = h.promise().continuation_; cont)
                return cont;            // ← symmetric transfer：直接跳到 awaiter
            return std::noop_coroutine();
        }
        void await_resume() noexcept {}
    };
    return FinalAwaiter{};
}
```

关键点：`await_suspend` 返回 `coroutine_handle<>` 而非 `void`，编译器会把它优化成尾调用——这是 C++20 协程提案 P0913 的重要交付物。如果改成 `cont.resume()` 加 `return`，每条协程链的尾部都会多压一层栈，深度嵌套会爆栈。这就是为什么我们的 `TaskNestedThreeLevels` 测试能稳定通过。

**验证：追踪一次三层嵌套的 final_suspend 链**：

```
fn1 co_await fn2 co_await fn3                           栈深度
├─ fn3 跑完 co_return                                       3
├─ FinalAwaiter::await_suspend(h3) → return continuation=h2 (尾调用，不压栈)
├─ fn2 在 await_resume 里取到 fn3 结果，继续执行              2 (而非 4)
├─ fn2 跑完 co_return
├─ FinalAwaiter::await_suspend(h2) → return continuation=h1  (尾调用)
└─ fn1 恢复                                                 1 (而非 5)
```

如果不用 symmetric transfer，每层都会增加一层 `resume()` 调用栈帧，10 层嵌套就 30 层栈帧；用了之后整条链路始终是常数栈深。

---

## 7. 第 6 步 — 写 Task::operator co_await（链接父子协程）

这段代码论证了：**`co_await child` 表达式**展开后，子协程如何获知「完成时该 resume 谁」。

```cpp
// src/include/async/Coroutine.h —— Task<T>::operator co_await
auto operator co_await() && noexcept {
    struct Awaiter {
        Handle handle_;
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> awaiting) noexcept {
            handle_.promise().continuation_ = awaiting;  // ← 把父协程登记到子 promise
            return handle_;                              // symmetric transfer 启动子协程
        }
        T await_resume() {
            return handle_.promise().getResult();         // 异常会在这里 rethrow
        }
    };
    return Awaiter{handle_};
}
```

关键点：`await_suspend` 收到的 `awaiting` 就是父协程的句柄，写入子 promise 的 `continuation_` 字段后返回 `handle_`——编译器立即把控制权转移到子协程。子协程跑到 `co_return`/`final_suspend` 时，`FinalAwaiter` 会读出这条 `continuation_` 跳回父，整个链路只用两次「跳转」完成「父挂起→子运行→子完成→父恢复」。这正是我们在 day32 引言里提到的「异步代码同步写」背后的全部机制。

**为什么要 `&&`（rvalue ref qualifier）**：`co_await` 表达式产生的 awaiter 不应被复用——它持有的 `handle_` 是一次性的，第二次 await 会触发 UB。`&&` 让 `Task` 在 co_await 时必须移动，原 Task 的 `handle_` 被置空，编译期就把双重 await 拦下。

**§5–§7 三段代码就是协程层的全部核心机制**：variant 处理结果、FinalAwaiter 完成跳转、operator co_await 链接父子。剩下的工作都是把这三件事接到 EventLoop 上。

---

## 8. 第 7 步 — 验证：Task 与 AsyncAwaitable 的析构边界

`Task<T>` 是值类型，析构发生在它离开作用域时。三种典型时序：

| 场景 | Task 析构时协程是否完成 | 处理 |
|------|----------------------|------|
| **正常完成后**（co_return 已执行） | 是 | `handle_.destroy()` 释放帧，promise.value_ 已被消费 |
| **被另一 Task `co_await` 链接后**（移动语义） | n/a | 原 Task 的 `handle_` 已被 `std::exchange` 置空，析构无操作 |
| **异常路径**（promise.exception_ 未消费） | 是 | `handle_.destroy()` 仍能正常释放，但抛出的异常已存进 `exception_`，不会丢失 |

`AsyncAwaitable` 的析构隐含约束：

- awaitable 是 `co_await` 表达式的临时对象，标准保证活到完整表达式结束
- await_suspend 中传递的回调若**异步触发**（在另一个线程），可能在 awaitable 已析构后才回调
- 我们的实现把 `result_` 与 `initiator_` 都放在 awaitable 内，**回调里访问 `this->result_` 是 UB**

为此，**正确用法**：在 await_suspend 中将必要状态拷贝/移动到回调闭包里，不依赖 awaitable 自身存活。当前实现做得保守——在 await_suspend 内**同步**调用 initiator，把握"回调要么已触发要么由 initiator 自己保证生命周期"。后续 day33 引入 io_uring 后会用 `shared_state` 模式重构。

**生命周期黄金规则**：

1. `Task<T>` 是值类型，离开作用域 = 协程帧释放（除非已 move 出去）。
2. `awaitable` 是临时对象，活到完整 `co_await` 表达式结束（包含 `await_resume`）。
3. **回调可能在 awaitable 析构后触发**——因此回调闭包不要捕获 `awaitable*`，只能捕获自身需要的状态。
4. `coroutine_handle` 是非拥有句柄，复制不影响生命周期；用错了就会双重 destroy 崩溃。

这四条记住了，写到 day33+ 都不会出生命周期 bug。

---

## 9. 第 8 步 — 写单元测试覆盖正常 / 异常 / 嵌套 / 移动 / 批量

### 9.1 19 个 GTest 用例分布

| 测试名 | 覆盖场景 |
|--------|---------|
| `CoroutineTest.TaskInt_SimpleReturn` | `Task<int>` 基本返回值 |
| `CoroutineTest.TaskVoid_SimpleReturn` | `void` 偏特化 |
| `CoroutineTest.TaskString_Return` | 移动语义，避免拷贝大对象 |
| `CoroutineTest.TaskChain_Await` | 两个协程串联 |
| `CoroutineTest.TaskMultipleAwait` | 同一函数内多次 co_await |
| `CoroutineTest.TaskNestedThreeLevels` | 三层嵌套协程的 continuation 链 |
| `CoroutineTest.TaskException_Propagates` | 异常透传到 syncWait |
| `CoroutineTest.TaskVoidException_Propagates` | void 协程的异常路径 |
| `CoroutineTest.TaskException_InNestedAwait` | 子协程 throw 在父协程 co_await 处 rethrow |
| `CoroutineTest.ReadyAwaitable_Int` / `_Void` | 立即就绪 awaitable |
| `CoroutineTest.AsyncAwaitable_Callback` / `_Void` | 异步回调驱动的 awaitable |
| `CoroutineTest.TaskMove_Ownership` | Task 移动后原对象 handle 为空 |
| `CoroutineTest.MakeReady_Value` / `_Void` | 工具函数 |
| `CoroutineTest.Suspend_YieldsControl` | 显式让出后被外部 resume |
| `CoroutineTest.TaskVector_AccumulateResults` | 批量协程结果聚合 |
| `CoroutineTest.TaskConditional_EarlyReturn` | 协程内分支提前 co_return |

### 9.2 测试输出

```
$ cd build && ./CoroutineTest
[==========] Running 19 tests from 1 test suite.
...
[       OK ] CoroutineTest.TaskConditional_EarlyReturn (0 ms)
[----------] 19 tests from CoroutineTest (3 ms total)
[  PASSED  ] 19 tests.
```

### 9.3 验证：用 ASAN 跑一遍找内存问题

```bash
$ cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -B build-asan && cmake --build build-asan
$ ./build-asan/CoroutineTest
[  PASSED  ] 19 tests.   # 0 个 leak / 0 个 UAF
```

协程帧是堆分配的（编译器调用 `::operator new`），析构必须配对 `handle_.destroy()`。ASAN 干净 = 我们没漏掉任何 destroy。这是验证协程实现正确性的最低门槛。

---

## 10. 局限与下一步

- 当前 `AsyncAwaitable` 不支持取消（cancellation token）。后续若实现请求超时，需要引入 `stop_token` 风格机制
- 没有 `when_all` / `when_any` 组合子（需要 `std::variant` + 计数器，留待 day37 RPC 时引入）
- `EventLoopScheduler` 在跨线程 resume 时使用 `queueInLoop`，每次都触发一次 wakeup write —— 如果协程频繁切换调度器会有性能损失，可考虑批量提交
- 日志系统的 traceId 在 `co_await` 跨线程后会丢失，需要在 awaitable 中显式传递（day38 可观测性增强会处理）

### 10.1 演进路线

- **Day 33**：把 `AsyncAwaitable` 接到 io_uring，让 `co_await socket.read(buf)` 真正成为零回调的异步 IO。
- **Day 34**：基于无锁队列 + Work-Stealing Pool 实现协程调度器，让 `co_await` 跨线程恢复时几乎无锁。
- **Day 35**：协程帧用内存池分配，把每次 `Task<T>` 创建的 `::operator new` 干掉。
- **Day 36**：与 muduo 的回调风格在同一压测下横向对比，证明协程在多步业务下确实更快也更易写。
