# Day 32.5 — C++20 协程重构 / setOnMessageCallback 副作用消除 / std::bind 替换

## 0. 阅读指南

本篇记录了在 day32（全异步 Raft RPC）基础上的第二轮重构，代号 **day32.5**。

核心动机：day32 的 `startElection` + `onVoteReply` / `heartbeatTick` + `onHeartbeatReply`
是"两段式跳板"设计：第一段发射 `callAsync`，第二段是回调函数。两段之间通过多个参数传递上下文，
逻辑分散且难以追踪。C++20 协程可以把两段合并为一段连续的线性代码，并彻底消除跳板。

```
§1  C++20 协程基础（给熟悉 TypeScript async/await 的读者）
§2  改进 A — std::bind → lambda（Phase 0 cleanup）
§3  改进 B — setOnMessageCallback 副作用消除（Phase 0 cleanup）
§4  改进 C — 协程基础设施：Task.h（FireAndForget + ResumeOnLoop）
§5  改进 D — RpcCallAwaiter：将 callAsync 桥接为 co_await（Phase 2）
§6  改进 E — RaftNode 协程化（Phase 3）
§7  工程化配置（CMakeLists.txt C++17→C++20）
§8  验证
§9  局限与下一步
```

---

## 1. 引言

### 1.1 两段式跳板的问题

day32 完成后，RaftNode 的选举流程如下：

```
becomeCandidate()
  └─ startElection()                    ← 第一段：构建请求、调用 callAsync
       └─ callAsync(RequestVote, ..., callback)
            └─ [网络传输，异步等待]
                 └─ callback(ok, resp)  ← 第二段：调用 onVoteReply
                      └─ onVoteReply(term, peerId, ok, reply)
```

问题：
1. **逻辑分散**：同一选举轮次的"发请求"和"处理响应"在两个函数里，中间通过 `electionTerm`/`peerId` 参数传递上下文；
2. **守卫重复**：`onVoteReply` 开头的两个守卫（检查 state_/currentTerm_）在 `startElection` 里已隐含了这层逻辑，代码读起来需要在两个函数之间来回跳跃；
3. **可扩展性差**：如果将来要在发票和收票中间插入日志重放、prevLogIndex 校验等逻辑，两段式会快速膨胀为三段、四段。

### 1.2 协程版效果

用协程把两段合并为一段：

```cpp
FireAndForget RaftNode::collectVote(uint64_t electionTerm, Peer peer) {
    // 第一段：构建请求
    RequestVoteArgs args{electionTerm, id_, lastLogIndex(), lastLogTerm()};

    // ── 挂起点 ── co_await 此处挂起，callAsync 被派发
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo("RequestVote", ...);

    // ── 恢复点 ── 响应到达后在 loop_ 线程继续执行
    // 第二段：处理响应（与第一段在同一函数，上下文共享，无需跨函数传参）
    if (!ok || state_ != Candidate || currentTerm_ != electionTerm) co_return;
    // ...处理投票结果
}
```

---

## 2. 改进 A — std::bind → lambda（Phase 0 cleanup）

### 2.1 业务场景

项目中三处使用了 `std::bind`：

```cpp
// Connection.cpp（构造函数）
channel_->setReadCallback(std::bind(&Connection::doRead, this));
channel_->setWriteCallback(std::bind(&Connection::doWrite, this));

// Acceptor.cpp
acceptChannel_->setReadCallback(std::bind(&Acceptor::acceptConnection, this));

// Eventloop.cpp
evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));
```

### 2.2 改进思路

`std::bind` 是 C++11 的历史遗留，现代 C++ 推荐用 lambda：
- **可读性**：`[this] { doRead(); }` 比 `std::bind(&Connection::doRead, this)` 更直白；
- **性能**：编译器能内联 lambda，`std::bind` 通常无法内联（返回值是 opaque 的 `std::_Bind<...>` 类型）；
- **一致性**：项目其他地方（TcpServer、RpcServer）已全部使用 lambda，保持风格统一；
- **C++20 兼容性**：`std::bind` 在 C++20 中已标记为"不推荐使用"（deprecated in C++17 for some usage patterns）。

### 2.3 编码实现步骤

**Connection.cpp（构造函数）**：
```cpp
// 修改前
channel_->setReadCallback(std::bind(&Connection::doRead, this));
channel_->setWriteCallback(std::bind(&Connection::doWrite, this));

// 修改后
channel_->setReadCallback([this] { doRead(); });
channel_->setWriteCallback([this] { doWrite(); });
```

**Acceptor.cpp**：
```cpp
// 修改前
acceptChannel_->setReadCallback(std::bind(&Acceptor::acceptConnection, this));

// 修改后
acceptChannel_->setReadCallback([this] { acceptConnection(); });
```

**Eventloop.cpp**：
```cpp
// 修改前
evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));

// 修改后
evtChannel_->setReadCallback([this] { handleWakeup(); });
```

### 2.4 嵌入执行路径

这三处都是"注册 channel 的 read 回调"，在 IO 事件触发时（kqueue 返回可读事件），EventLoop 调用 `Channel::handleEvent()` → 调用 `readCallback_`。替换前后行为完全一致，仅消除了 `<functional>` 头文件的依赖（Connection.cpp/Acceptor.cpp/Eventloop.cpp 可移除 `#include <functional>`）。

---

## 3. 改进 B — setOnMessageCallback 副作用消除（Phase 0 cleanup）

### 3.1 业务场景

`setOnMessageCallback` 原来的实现：

```cpp
void Connection::setOnMessageCallback(std::function<void(Connection *)> const &cb) {
    onMessageCallback_ = cb;
    // 隐藏的副作用：改变了 channel_ 的 read 回调！
    channel_->setReadCallback([this] { Business(); });
}
```

问题：**setter 函数不应该有副作用**。一个 `set` 函数改变了另一个对象的回调，违反了最小惊讶原则：

- 如果调用者只是想"更新回调但不想切换读模式"，没有任何办法阻止副作用；
- 如果调用者在 `setOnMessageCallback` 之前还需要设置其他东西（如 `enableInLoop`），顺序就变得微妙；
- AsyncRpcClient 在 `onConnected` 里调用 `setOnMessageCallback` 时，需要非常清楚地知道这会触发 channel 切换，这是隐式知识。

### 3.2 改进思路

将 setter 拆分为两个职责明确的函数：
- `setOnMessageCallback(cb)`：**只存储**回调，无副作用；
- `enableMessageMode()`：**显式**将 `channel_` 的 read 回调切换为 `Business()`。

调用方必须显式调用两者，意图清晰：

```cpp
// 修改后（TcpServer::newConnection）
conn->setOnMessageCallback(onMessageCallback_);
conn->enableMessageMode();  // 明确：从这一刻起开始接收业务数据
conn->enableInLoop();
```

### 3.3 编码实现步骤

**Connection.h** — 新增声明：
```cpp
// 切换到"业务模式"：把 channel_ 的 read 回调设为 Business()。
// 必须在 setOnMessageCallback() 之后、enableInLoop() 之前显式调用。
void enableMessageMode();
```

**Connection.cpp** — 拆分实现：
```cpp
void Connection::setOnMessageCallback(std::function<void(Connection *)> const &cb) {
    // 只存储回调，不产生任何副作用
    onMessageCallback_ = cb;
}

void Connection::enableMessageMode() {
    channel_->setReadCallback([this] { Business(); });
}
```

**构造函数中同时清除构造时的临时 read 回调**：
```cpp
Connection::Connection(...) {
    // channel_ 初始 read 回调指向 doRead（原始数据读取）
    channel_->setReadCallback([this] { doRead(); });
    channel_->setWriteCallback([this] { doWrite(); });
    // ...
}
```

`doRead` 是底层读取（从 fd 读字节到 inputBuffer_），`Business` 是调用 `onMessageCallback_`（应用层处理）。

### 3.4 嵌入执行路径

**TcpServer::newConnection**：
```cpp
conn->setOnMessageCallback(onMessageCallback_);
conn->enableMessageMode();  // ← 新增，明确切换
conn->setDeleteConnectionCallback(...);
conn->enableInLoop();
```

**AsyncRpcClient::onConnected**：
```cpp
conn_->setOnMessageCallback([this](Connection *c) { onMessage(c); });
conn_->enableMessageMode();  // ← 新增，明确切换
conn_->enableInLoop();
```

---

## 4. 改进 C — 协程基础设施：Task.h

### 4.1 C++20 协程基础知识

> 如果你熟悉 TypeScript 的 `async/await`，这一节将帮助你快速理解 C++20 协程的底层机制。

#### 4.1.1 协程 vs 普通函数 vs 线程

| | 普通函数 | 线程 | C++20 协程 |
|---|---|---|---|
| 调用时 | 立即执行 | 新线程开始执行 | 视 `initial_suspend` 决定 |
| 暂停点 | 不可暂停（只能返回） | 不可暂停（除非 mutex/sleep） | `co_await` / `co_yield` 处主动挂起 |
| 挂起时保存什么 | 不适用 | 整个线程栈 | 协程帧（只保存函数局部状态） |
| 恢复 | 不适用 | 调度器决定 | 显式调用 `handle.resume()` |
| 内存开销 | 调用栈 | MB 级（线程栈） | KB 级（仅协程帧） |
| 上下文切换 | 无 | 昂贵（内核级） | 极廉价（用户态，一次函数调用） |

**与 TypeScript async/await 的对比**：

TypeScript:
```typescript
async function fetchData(): Promise<string> {
    const result = await httpGet(url);  // 挂起，等待 Promise resolve
    return result.data;                  // 恢复后继续
}
```

C++20:
```cpp
FireAndForget fetchData() {
    auto [ok, result] = co_await rpcClient->callAsyncCo(url);  // 挂起，等待 RPC 返回
    // 恢复后继续
}
```

核心相似：`co_await` 和 `await` 都是"挂起点"——函数执行到这里时保存状态，让出执行权，等待异步操作完成后再恢复。

**关键区别**：TypeScript 的 `async function` 自动返回 `Promise`，事件循环自动恢复协程；C++ 需要手动定义恢复策略（由 awaiter 的 `await_suspend` 决定何时、由谁 `handle.resume()`）。

#### 4.1.2 协程帧（Coroutine Frame）

当编译器看到函数体中有 `co_await`/`co_return`/`co_yield` 时，该函数是协程。编译器会：
1. 把所有局部变量和挂起恢复所需状态打包进一个 **协程帧（coroutine frame）**，分配在堆上；
2. 生成一个状态机，每次 `resume()` 从上次挂起的位置继续。

类比 TypeScript：V8 也会把 `async function` 编译成内部状态机，只是对程序员透明。

#### 4.1.3 `promise_type` 协议

协程的行为由 **`promise_type`** 内嵌类型控制。编译器展开协程 `F()` 的方式（伪代码）：

```
// 编译器生成的伪代码（实际在协程帧中）：
F_frame {
    promise_type promise;
    <局部变量们>
    <挂起状态枚举>
};

// 调用 F() 时：
auto frame = new F_frame();
auto return_obj = frame->promise.get_return_object();  // ← 这就是调用方拿到的返回值
co_await frame->promise.initial_suspend();             // ← 是否立即执行
// 执行函数体 ...
// 遇到 co_await expr：
//   1. 求值 expr，得到一个 awaiter
//   2. 调用 awaiter.await_ready()：若 true，跳过挂起
//   3. 若需挂起：调用 awaiter.await_suspend(coroutine_handle)
//   4. 协程挂起（控制权返回调用方）
//   5. 某处调用 handle.resume()
//   6. 调用 awaiter.await_resume()，其返回值作为 co_await 表达式的结果
// 函数体结束：
co_await frame->promise.final_suspend();               // ← 完成后是否挂起
delete frame;  // 若 final_suspend 返回 suspend_never
```

#### 4.1.4 Awaiter 三方法

实现 `co_await` 表达式的类型需要提供三个方法：

```cpp
struct MyAwaiter {
    // 1. 是否跳过挂起（优化路径）。
    //    若 true：协程不挂起，直接调用 await_resume() 取结果，继续执行。
    //    若 false：继续调用 await_suspend()。
    bool await_ready() const noexcept;

    // 2. 挂起时调用。h 是本协程的 handle（可用来恢复）。
    //    - 返回 void：协程挂起，控制权返回到调用 resume() 的地方（或首次调用的地方）。
    //    - 返回 bool：false = 继续执行（不挂起），true = 挂起。
    //    - 返回 coroutine_handle<> = 立刻恢复另一个协程（对称传递）。
    void await_suspend(std::coroutine_handle<> h);

    // 3. 协程恢复后，co_await 表达式的求值结果。
    //    类比 Promise.resolve(value) 的 value。
    ResultType await_resume();
};
```

类比 TypeScript：
- `await_ready()` ≈ `Promise.resolve()` 的已resolved状态（微任务队列直接跳过异步）；
- `await_suspend()` ≈ `.then(callback)` 注册回调；
- `await_resume()` ≈ 回调收到的 `value`。

#### 4.1.5 `coroutine_handle<>`

`std::coroutine_handle<T>` 是一个轻量级句柄（本质是指向协程帧的指针，8 字节），提供：
- `handle.resume()` — 从挂起点继续执行协程；
- `handle.destroy()` — 销毁协程帧（释放内存）；
- `handle.done()` — 协程是否已完成（执行到 `co_return` 或函数末尾）。

`std::coroutine_handle<>` = `std::coroutine_handle<void>` = 类型擦除版（可引用任意 promise_type 的协程）。

#### 4.1.6 与 TypeScript 最大的区别：谁来恢复？

TypeScript：事件循环自动把已 resolve 的 Promise 恢复对应的 async 函数（微任务队列）。

C++：需要**明确决定**在哪个线程、由谁调用 `handle.resume()`。这是 C++20 协程灵活也危险的地方。

本项目的约定：**RpcCallAwaiter 的 callback 在 `loop_` 线程触发，`h.resume()` 也在 `loop_` 线程**。由此保证协程体（包括恢复后的代码）始终在 `loop_` 线程执行，维持 single-thread invariant（Raft 状态无需加锁）。

### 4.2 FireAndForget 模式

#### 4.2.1 设计目标

`collectVote` 和 `sendHeartbeat` 不需要调用方等待结果——结果（投票/心跳）的处理逻辑都在协程体内部。这种"点火即忘"的模式用 `FireAndForget` 返回类型表达：

```cpp
struct FireAndForget {
    struct promise_type {
        FireAndForget get_return_object() noexcept { return {}; }
        // initial_suspend = suspend_never：调用后立刻开始执行（不挂起）
        // 类比 TypeScript：async function 调用后立刻执行到第一个 await
        std::suspend_never initial_suspend() noexcept { return {}; }
        // final_suspend = suspend_never：完成后自动销毁协程帧
        // 不需要外部调用 handle.destroy()
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        // 未处理异常：terminate（协程不应抛异常逃逸）
        void unhandled_exception() noexcept { std::terminate(); }
    };
};
```

**关键设计决策**：`initial_suspend = suspend_never` + `final_suspend = suspend_never`。

这意味着：
1. `collectVote(term, peer)` 调用后立刻开始执行协程体（不挂起），直到遇到第一个 `co_await`；
2. 协程完成（`co_return` 或函数末尾）后，协程帧自动销毁；
3. 调用方（`runElection`）不持有任何 handle，不需要手动管理协程生命周期。

#### 4.2.2 线程安全

```
loop_ 线程                              网络线程
────────────────────                   ──────────────────
runElection()
  ├─ collectVote(term, peer0)  ─ 启动协程，挂起于 co_await
  ├─ collectVote(term, peer1)  ─ 启动协程，挂起于 co_await
  └─ collectVote(term, peer2)  ─ 启动协程，挂起于 co_await

                                       [网络响应到达]
                                       callAsync callback
                                         ok_ = ok;
                                         resp_ = std::move(resp);
                                         h.resume();  → loop_ 线程

loop_ 线程（处理 queueInLoop 事件）
  collectVote 协程恢复
  ├─ 检查守卫（state_, currentTerm_）
  ├─ 解析 reply
  └─ becomeLeader() / co_return
```

**all runs on loop_ thread**：`collectVote` 从启动到完成（包括处理响应）全部在 `loop_` 线程。Raft 状态（`currentElectionVotes_`/`state_`/`currentTerm_`）无需加锁。

### 4.3 ResumeOnLoop 辅助 Awaitable

```cpp
template<typename Loop>
struct ResumeOnLoop {
    Loop *loop;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const {
        loop->queueInLoop([h]() mutable { h.resume(); });
    }
    void await_resume() const noexcept {}
};
```

**用途**：在任意线程启动协程后，切换到指定 `EventLoop` 线程继续执行：

```cpp
FireAndForget someTask() {
    // 当前线程：任意（可能是 sub-reactor）
    co_await ResumeOnLoop{&loop_};
    // 从这里开始：保证在 loop_ 线程执行
    // 可以安全访问 Raft 状态
}
```

本版本中 `RaftNode` 的协程（`collectVote`/`sendHeartbeat`）均从 `loop_` 线程启动，不需要 `ResumeOnLoop`。但未来如果有从 RPC 回调（sub-reactor 线程）启动协程的需求，`ResumeOnLoop` 可直接派上用场。

---

## 5. 改进 D — RpcCallAwaiter：将 callAsync 桥接为 co_await

### 5.1 业务场景

`AsyncRpcClient::callAsync` 是基于回调的异步接口：

```cpp
void callAsync(const std::string &method, const std::string &json,
               Callback callback, int timeoutMs = 200);
```

协程需要一个"可 co_await 的"接口：

```cpp
auto [ok, resp] = co_await client->callAsyncCo("RequestVote", reqJson, 150);
```

`RpcCallAwaiter` 负责把"回调风格"桥接为"awaitable 风格"，这是 C++20 协程最常见的模式。

类比 TypeScript：
```typescript
// 回调风格
function callAsync(method, cb) { /* ... */ }

// 桥接为 Promise（类比 RpcCallAwaiter）
function callAsyncPromise(method): Promise<[boolean, string]> {
    return new Promise((resolve) => {
        callAsync(method, (ok, resp) => resolve([ok, resp]));
    });
}

// 使用
const [ok, resp] = await callAsyncPromise("RequestVote");
```

### 5.2 实现原理

```cpp
class RpcCallAwaiter {
public:
    RpcCallAwaiter(AsyncRpcClient *client, std::string method,
                   std::string json, int timeoutMs);

    // 1. 永远不跳过挂起（RPC 必然是异步的）
    bool await_ready() const noexcept { return false; }

    // 2. 挂起时：发起 callAsync，注册 callback
    void await_suspend(std::coroutine_handle<> h) {
        client_->callAsync(
            method_, json_,
            [this, h](bool ok, std::string resp) mutable {
                ok_   = ok;             // 写入结果到协程帧
                resp_ = std::move(resp);
                h.resume();            // 恢复协程（在 loop_ 线程）
            },
            timeoutMs_);
        // await_suspend 返回后，协程挂起。
        // callAsync 立刻返回（非阻塞），loop_ 继续处理其他事件。
    }

    // 3. 协程恢复后，co_await 的结果
    std::pair<bool, std::string> await_resume() noexcept {
        return {ok_, std::move(resp_)};
    }

private:
    AsyncRpcClient *client_;
    std::string method_, json_;
    int timeoutMs_;
    bool ok_{false};        // 结果存储在协程帧上
    std::string resp_;      // 协程帧存活时，callback 写入安全
};
```

### 5.3 协程帧生命周期安全分析

**问题**：callback lambda 捕获了 `this`（`RpcCallAwaiter*`）和 `h`（协程帧 handle）。这两者在 callback 触发时是否安全？

**分析**：
- `RpcCallAwaiter` 对象存活在**协程帧**上（它是 `co_await` 表达式的临时对象，编译器把它存入协程帧的局部变量区）；
- 协程帧通过 `FireAndForget`（`final_suspend = suspend_never`）管理：帧在 `final_suspend` 之前始终有效；
- callback 触发时 → 写 `ok_/resp_` → 调用 `h.resume()` → 协程从 `await_resume()` 处恢复 → 继续执行协程体 → 到达 `co_return` 或末尾 → `final_suspend = suspend_never` → 帧销毁；
- **写入（`ok_/resp_`）** 发生在 `h.resume()` 之前 → 帧存活；
- **帧销毁** 在协程完成后 → callback 里的 `h.resume()` 调用点之后很久才析构。

**结论**：安全。

### 5.4 停机安全（stop() 路径）

`RaftNode::stop()` 时：
1. `peerClients_.clear()` → 每个 `AsyncRpcClient` 析构；
2. `AsyncRpcClient` 析构 → `failAllPending()` → 所有 pending 的 `callAsync` 回调以 `{ok=false, ""}` 触发；
3. `RpcCallAwaiter` 的 callback 触发 → `ok_ = false` → `h.resume()`；
4. `collectVote`/`sendHeartbeat` 协程恢复 → 检查 `!ok` 守卫 → `co_return`；
5. 协程帧自动销毁（`final_suspend = suspend_never`）。

**结论**：stop() 路径下，所有飞行中的协程都会在 `peerClients_.clear()` 后被依次清理，无内存泄漏，无 use-after-free。

---

## 6. 改进 E — RaftNode 协程化（Phase 3）

### 6.1 旧版 vs 新版对比

#### 选举（requestVote）

**旧版（两段式）**：

```cpp
// 第一段：startElection()
void RaftNode::startElection() {
    uint64_t term = currentTerm_.load();
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        RequestVoteArgs args{term, id_, lastLogIndex(), lastLogTerm()};
        getOrCreateClient(peer)->callAsync("RequestVote", json(args).dump(),
            [this, term, peerId=peer.id](bool ok, std::string resp) {
                RequestVoteReply reply{};
                if (ok) { try { reply = json::parse(resp).get<RequestVoteReply>(); }
                          catch (...) { ok = false; } }
                onVoteReply(term, peerId, ok, reply);  // 跳转到第二段
            }, 150);
    }
}

// 第二段：onVoteReply()（跳板）
void RaftNode::onVoteReply(uint64_t electionTerm, int peerId, bool ok,
                            RequestVoteReply reply) {
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) return;
    if (!ok) return;
    if (reply.term > currentTerm_.load()) { becomeFollower(reply.term); return; }
    if (reply.voteGranted) {
        ++currentElectionVotes_;
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}
```

**新版（协程版）**：

```cpp
// 触发入口（不变）
void RaftNode::runElection() {
    uint64_t term = currentTerm_.load();
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        collectVote(term, peer);  // 并发发射 N-1 个协程
    }
}

// 单段协程：发请求 + 处理响应合为一体
FireAndForget RaftNode::collectVote(uint64_t electionTerm, Peer peer) {
    RequestVoteArgs args{electionTerm, id_, lastLogIndex(), lastLogTerm()};

    // ── 挂起：callAsync 派发出去 ────────────────────────────────────────
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "RequestVote", json(args).dump(), 150);

    // ── 恢复：在 loop_ 线程处理响应 ─────────────────────────────────────
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) co_return;
    if (!ok) co_return;

    RequestVoteReply reply{};
    try { reply = json::parse(respJson).get<RequestVoteReply>(); }
    catch (...) { co_return; }

    if (reply.term > currentTerm_.load()) { becomeFollower(reply.term); co_return; }
    if (reply.voteGranted) {
        ++currentElectionVotes_;
        LOG_INFO << "[Node " << id_ << "] 收到节点 " << peer.id
                 << " 的投票（已得票=" << currentElectionVotes_ << "/" << quorum_ << "）";
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}
```

**代码量对比**：旧版 2 个函数 ~40 行，新版 2 个函数 ~25 行（减少 37%）；逻辑完全等价。

#### 心跳（appendEntries）

**旧版**：

```cpp
void RaftNode::heartbeatTick() {
    if (state_.load() != State::Leader) return;
    uint64_t term = currentTerm_.load();
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        AppendEntriesArgs args{term, id_};
        getOrCreateClient(peer)->callAsync("AppendEntries", json(args).dump(),
            [this, peerId=peer.id](bool ok, std::string resp) {
                AppendEntriesReply reply{};
                if (ok) { try { reply = json::parse(resp).get<AppendEntriesReply>(); }
                          catch (...) { ok = false; } }
                onHeartbeatReply(peerId, ok, reply);
            }, 100);
    }
}

void RaftNode::onHeartbeatReply(int /*peerId*/, bool ok, AppendEntriesReply reply) {
    if (!ok) return;
    if (reply.term > currentTerm_.load()) becomeFollower(reply.term);
}
```

**新版**：

```cpp
void RaftNode::heartbeatTick() {
    if (state_.load() != State::Leader) return;
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        sendHeartbeat(peer);
    }
}

FireAndForget RaftNode::sendHeartbeat(Peer peer) {
    AppendEntriesArgs args{currentTerm_.load(), id_};

    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "AppendEntries", json(args).dump(), 100);

    if (!ok) co_return;
    try {
        auto reply = json::parse(respJson).get<AppendEntriesReply>();
        if (reply.term > currentTerm_.load()) becomeFollower(reply.term);
    } catch (...) {}
}
```

### 6.2 守卫逻辑说明

`collectVote` 恢复后的两个守卫：

```cpp
// 守卫①：state_ != Candidate
// 含义：在等待投票响应期间（网络延迟可能 10ms~150ms），
//   当前节点可能已经：
//   (a) 收到更高 term 的消息 → becomeFollower：state_ = Follower
//   (b) 收到足够选票 → becomeLeader：state_ = Leader
//   (c) 收到 Leader 心跳 → becomeFollower：state_ = Follower
//   任一情况下，这个迟到的投票都无效。
if (state_.load() != State::Candidate) co_return;

// 守卫②：currentTerm_ != electionTerm
// 含义：节点的 term 可能已经递增（收到更高 term → becomeFollower 递增 term）。
// 旧 term 的投票不能计入新 term 的选举。
if (currentTerm_.load() != electionTerm) co_return;
```

注意：两个守卫实际写在一行 `if (A || B) co_return;`，短路求值。

---

## 7. 工程化配置

### 7.1 CMakeLists.txt：C++17 → C++20

```cmake
# 修改前
set(CMAKE_CXX_STANDARD 17)

# 修改后
set(CMAKE_CXX_STANDARD 20)
```

C++20 协程相关头文件 `<coroutine>` 需要 C++20 标准。Apple Clang 14+（Xcode 14+）已完整支持。

### 7.2 include 路径

新增 `src/include/coro` 到 NetLib 的 include 路径：

```cmake
target_include_directories(NetLib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/coro>
    # ...
)
```

包含关系：`RaftNode.h` → `#include "coro/Task.h"` → `#include <coroutine>`

---

## 8. 验证

### 8.1 构建验证

```bash
# 完整清理 + 重新 configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 编译 NetLib（含 RaftNode、AsyncRpcClient）
cmake --build build --target NetLib -j

# 编译示例 HTTP server
cmake --build build --target http_server -j

# 运行测试
cd build && ctest --output-on-failure
```

**实际输出（day32.5 完成后）**：
```
[100%] Built target NetLib
```
零警告，零错误。

### 8.2 功能验证

Raft 协程版与原版行为完全等价（协程只是改变了"发请求+处理响应"的代码组织方式，不影响 Raft 协议逻辑）。可通过以下方式验证：

```bash
# 启动 3 节点集群（需要 3 个终端）
RAFT_NODE_ID=0 RAFT_RPC_PORT=9000 ./build/examples/raft_server
RAFT_NODE_ID=1 RAFT_RPC_PORT=9001 ./build/examples/raft_server
RAFT_NODE_ID=2 RAFT_RPC_PORT=9002 ./build/examples/raft_server

# 观察日志中的选举和心跳信息：
# [Node 0] 收到节点 1 的投票（已得票=2/2）
# [Node 0] 成为 Leader（term=1）
```

---

## 9. 局限与下一步

### 9.1 当前局限

1. **日志复制未实现**：`AppendEntries` 当前只发送空心跳（`args` 无 `entries` 字段）。Raft 完整实现需要携带日志条目，`sendHeartbeat` 协程届时需要扩展为 `replicateLog` 协程。

2. **持久化未实现**：`currentTerm_`/`votedFor_`/`log_` 应在每次修改后持久化到磁盘。节点重启后需从磁盘恢复状态。

3. **`ResumeOnLoop` 未被使用**：基础设施已就绪，等待未来有从非 `loop_` 线程启动协程的需求。

4. **协程异常策略 = terminate**：`FireAndForget::promise_type::unhandled_exception` 直接 `std::terminate()`。生产场景可改为捕获异常并记录日志，但需确保协程体内的异常都能被妥善处理，避免静默失败。

### 9.2 下一步（day33 计划）

day33 原计划：探索 **io_uring 后端**（Linux 专属高性能 IO）。

协程重构为 io_uring 后端铺好了路：
- io_uring 的 completion queue 完全可以被封装成 awaitable（`IoUringSqeAwaiter`）；
- 将来 `read`/`write` 可以直接 `co_await ioUringRead(fd, buf, len)` — 和 `callAsyncCo` 一个风格；
- 没有协程的情况下，io_uring 的回调链会比 epoll/kqueue 更难管理，协程正好消解这一复杂度。
