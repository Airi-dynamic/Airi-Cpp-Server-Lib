# Day 30.5 — 项目结构与运行时全景梳理（所有权 / 启动序 / 完整场景调用链）

> **主题**：经过 30 天迭代，项目层层封装与回调注册已经形成了"知道每个类是什么、却讲不清一次请求到底走了多少 syscall"的状态。本日不引入任何新功能，专门把项目当作一个**可逐步追踪的运行时系统**来重新梳理：所有权关系、初始化顺序、完整场景的调用链、跨线程切换点、最终落到的系统调用。
> **目标**：读完本篇后，你应当能在脑中"放慢镜头"，对一个连接从 `accept()` 到 `close()` 的每一步说出：现在在哪个线程、哪个对象的哪个方法、为什么要在这里调、副作用是什么。
> **基于**：Day 30（项目收官状态）。代码引用统一指向 [HISTORY/day30/src/](../HISTORY/day30/src/) 快照，与当前根目录 `src/` 相同。

---

## 0. 阅读指南

- §1 引言说明本篇的"为什么"。
- §2 是**全景所有权树**：所有 unique_ptr / shared_ptr 的归属与析构次序。读到此节卡壳就回 §2 查。
- §3 是**启动顺序**：`main()` → `srv.start()` 内部到底发生了什么、构造了多少对象、为什么子线程必须先就绪主线程才能 listen。
- §4-§7 是 4 个完整场景的"逐函数代码追踪"，每个场景都按本仓库 [日志模板规范.md](日志模板规范.md) §3 的"四块结构"组织（业务+时序表 / 逐函数代码追踪 / ASCII 图 / 一句话职责表）。
- §8 跨线程通信备忘录，是所有"跨 reactor 操作"必须经过的 3 道闸门。
- §9 易错点速查：30 天里踩过的 5 个真实坑，每条都标出"代码长这样就一定错"的特征。
- §10 提供 lldb / dtrace / sample 实地观察方法，让本文的每个步骤都"亲眼看得见"。

---

## 1. 引言：为什么需要这一篇

到 day30 收官时，本项目已经累积成下面这个规模：

```
  src/
    include/  37 个头文件
    common/   22 个 .cpp
  examples/   1 个 ~430 行的 demo 服务器
  tests/      10 个测试套件，34 个 case
  benchmark/  conn_scale_test (RSS) + wrk + flamegraph / Instruments trace
```

层级是清晰的，但**运行时层面已经几乎没有人能脱口讲清下面这种问题**：

> Q1：客户端 TCP `connect(127.0.0.1:18888)` 之后，到我的 `handleApiUsers` 被调用，**期间一共建了几个对象、跑过几个线程、做了几次 syscall**？
> Q2：当回调里 `resp->setBody("hello")` 写完之后，"hello" 这 5 个字节**最终是怎么走出网卡**的？是直接 `write()` 吗？什么时候才会写到 OutputBuffer？
> Q3：客户端突然 `Ctrl+C`，**~Connection() 在哪个线程触发**？为什么 day27 那个 KqueuePoller UAF 必须用 `release()` + `delete raw` 而不能 shared_ptr？
> Q4：服务端 `Ctrl+C`，到所有 `EventLoop` 析构、所有 fd 关闭，**这些动作的顺序是什么**？为什么先 `joinAll()` 再让成员逆序析构？

day25-day29 的 dev-log 各自讲了某个改动的 X 节，但没有一篇把它们**串起来当成一个连续的运行时叙事**。本篇就是这个"串起来"。

> **声明**：本篇不引入任何新代码、不改任何设计，纯粹是对现状的"放慢镜头"重述。下一天 day31 才会真正动手加 WebSocket。

---

## 2. 模块全景与所有权树

### 2.1 一张图：对象层级与所有权

下面这棵树用 `└──` 表示 **唯一所有权（unique_ptr / 直接成员）**，用 `╌╌` 表示 **借用裸指针 / weak_ptr / 不持有所有权**。每个节点尾部 `[线程归属]` 标明该对象在哪个线程构造并且只能在哪个线程被读写访问。

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ main()  [main thread, tid=T0]                                               │
└─────────────────────────────────────────────────────────────────────────────┘
   │
   │ 栈对象（局部变量）
   │
   ├── HttpServer srv                                                  [T0]
   │   │
   │   ├── std::unique_ptr<TcpServer> server_                          [T0]
   │   │   │
   │   │   ├── std::unique_ptr<Eventloop> mainReactor_                 [T0]
   │   │   │   │
   │   │   │   ├── std::unique_ptr<Poller> poller_  (Kqueue/Epoll)    [T0]
   │   │   │   │   └── int kqueueFd_     ── kqueue() 返回的内核句柄
   │   │   │   ├── std::unique_ptr<Channel> evtChannel_  (wakeup)     [T0]
   │   │   │   │   └── 监听 wakeupReadFd_ / evtfd_
   │   │   │   ├── std::unique_ptr<TimerQueue> timerQueue_             [T0]
   │   │   │   │   └── std::set<{TimeStamp, Timer*}> timers_
   │   │   │   ├── int wakeupReadFd_ / wakeupWriteFd_  (macOS pipe)
   │   │   │   │   或 int evtfd_                       (Linux eventfd)
   │   │   │   ├── std::vector<std::function<void()>> pendingFunctors_
   │   │   │   ├── std::mutex mutex_   ← 仅保护 pendingFunctors_
   │   │   │   └── std::thread::id tid_ = T0  ← 构造时捕获，决定 isInLoopThread()
   │   │   │
   │   │   ├── std::unique_ptr<Acceptor> acceptor_                     [T0]
   │   │   │   ├── std::unique_ptr<Socket> sock_   (listen fd, 一次 socket()+bind()+listen())
   │   │   │   └── std::unique_ptr<Channel> acceptChannel_
   │   │   │       └── 借用 mainReactor_ 的 poller_ 注册 EVFILT_READ
   │   │   │
   │   │   ├── std::unique_ptr<EventLoopThreadPool> threadPool_        [T0]
   │   │   │   ├── Eventloop *mainLoop_    ╌╌╌╌╌╌  (借用，不持有)
   │   │   │   ├── std::vector<std::unique_ptr<EventLoopThread>> threads_
   │   │   │   │   └── EventLoopThread #i                              [T0 构造]
   │   │   │   │       ├── std::thread thread_   ── 子线程 Ti
   │   │   │   │       └── std::unique_ptr<Eventloop> loop_            [Ti 构造]
   │   │   │   │           └── (内部结构同 mainReactor_，但 tid_ = Ti)
   │   │   │   └── std::vector<Eventloop*> loops_  ╌╌╌╌  (借用 thread 内的 loop_)
   │   │   │
   │   │   └── std::unordered_map<int, std::unique_ptr<Connection>> connections_  [T0]
   │   │       │  ★ 重要约束：该 map 的所有 insert/erase 操作只能在 T0 执行
   │   │       │
   │   │       └── Connection (per fd)                          [归属 sub-reactor Ti]
   │   │           ├── Eventloop *loop_  ╌╌╌  (borrowed sub-reactor)
   │   │           ├── std::unique_ptr<Socket> sock_  (client fd)
   │   │           ├── std::unique_ptr<Channel> channel_
   │   │           ├── Buffer inputBuffer_   (类内成员，非指针)
   │   │           ├── Buffer outputBuffer_
   │   │           ├── std::any context_  ← 上层协议挂载点 (HttpContext 实例放这里)
   │   │           ├── std::shared_ptr<bool> alive_ ← 析构时置 false
   │   │           ├── TimeStamp lastActive_
   │   │           └── std::function<void(Connection*)> onMessageCallback_
   │   │                  ╌╌╌→ HttpServer::onMessage  (借用 HttpServer 实例)
   │   │
   │   ├── std::vector<Middleware> middlewares_                        [T0]
   │   ├── std::unordered_map<string,RouteHandler> routes_             [T0]
   │   └── HttpCallback httpCallback_                                  [T0]
   │
   ├── AsyncLogging asyncLog                                          [T0 构造]
   │   ├── std::unique_ptr<Buffer> current_  (4MB 大缓冲)
   │   ├── std::unique_ptr<Buffer> next_
   │   ├── std::vector<std::unique_ptr<Buffer>> buffers_   ← 已满待刷的
   │   ├── std::mutex mutex_   ← 保护 current_/next_/buffers_ 的交接
   │   ├── std::condition_variable cv_
   │   └── std::thread thread_   ── 后端写线程 [T_log]
   │
   └── 各类 stack-only 工具：RateLimiter / CorsMiddleware / GzipMiddleware / AuthMiddleware
       └── 它们的 toMiddleware() 返回的 Middleware (std::function) 被复制进
           HttpServer::middlewares_，原对象生命周期需 ≥ srv 即可
```

### 2.2 关键所有权规律

1. **所有权链是一棵严格的树**：每个对象有且只有一个 `unique_ptr` 持有方。`shared_ptr` 在本项目中只有一个用途——`Connection::alive_`，而且持有方仅 1 个（Connection 自己），weak_ptr 由定时器闭包持有用于"判活"，不参与生命周期管理。
2. **横向引用全部用裸指针 / weak_ptr，永远向上游借**：例如 `EventLoopThreadPool::loops_` 是 `vector<Eventloop*>`，不持有；`Connection::loop_` 是裸指针，借自 sub-reactor。这种"向上游借"模式保证了**析构顺序天然安全**——只要上游晚于下游析构。
3. **map<fd, unique_ptr<Connection>> 是单线程数据结构**：`TcpServer::connections_` 的所有 insert / erase 都必须发生在 main reactor (T0) 线程。任何 sub-reactor 想触发 erase 都必须 `mainReactor_->queueInLoop(...)` 切换到 T0。

### 2.3 析构顺序（C++ 成员按声明逆序析构）

`TcpServer` 成员声明顺序：
```
mainReactor_  → acceptor_  → threadPool_  → connections_
   (1)            (2)           (3)            (4)
```
逆序析构得到：`connections_(1st) → threadPool_(2nd) → acceptor_(3rd) → mainReactor_(4th)`。

但注意 `~Connection()` 内部要调 `loop_->deleteChannel(channel_)`，要求 EventLoop 仍然存活且 IO 线程已退出（避免与 `poll()` 中的 events_ 缓存竞态）。所以 `~TcpServer()` 必须**在成员析构开始前**手动做两件事：

来自 [HISTORY/day30/src/common/net/TcpServer.cpp](../HISTORY/day30/src/common/net/TcpServer.cpp)：

```cpp
TcpServer::~TcpServer() {
    // 1. 令所有 reactor 退出 loop()（幂等，重复调用无害）
    stop();
    // 2. 显式等待所有 IO 线程实际退出（join），
    //    之后 connections_ 析构时调用 loop->deleteChannel() 才没有竞态风险
    threadPool_->joinAll();
    // 3. 成员按声明逆序自动析构（详见 TcpServer.h 注释）
}
```

这是 day27 那次 KqueuePoller UAF 的根因修复。详细见 §9.1。

---

## 3. 启动顺序：从 `main()` 到 `mainReactor_->loop()` 的每一步

### 3.1 时序总览

下面是 `examples/src/http_server.cpp` 调用 `srv.start()` 之前 + `start()` 内部，按时间从上到下发生的所有事件。"线程"列表示这一步运行在哪个线程。

| 时刻 | 线程 | 动作 | 关键状态 / 副作用 |
|---|---|---|---|
| T0.0 | T0 | `AsyncLogging asyncLog("airi_server")` 构造 | 仅设置成员，未启线程 |
| T0.1 | T0 | `asyncLog.start()` | 启动后端线程 [T_log]，等 latch 后端就绪后返回 |
| T0.2 | T0 | `Logger::setOutput([&]{ asyncLog.append(...) })` | 全局回调指针指向 asyncLog |
| T0.3 | T0 | `HttpServer srv(options)` 构造 | 内部 → `TcpServer` 构造 |
| T0.4 | T0 | ↳ `mainReactor_ = make_unique<Eventloop>()` | 构造主 EventLoop（poller、wakeupFd、timerQueue） |
| T0.5 | T0 | ↳ `acceptor_ = make_unique<Acceptor>(mainReactor_, "0.0.0.0", 18888)` | `socket()` + `bind()` + `listen()` + 加入 mainReactor 的 poller |
| T0.6 | T0 | ↳ `acceptor_->setNewConnectionCallback(&TcpServer::newConnection)` | 注册"新连接到达"回调 |
| T0.7 | T0 | ↳ `threadPool_ = make_unique<EventLoopThreadPool>(mainReactor_)`，`setThreadNums(N)` | 仅设置数字，未启线程 |
| T0.8 | T0 | `srv.use(...)` × 5 | middlewares_ vector push_back 5 个 std::function |
| T0.9 | T0 | `srv.addRoute(...)`、`addPrefixRoute(...)` | routes_ map / prefixRoutes_ vector 填充 |
| T0.10 | T0 | `Signal::signal(SIGINT, [&]{ srv.stop(); })` | 全局信号表注册 |
| T0.11 | T0 | `srv.start()` → `server_->Start()` | ↓ 进入下面 11 步 |
| T1.0 | T0 | ↳ `threadPool_->start()` | for i = 0..N-1： |
| T1.1 | T0 | ↳↳ `EventLoopThread t; t.startLoop()` | 启动线程 Ti，**同步等待** Ti 内 EventLoop 就绪 |
| T1.2 | Ti | ↳↳↳ Ti 进入 `threadFunc()`：`loop_ = make_unique<Eventloop>()` | sub-reactor 在 Ti 线程内构造 → tid_=Ti |
| T1.3 | Ti | ↳↳↳ `cv_.notify_one()` 通知 T0 | T0 的 startLoop() 返回 loop_.get() |
| T1.4 | Ti | ↳↳↳ `loop_->loop()` 进入 sub-reactor 主循环 | 阻塞在 `kevent()` |
| T1.5 | T0 | 回到 T0：`loops_.push_back(loop)` | T0 持有 N 个 sub-reactor 的裸指针 |
| T2.0 | T0 | ↳ `mainReactor_->loop()` | 主线程进入 main-reactor 主循环 |
| T2.1 | T0 | ↳↳ 阻塞在 `kevent()` 等待新连接 | listen fd 在 poller 中 |

### 3.2 逐函数代码追踪

#### 第 1 步：`AsyncLogging::start()` — 启动日志后端线程

来自 [HISTORY/day30/src/common/log/AsyncLogging.cpp](../HISTORY/day30/src/common/log/AsyncLogging.cpp)：

```cpp
void AsyncLogging::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&AsyncLogging::threadFunc, this);
    latch_.wait();  // 等后端 threadFunc 内 latch_.countDown() 之后才返回
}
```

**为什么要 latch**：保证 `start()` 返回时后端线程已经初始化好 LogFile，第一条 `LOG_INFO` 进来时不会丢日志/竞态写文件。

#### 第 2 步：`Eventloop::Eventloop()` — 主 reactor 构造

来自 [HISTORY/day30/src/common/net/Eventloop.cpp](../HISTORY/day30/src/common/net/Eventloop.cpp)：

```cpp
Eventloop::Eventloop() : poller_(nullptr), quit_(false), tid_(std::this_thread::get_id()) {
    poller_ = Poller::newDefaultPoller(this);  // 工厂返回 KqueuePoller / EpollPoller
    timerQueue_ = std::make_unique<TimerQueue>(this);
#ifdef __APPLE__
    pipe(pipeFds);
    wakeupReadFd_ = pipeFds[0];
    wakeupWriteFd_ = pipeFds[1];
    fcntl(wakeupReadFd_,  F_SETFL, O_NONBLOCK);
    fcntl(wakeupWriteFd_, F_SETFL, O_NONBLOCK);
    evtChannel_ = std::make_unique<Channel>(this, wakeupReadFd_);
#endif
    evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));
    evtChannel_->enableReading();   // 把 wakeup 端注册到 poller 中
}
```

**代入实参（mainReactor 构造时刻）**：
- `tid_` = T0（main thread id）
- `poller_` = 新 KqueuePoller，内部 `kqueue()` 系统调用 → 内核分配 kqueueFd_
- `pipe(pipeFds)` → 内核分配两个 fd（macOS：管道两端；Linux：1 个 eventfd）
- `evtChannel_->enableReading()` → `loop_->updateChannel(this)` → `poller_->updateChannel(this)` → `kevent(kqueueFd_, EV_ADD|EVFILT_READ for wakeupReadFd_)` 一次 syscall

**此刻全局状态**：
```
mainReactor_.tid_              = T0
mainReactor_.poller_.kqueueFd_ = 7（示意）
mainReactor_.wakeupReadFd_     = 5
mainReactor_.wakeupWriteFd_    = 6
kernel kqueue 关注列表           = { fd=5 EVFILT_READ udata=evtChannel_ }
```

#### 第 3 步：`Acceptor::Acceptor()` — listen socket 准备

来自 [HISTORY/day30/src/common/net/Acceptor.cpp](../HISTORY/day30/src/common/net/Acceptor.cpp)：

```cpp
Acceptor::Acceptor(Eventloop *loop, const char *ip, uint16_t port) {
    sock_ = std::make_unique<Socket>();             // ① socket(AF_INET, SOCK_STREAM, 0)
    InetAddress addr(ip, port);
    setsockopt(sock_->getFd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sock_->bind(&addr);                              // ② bind()
    sock_->listen();                                 // ③ listen(SOMAXCONN)
    sock_->setnonblocking();                         // ④ fcntl(F_SETFL, O_NONBLOCK)
    acceptChannel_ = std::make_unique<Channel>(loop, sock_->getFd());
    acceptChannel_->setReadCallback(std::bind(&Acceptor::acceptConnection, this));
    acceptChannel_->enableReading();                 // ⑤ kevent(EV_ADD|EVFILT_READ for listen fd)
    // 注意：Acceptor 不使用 ET 模式，避免多个连接同时到达时只 accept 一次导致丢连接
}
```

**5 次 syscall 全部发生在 T0**：`socket → bind → listen → fcntl → kevent`。
**此刻全局状态**：
```
listen fd = 8
kernel kqueue 关注列表 = {
    fd=5 EVFILT_READ udata=evtChannel_,
    fd=8 EVFILT_READ udata=acceptChannel_
}
TcpServer 自身还没注册"新连接回调到 mainReactor_"，因为 Acceptor 构造完后
TcpServer 才会调 `acceptor_->setNewConnectionCallback(&TcpServer::newConnection)`。
```

#### 第 4 步：`EventLoopThreadPool::start()` — 拉起 N 个 sub-reactor

来自 [HISTORY/day30/src/common/net/EventLoopThreadPool.cpp](../HISTORY/day30/src/common/net/EventLoopThreadPool.cpp)：

```cpp
void EventLoopThreadPool::start() {
    for (int i = 0; i < threadNums_; ++i) {
        auto t = std::make_unique<EventLoopThread>();
        loops_.push_back(t->startLoop());   // 同步等待 sub-reactor 就绪
        threads_.push_back(std::move(t));
    }
}
```

来自 [HISTORY/day30/src/common/net/EventLoopThread.cpp](../HISTORY/day30/src/common/net/EventLoopThread.cpp)：

```cpp
Eventloop *EventLoopThread::startLoop() {
    thread_ = std::thread(&EventLoopThread::threadFunc, this);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return loopReady_; });
    }
    return loop_.get();
}

void EventLoopThread::threadFunc() {
    loop_ = std::make_unique<Eventloop>();   // ★ 构造发生在 Ti 线程
    {
        std::unique_lock<std::mutex> lock(mutex_);
        loopReady_ = true;
        cv_.notify_one();                    // 唤醒 startLoop()
    }
    loop_->loop();                           // 阻塞到 setQuit() + wakeup()
}
```

**关键点**：`loop_` 必须在 Ti 线程内构造，否则 `Eventloop::tid_` 会捕获错误线程 ID，后续 `isInLoopThread()` 永远返回 false → 所有 `runInLoop` 退化成 `queueInLoop`，性能崩塌且语义出错。

**N=4（4 个 sub-reactor）执行完后的全局状态**：
```
活跃线程：T0, T1, T2, T3, T4, T_log
每个 sub-reactor Ti（i=1..4）独立持有：
   poller_.kqueueFd_  = 各自 1 个
   wakeupReadFd_/wakeupWriteFd_ = 各自 1 对 pipe
   kernel kqueue 关注列表 = { wakeupReadFd_ }（暂无 client）
mainReactor 的 kqueue 关注列表 = { wakeupReadFd_, listen fd=8 }
```

#### 第 5 步：`mainReactor_->loop()` — main 线程进入永久循环

来自 [HISTORY/day30/src/common/net/Eventloop.cpp](../HISTORY/day30/src/common/net/Eventloop.cpp)：

```cpp
void Eventloop::loop() {
    while (!quit_) {
        int timeout = timerQueue_->nextTimeoutMs();   // -1 / 0 / >0
        std::vector<Channel *> channels = poller_->poll(timeout);
        for (auto *ch : channels)
            ch->handleEvent();
        timerQueue_->processExpiredTimers();
        doPendingFunctors();
    }
}
```

**此时所有 5+1 个线程都在 `kevent()` 上阻塞**，CPU 占用 ~0%。系统进入"待客状态"。`Start()` 函数体此后不会再返回，直到收到 `SIGINT` → `srv.stop()` → `mainReactor_->setQuit() + wakeup()` → loop() 退出 → `Start()` 返回 → `main()` 继续。

### 3.3 启动序的"为什么必须这样"

| 选择 | 替代方案 | 为什么不行 |
|---|---|---|
| sub-reactor 先全部 `startLoop()` 完再 `mainReactor_->loop()` | 同时启动 | accept 到的新连接要 `nextLoop()` 选 sub-reactor，必须保证 `loops_` 已经填好 |
| `loop_` 在 Ti 线程内构造而非 T0 构造完再传给 Ti | T0 构造、Ti 跑 loop | `tid_` 会捕获 T0，`isInLoopThread()` 永错 |
| `startLoop()` 用条件变量等待 `loopReady_` | 不等待，直接返回 | T0 拿到的 `loops_[i]` 可能是 nullptr（Ti 还没构造完） |
| `latch_` 等 AsyncLogging 后端就绪 | 不等待 | 第一条日志可能在文件未打开时丢失 |

---

## 4. 场景 A — 一次完整的连接建立

### 4.1 业务场景

> 用户在浏览器输入 `http://127.0.0.1:18888/`。OS 完成三次握手，listen fd 上 readable。我们要做的是：accept 出 client fd → 选一个 sub-reactor → 在该 sub-reactor 上启用 client fd 的读事件 → 但 connections_ map 的 insert 必须发生在 main reactor 线程。

### 4.2 调用链总览（树形 + 关键代码）

> 设当前 4 个 sub-reactor T1..T4，已有 3 个连接挂在 T1/T2/T3，下一个 round-robin 应该轮到 T4。

#### 整体调用树

调用链横跨两个线程；中间通过 `queueInLoop + wakeup pipe` 跳跃，用 `═══════ wakeup ═══════►` 表示跨线程传递点。

```
┌───────────────────────── T0 (mainReactor) ─────────────────────────┐
│                                                                    │
│  Eventloop::loop()                                                 │
│    └─ poller_->poll()           // 阻塞在 kevent()                 │
│         ▲ listen fd readable    // OS 三次握手完成                 │
│    └─ ch->handleEvent()         // ch = acceptChannel_             │
│         └─ readCallback()       // = Acceptor::acceptConnection    │
│              └─ Acceptor::acceptConnection()                       │
│                   ├─ ::accept(listen_fd) ──► fd=42                 │
│                   ├─ fcntl(42, O_NONBLOCK)                         │
│                   └─ newConnectionCallback_(42)                    │
│                        └─ TcpServer::newConnection(42)             │
│                             ├─ subLoop = threadPool_->nextLoop()   │
│                             │     // round-robin → T4              │
│                             ├─ make_unique<Connection>(42, subLoop)│
│                             │     └─ Channel ctor: setReadCallback │
│                             │           = Connection::doRead       │
│                             ├─ conn->setOnMessageCallback(...)     │
│                             │     └─ ★ readCallback 被覆盖为       │
│                             │           Connection::Business       │
│                             ├─ conn->setDeleteConnectionCallback() │
│                             ├─ connections_[42] = std::move(conn)  │
│                             │     // ★ map insert 仅在 T0 发生     │
│                             ├─ newConnectCallback_(raw)            │
│                             │     └─ HttpServer::onNewConnection   │
│                             │          ├─ conn->setContext(...)    │
│                             │          └─ conn->touchLastActive()  │
│                             └─ subLoop->queueInLoop(               │
│                                    [raw]{ raw->enableInLoop(); })  │
│                                  └─ pendingFunctors_.emplace_back  │
│                                  └─ wakeup()                       │
│                                       └─ write(T4.wakeupWriteFd,1) ═══ wakeup ═══┐
│  回到 poller_->poll() 阻塞                                                       │
└──────────────────────────────────────────────────────────────────────────────────┘
                                                                                  │
┌──────────────────────── T4 (sub-reactor) ────────────────────────────────────────┘
│                                                                    
│  Eventloop::loop()           // 一直阻塞在 kevent()
│    └─ poller_->poll()        // wakeupReadFd 可读 → 返回
│    └─ ch->handleEvent()      // ch = evtChannel_ (wakeup channel)
│         └─ readCallback()    // = Eventloop::handleWakeup
│              └─ read(wakeupReadFd_) 排空
│    └─ doPendingFunctors()
│         └─ functor()         // 即 [raw]{ raw->enableInLoop(); }
│              └─ Connection::enableInLoop()
│                   ├─ channel_->enableReading()
│                   │    └─ loop_->updateChannel(channel_)
│                   │         └─ poller_->updateChannel(channel_)
│                   │              └─ kevent(EV_ADD|EVFILT_READ,
│                   │                        udata=channel_)
│                   └─ channel_->enableET()  // kqueue 实际不区分 ET
│  回到 poller_->poll() 阻塞   // 此时 fd=42 已挂在 T4 的 kqueue 上
└────────────────────────────────────────────────────────────────────
```

#### 关键代码块

**① 跨线程跳板 —— `Eventloop::queueInLoop`**

```cpp
void Eventloop::queueInLoop(std::function<void()> func) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(func)); // ★ move，防形参共享
    }
    wakeup();   // write(wakeupWriteFd_, "w", 1) → 唤醒 T4
}
```

**② 回调"两步注入"—— `Connection` 构造 + `setOnMessageCallback`**

```cpp
Connection::Connection(int fd, Eventloop *loop) ... {
    channel_ = std::make_unique<Channel>(loop_, fd);
    channel_->setReadCallback(std::bind(&Connection::doRead, this));   // 第一次：仅读 IO
    ...
}

void Connection::setOnMessageCallback(std::function<void(Connection*)> const &cb) {
    onMessageCallback_ = cb;
    channel_->setReadCallback(std::bind(&Connection::Business, this)); // ★ 覆盖为 Business
}
```

> 这是全项目最容易踩坑的"隐式副作用"：`setOnMessageCallback` 不仅存了 `cb`，还顺手改写了 channel 的读回调。IDE 跳转无法揭示这一步——只能靠人脑记住。

**③ 把 fd 挂上 sub-reactor —— `Connection::enableInLoop`**

```cpp
void Connection::enableInLoop() {
    channel_->enableReading();   // → updateChannel → kevent(EV_ADD|EVFILT_READ)
    channel_->enableET();
}
```

#### 本场景为什么必须跨线程

| 操作 | 必须的执行线程 | 原因 |
|---|---|---|
| `connections_[fd] = ...` | T0（mainReactor 单线程） | map 不加锁，且全部增删都汇聚到 T0 |
| `kevent(EV_ADD, ...)` | fd 归属线程 T4 | kqueue/epoll 跨线程并发改 fd 是 UB |
| `setOnMessageCallback` | T0（连接尚未 enable） | 此时 T4 还看不见 fd，不存在并发读 channel 的可能 |

### 4.3 逐函数代码追踪

#### 第 1 步：listen fd 上有数据，T0 的 poller 返回

来自 [HISTORY/day30/src/common/net/Poller/kqueue/KqueuePoller.cpp](../HISTORY/day30/src/common/net/Poller/kqueue/KqueuePoller.cpp)：

```cpp
std::vector<Channel *> KqueuePoller::poll(int timeout) {
    int nfds = kevent(kqueueFd_, nullptr, 0, events_.data(), events_.size(), pts);
    // ...
    std::unordered_map<Channel *, int> channelEvents;
    for (int i = 0; i < nfds; ++i) {
        Channel *ch = static_cast<Channel *>(events_[i].udata);
        int readyEv = 0;
        if (events_[i].filter == EVFILT_READ)  readyEv |= Channel::READ_EVENT;
        if (events_[i].filter == EVFILT_WRITE) readyEv |= Channel::WRITE_EVENT;
        channelEvents[ch] |= readyEv;
    }
    for (auto &[ch, ev] : channelEvents) {
        ch->setReadyEvents(ev);
        activeChannels.push_back(ch);
    }
    return activeChannels;
}
```

**代入实参**：`nfds = 1`，`events_[0] = { ident=8, filter=EVFILT_READ, udata=acceptChannel_ }`。
**返回值**：`activeChannels = { acceptChannel_ }`，且 `acceptChannel_->ready_events_ = READ_EVENT`。
**syscall**：`kevent()` 1 次。

#### 第 2 步：`Channel::handleEvent()` 分发

来自 [HISTORY/day30/src/common/net/Channel.cpp](../HISTORY/day30/src/common/net/Channel.cpp)：

```cpp
void Channel::handleEvent() {
    if (ready_events_ & READ_EVENT) {
        if (readCallback) readCallback();
    }
    if (ready_events_ & WRITE_EVENT) {
        if (writeCallback) writeCallback();
    }
}
```

`readCallback` 是 `Acceptor` 在构造时塞进去的 `std::bind(&Acceptor::acceptConnection, this)`。

#### 第 3 步：`Acceptor::acceptConnection()`

```cpp
void Acceptor::acceptConnection() {
    InetAddress clientAddr;
    int clientFd = sock_->accept(&clientAddr);   // ::accept() syscall
    if (clientFd == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
            errno == ECONNABORTED || errno == EPROTO)
            return;
        LOG_ERROR << "[Acceptor] accept 失败...";
        return;
    }
    int oldFlags = fcntl(clientFd, F_GETFL);
    if (oldFlags != -1)
        fcntl(clientFd, F_SETFL, oldFlags | O_NONBLOCK);   // 2 次 syscall
    if (newConnectionCallback_)
        newConnectionCallback_(clientFd);
}
```

**syscall**：`accept()` + `fcntl(F_GETFL)` + `fcntl(F_SETFL)` = 3 次。
**注意**：本函数**不**遍历 accept 直到 `EAGAIN`。原因写在源码注释："Acceptor 不使用 ET 模式，避免多个连接同时到达时只 accept 一次导致丢连接"——LT 模式下 listen fd 仍会反复就绪，靠下次 `loop()` 迭代来 accept 后续连接。这是有意的简化，吞吐换正确性。

#### 第 4 步：`TcpServer::newConnection(fd=42)`

```cpp
void TcpServer::newConnection(int fd) {
    if (fd == -1) { return; }
    if (shouldRejectNewConnection(connections_.size(), maxConnections_)) {
        ::close(fd); return;
    }
    Eventloop *subLoop = threadPool_->nextLoop();   // 轮询拿 T4 的 loop
    auto conn = std::make_unique<Connection>(fd, subLoop);

    conn->setOnMessageCallback(onMessageCallback_);
    conn->setDeleteConnectionCallback(
        std::bind(&TcpServer::deleteConnection, this, std::placeholders::_1));

    Connection *rawConn = conn.get();
    connections_[fd] = std::move(conn);             // ← T0 单独操作 map

    if (newConnectCallback_)
        newConnectCallback_(rawConn);               // ← HttpServer::onNewConnection

    subLoop->queueInLoop([rawConn]() { rawConn->enableInLoop(); });
}
```

**关键设计点**（必须按这个顺序）：
1. **先回调，后 enableInLoop**：如果反过来，T4 可能瞬间收到客户端发的请求 → onMessageCallback_ 还没设置 → segfault。
2. **map insert 发生在 newConnectCallback_ 之前**：保证 onNewConnection 调到 `conn->setContext(...)` 时 conn 已在 map 里、生命周期被持有。
3. **enableInLoop 通过 queueInLoop 跨线程投递**：因为 `Channel::enableReading` 内部要调 `poller_->updateChannel` → 必须在 poller 归属线程（T4）执行，否则 kqueue 是非线程安全的。

#### 第 5 步：`Eventloop::queueInLoop` 跨线程投递

来自 [HISTORY/day30/src/common/net/Eventloop.cpp](../HISTORY/day30/src/common/net/Eventloop.cpp)：

```cpp
void Eventloop::queueInLoop(std::function<void()> func) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 必须 std::move：若按 const ref 形式 emplace_back(func)，会与形参 func
        // 共享 std::function 的内部小对象/引用计数...
        pendingFunctors_.emplace_back(std::move(func));
    }
    wakeup();
}
```

`wakeup()` 在 macOS 下：

```cpp
char buf = 'w';
write(wakeupWriteFd_, &buf, 1);   // 1 次 syscall
```

T4 阻塞在 `kevent()`，pipe 写入触发 wakeup 端可读 → kevent 返回 → loop() 推进。

#### 第 6 步：T4 唤醒后 `doPendingFunctors()`

```cpp
void Eventloop::doPendingFunctors() {
    std::vector<std::function<void()>> functors;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);   // ★ swap 出来再执行，临界区极短
    }
    for (const auto &func : functors)
        func();
}
```

**为什么先 swap 再执行**：func 内部可能再次调 `queueInLoop()`（典型场景：HttpServer::scheduleIdleClose 在定时器回调里再 schedule 下一次定时器）。如果不 swap，临界区会嵌套获取 mutex，死锁。

#### 第 7 步：`Connection::enableInLoop()` 真正激活 fd

来自 [HISTORY/day30/src/common/net/Connection.cpp](../HISTORY/day30/src/common/net/Connection.cpp)：

```cpp
void Connection::enableInLoop() {
    channel_->enableReading();
    channel_->enableET();
}
```

`enableReading()` → `Channel::enableReading()` → `loop_->updateChannel(this)` → `poller_->updateChannel(this)` → `kevent(EV_ADD|EVFILT_READ, udata=channel_)`。**1 次 syscall**。

### 4.4 本场景 ASCII 序列图

```
   T0 (main reactor)                 T4 (sub reactor)              kernel
       │                                  │                           │
       │  阻塞在 kevent()                 │  阻塞在 kevent()          │
       │                                  │                           │
       │  ◀── listen fd readable ──────────────────────────── 三次握手完成
       │                                  │                           │
       ├─ acceptConnection                │                           │
       │     accept(listen_fd) ──────────────────────────────▶ client fd=42
       │     fcntl(42, NONBLOCK) ────────────────────────────▶
       │                                  │                           │
       ├─ TcpServer::newConnection(42)    │                           │
       │     subLoop = T4.loop            │                           │
       │     new Connection(42, T4.loop)  │                           │
       │     connections_[42] = move(conn)│                           │
       │     onNewConnection(conn)        │                           │
       │       setContext(HttpContext)    │                           │
       │     T4.queueInLoop([rawConn]{    │                           │
       │       rawConn->enableInLoop();   │                           │
       │     })                           │                           │
       │     T4.wakeup() ────write(pipe)──┼──▶  pipe 内核写入        │
       │                                  ◀── pipe 可读 ─────────────│
       │  回到 kevent()                   ├─ handleWakeup → read(pipe)│
       │                                  ├─ doPendingFunctors:       │
       │                                  │     enableInLoop()        │
       │                                  │       kevent(EV_ADD,fd=42)│
       │                                  │                           │
       │                                  │  回到 kevent()            │
```

### 4.5 本场景涉及函数职责一句话表

| 函数 | 调用时机 | 职责 |
|---|---|---|
| `KqueuePoller::poll` | EventLoop::loop 每次迭代 | 阻塞在 kevent，返回活跃 Channel 列表 |
| `Channel::handleEvent` | poll 返回后 | 按 ready_events_ 触发 readCallback / writeCallback |
| `Acceptor::acceptConnection` | listen fd 可读时 | accept 出 client fd，设非阻塞，回调 TcpServer |
| `TcpServer::newConnection` | accept 成功后 | 建 Connection，注入回调，map insert，跨线程 enableInLoop |
| `EventLoopThreadPool::nextLoop` | 选 sub-reactor 时 | round-robin 返回一个 sub-reactor |
| `HttpServer::onNewConnection` | newConnectCallback_ 触发 | 把 HttpContext 挂到 conn->context_ |
| `Eventloop::queueInLoop` | 跨线程投递任务时 | 推入 pendingFunctors_ 并 wakeup 目标线程 |
| `Eventloop::wakeup` | queueInLoop 末尾 | write 1 字节到 pipe / eventfd 唤醒 poll |
| `Eventloop::handleWakeup` | wakeup pipe 可读时 | read 排空 pipe（不做其他事） |
| `Eventloop::doPendingFunctors` | poll 返回后 | swap 出待执行任务列表，逐个执行 |
| `Connection::enableInLoop` | sub-reactor 唤醒后 | 让 fd 在 sub-reactor 的 poller 中开始监听读事件 |

---

## 5. 场景 B — 一次完整的请求/响应

### 5.1 业务场景

> 客户端发送 `GET /api/users HTTP/1.1\r\nHost:...\r\nAuthorization: Bearer demo-token-2024\r\n\r\n`，期望 200 + JSON body。整个请求 132 字节。响应 body 102 字节、加 headers ~250 字节。**全过程不触发回压、不触发 sendfile**，是最常见的"小请求小响应"路径。

### 5.2 调用链总览（树形 + 关键代码）

> 整条链全部发生在 T4 一个线程内（请求小、响应小、不触发回压、不触发 sendfile）。**没有任何跨线程跳转**，因此没有 wakeup pipe，也没有 queueInLoop。

#### 整体调用树

```
T4 (sub-reactor)
│
│  Eventloop::loop()
│    └─ poller_->poll()                  // 阻塞在 kevent()
│         ▲ fd=42 readable               // 客户端 132 字节请求到达
│    └─ ch->handleEvent()                // ch = connection.channel_
│         └─ readCallback()              // = Connection::Business
│              │   ★（本来构造时是 doRead，被 setOnMessageCallback 改写）
│              └─ Connection::Business()
│                   ├─ if (state_ != kConnected) return;     // 状态守卫
│                   ├─ Connection::doRead()
│                   │    └─ while (true) {                   // ET 循环
│                   │         n = inputBuffer_.readFd(42)    // readv syscall
│                   │         if (n>0) continue;
│                   │         if (n==0) { state=kClosed; break; }
│                   │         if (errno==EAGAIN) break;      // 正常退出
│                   │       }
│                   │   // 出循环：inputBuffer_ 拥有 132 字节
│                   └─ if (state_ == kConnected)
│                        └─ onMessageCallback_(this)          // = HttpServer::onMessage
│                             └─ HttpServer::onMessage(conn)
│                                  └─ while (有未消耗字节) {
│                                       parser.parse(buf->peek(), n, &consumed)
│                                       buf->retrieve(consumed)
│                                       if (parser.isComplete())
│                                          └─ HttpServer::onRequest(conn, req)
│                                               ├─ LogContext::Guard 入栈
│                                               ├─ 构建 dispatch lambda
│                                               ├─ 构建洋葱 runChain：
│                                               │    AccessLog → RateLimiter
│                                               │      → CORS → Auth → Gzip
│                                               │      → dispatch
│                                               │         └─ handleApiUsers(req,&resp)
│                                               │              └─ resp.setBody(json)
│                                               │   ★ 业务在此同步运行于 T4
│                                               ├─ runChain()                    // 同步链
│                                               └─ conn->send(resp.serialize())
│                                                    └─ Connection::send(string&&)
│                                                         ├─ canDirectWrite ✓
│                                                         ├─ ::write(42,buf,250) // 1 syscall
│                                                         └─ remaining==0 → 直接返回
│                                       parser.reset();      // 准备下个请求
│                                     }
│                   // Business() 末尾 state_ 仍为 kConnected → 不调 close
│  回到 poller_->poll() 阻塞              // 等下次请求或 keep-alive 超时
└──────────────────────────────────────────────────────────
```

#### 关键代码块

**① 入口分发 —— `Channel::handleEvent`（与场景 A 相同）**

```cpp
void Channel::handleEvent() {
    if (ready_events_ & READ_EVENT && readCallback) readCallback();
    if (ready_events_ & WRITE_EVENT && writeCallback) writeCallback();
}
```

> 同一个 `readCallback` 槽，在场景 A 里指向 `Acceptor::acceptConnection`，在场景 B 里指向 `Connection::Business`——这就是"回调注入"模式的好处。

**② IO 与业务编排 —— `Connection::Business`**

```cpp
void Connection::Business() {
    if (state_ != State::kConnected) return;   // ★ 状态守卫，防同帧 read+write 重入
    doRead();
    if (state_ == State::kConnected) {
        if (onMessageCallback_) onMessageCallback_(this);   // 业务回调
    } else {
        close();    // 统一在所有读完成后才触发删除
    }
}
```

**③ HTTP 状态机 + 粘包/Pipeline —— `HttpServer::onMessage`**

```cpp
void HttpServer::onMessage(Connection *conn) {
    auto *ctx = conn->getContextAs<HttpConnectionContext>();
    Buffer *buf = conn->getInputBuffer();
    while (buf->readableBytes() > 0) {
        size_t consumed = 0;
        bool ok = ctx->parser.parse(buf->peek(), buf->readableBytes(), &consumed);
        buf->retrieve(consumed);
        if (!ok) { conn->close(); return; }
        if (!ctx->parser.isComplete()) break;        // 半包：等下次
        onRequest(conn, ctx->parser.request());      // 完整请求 → 派发
        ctx->parser.reset();                         // 复位，处理下一个 pipeline 请求
    }
}
```

**④ 业务执行点 —— `HttpServer::onRequest`**

```cpp
void HttpServer::onRequest(Connection *conn, const HttpRequest &req) {
    LogContext::Guard g(makeReqId());
    HttpResponse resp;
    auto dispatch = [&](const HttpRequest &r, HttpResponse *p, NextFn) {
        auto it = routes_.find(routeKey(r));
        if (it != routes_.end()) it->second(r, p);   // ★ 业务函数同步执行于 T4
        else { p->setStatus(404, "Not Found"); }
    };
    NextFn chain = buildOnion(middlewares_, dispatch);  // 洋葱组装
    chain(req, &resp, [](){});                          // 同步跑完整条链
    conn->send(resp.serialize());                       // 直写 socket
    if (closeConnection(req, resp)) conn->close();
}
```

> 这一行 `it->second(r, p)` 就是**用户业务的真正落点**。如果 `handleApiUsers` 内部 `sleep(5)`，T4 上挂着的所有连接全部停摆 5 秒。

**⑤ 零拷贝直写 —— `Connection::send(std::string&&)`**

```cpp
void Connection::send(std::string &&msg) {
    bool canDirectWrite = !channel_->isWriting() && outputBuffer_.readableBytes() == 0;
    if (canDirectWrite) {
        ssize_t n = ::write(sock_->getFd(), msg.data(), msg.size());
        if (n == (ssize_t)msg.size()) return;       // ★ 全写完，msg 直接析构
        // ...写不完才落入 outputBuffer_ + EPOLLOUT 续传
    }
    // ...
}
```

#### 本场景的执行模型一句话

> **整条链都跑在 T4 一个线程里**，没有任何 mutex、没有任何 wakeup、没有任何 queueInLoop——这就是 one-loop-per-thread 的本意。代价是：业务函数不能阻塞，否则同一 sub-reactor 上挂的所有连接同时受影响。

### 5.3 逐函数代码追踪

#### 第 1 步：T4 的 poll 返回 client fd 的读事件

同 §4.3 第 1 步，只是这次 `events_[0] = { ident=42, udata=connection.channel_ }`。

#### 第 2 步：`Channel::handleEvent()` → `Connection::Business`

注意 `Connection::setOnMessageCallback` 内部偷偷换了 readCallback：

```cpp
void Connection::setOnMessageCallback(std::function<void(Connection *)> const &cb) {
    onMessageCallback_ = cb;
    channel_->setReadCallback(std::bind(&Connection::Business, this));
}
```

所以 `channel_->handleEvent()` 的 readCallback 实际是 `Connection::Business`，**不是 doRead**。

#### 第 3 步：`Connection::Business()` — IO 与业务的统一编排

```cpp
void Connection::Business() {
    // 状态守卫：kqueue/epoll 的同一批次事件中，同一 fd 可能出现多次
    if (state_ != State::kConnected)
        return;

    doRead();
    if (state_ == State::kConnected) {
        if (onMessageCallback_)
            onMessageCallback_(this);
    } else {
        close();
    }
}
```

**为什么把 close() 放在最后**：详见源码注释——`close()` 内部最终会 `queueInLoop(delete raw)` 投递析构。如果 `close()` 在 `onMessageCallback_(this)` 之前调用，main reactor 可能瞬间删掉本对象（罕见但理论可能），onMessageCallback_ 内对 `this` 的访问就成了 UAF。

#### 第 4 步：`Connection::doRead()` — 循环读到 EAGAIN

```cpp
void Connection::doRead() {
    int sockfd = sock_->getFd();
    while (true) {
        int savedErrno = 0;
        ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);
        if (n > 0) {
            continue;
        } else if (n == 0) {
            state_ = State::kClosed;
            // 不在此处调用 close()，由 Business() 在所有回调完成后统一触发
            break;
        } else {
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
                break;
            state_ = State::kFailed;
            break;
        }
    }
}
```

**ET 模式必须循环读**——这是源码注释强调的。第一次 `readv()` 可能只读到 4KB（因为 kernel 把 4KB 标记为可读），第二次 `readv()` 真正读到剩余字节，第三次 `readv()` 返回 -1/EAGAIN，循环退出。

来自 [HISTORY/day30/src/common/net/Buffer.cpp](../HISTORY/day30/src/common/net/Buffer.cpp)：

```cpp
ssize_t Buffer::readFd(int fd, int *savedErrno) {
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t writable = writableBytes();
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len  = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len  = sizeof(extrabuf);
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0)                       *savedErrno = errno;
    else if ((size_t)n < writable)   writerIndex_ += n;
    else { writerIndex_ = buffer_.size(); append(extrabuf, n - writable); }
    return n;
}
```

**spread io 设计精髓**：vec[0] 指向 Buffer 内剩余空间（默认 1024 起），vec[1] 指向栈上 64KB 兜底。一次 `readv()` syscall 至多取 65KB+，避免 Buffer 反复 resize 也避免 syscall 次数失控。

#### 第 5 步：`HttpServer::onMessage` — 状态机驱动 + pipeline 循环

来自 [HISTORY/day30/src/common/http/HttpServer.cpp](../HISTORY/day30/src/common/http/HttpServer.cpp)：

```cpp
void HttpServer::onMessage(Connection *conn) {
    if (conn->getState() != Connection::State::kConnected) return;
    HttpConnectionContext *ctx = conn->getContextAs<HttpConnectionContext>();
    if (!ctx) {
        conn->setContext(HttpConnectionContext{limits_});
        ctx = conn->getContextAs<HttpConnectionContext>();
    }
    if (autoClose_) conn->touchLastActive();

    Buffer *buf = conn->getInputBuffer();
    while (buf->readableBytes() > 0) {
        if (!ctx->requestInProgress) {
            ctx->requestInProgress = true;
            ctx->requestStart = SteadyClock::now();
        }
        // ... requestTimeoutSec 检查
        int consumed = 0;
        if (!ctx->parser.parse(buf->peek(), (int)buf->readableBytes(), &consumed)) {
            // 400 / 413
            return;
        }
        if (consumed > 0) buf->retrieve((size_t)consumed);
        else { break; }
        if (!ctx->parser.isComplete()) break;
        if (!onRequest(conn, ctx->parser.request())) return;
        ctx->parser.reset();
        ctx->requestInProgress = false;
    }
}
```

**为什么是 while 循环**：HTTP/1.1 pipeline 允许客户端连续发多条不等响应（也包括最常见的"两个 HTTP 请求被一次 read 一起拿到"的粘包场景）。一次 `doRead()` 可能让 inputBuffer 里同时存在 N 条完整请求 + 半条尾巴。while 循环每轮 parse 一条、retrieve 一条、dispatch 一条，最后剩下的半条留给下一次 doRead 续写。

**代入实参**：本场景下 readableBytes=132、parse 完返回 consumed=132、isComplete=true → 进入 `onRequest` 1 次后 retrieve 132、循环条件 0>0 不满足 → 退出 while。

#### 第 6 步：`HttpServer::onRequest` — 中间件链 + dispatch + send

```cpp
bool HttpServer::onRequest(Connection *conn, const HttpRequest &req) {
    LogContext::Guard logGuard(LogContext::nextRequestId(), req.methodString(), req.url());

    const std::string connHeader = req.header("Connection");
    bool close = (connHeader == "close") ||
                 (req.version() == HttpRequest::Version::kHttp10 && connHeader != "keep-alive");

    HttpResponse resp(close);

    auto dispatch = [&]() {
        auto it = routes_.find(makeRouteKey(req.method(), req.url()));
        if (it != routes_.end()) { it->second(req, &resp); return; }
        for (const auto &route : prefixRoutes_) {
            if (route.method == req.method() &&
                req.url().compare(0, route.prefix.size(), route.prefix) == 0) {
                route.handler(req, &resp);
                return;
            }
        }
        httpCallback_(req, &resp);
    };

    size_t index = 0;
    std::function<void()> runChain = [&]() {
        if (index < middlewares_.size()) {
            auto &mw = middlewares_[index++];
            mw(req, &resp, runChain);
            return;
        }
        dispatch();
    };
    runChain();

    // ... 监控记录、日志
    conn->send(resp.serialize());
    if (resp.hasSendFile()) {
        conn->sendFile(resp.sendFilePath(), resp.sendFileOffset(), resp.sendFileCount());
    }
    if (resp.closeConnection()) {
        conn->close();
        return false;
    }
    return true;
}
```

**洋葱模型展开**（5 层 + dispatch）：
```
runChain 第 1 次调用    : index=0 → AccessLog mw   → 调 next() 即 runChain
runChain 第 2 次调用    : index=1 → RateLimiter mw → 调 next()
runChain 第 3 次调用    : index=2 → CorsMiddleware → 调 next()
runChain 第 4 次调用    : index=3 → AuthMiddleware → 调 next()  (鉴权通过)
runChain 第 5 次调用    : index=4 → GzipMiddleware → 调 next()
runChain 第 6 次调用    : index=5 ≥ middlewares.size() → dispatch()
                          → routes_ 查找命中 → handleApiUsers(req, &resp)
返回回到 GzipMiddleware  : 现在 resp.body 已填好，按 Accept-Encoding 决定是否压缩
返回回到 AuthMiddleware  : 无后置逻辑
返回回到 CorsMiddleware  : 注入 Access-Control-* headers
返回回到 RateLimiter     : 无后置逻辑
返回回到 AccessLog       : 计时 + 日志
runChain 最终返回
```

#### 第 7 步：`Connection::send(std::string&&)` — 零拷贝直写路径

```cpp
void Connection::send(std::string &&msg) {
    if (msg.empty()) return;
    ssize_t nwrote = 0;
    size_t sz = msg.size();
    bool faultError = false;

    bool canDirectWrite = !channel_->isWriting() && outputBuffer_.readableBytes() == 0;
    if (canDirectWrite) {
        int savedErrno = 0;
        nwrote = ::write(sock_->getFd(), msg.data(), sz);
        if (nwrote < 0) savedErrno = errno;
        if (nwrote >= 0) {
            if ((size_t)nwrote == sz)
                return;                              // ★ 全部写完，msg 被丢弃，无任何拷贝
        } else {
            // EAGAIN 处理...
        }
    }
    // 部分写完 / 不能直写 → outputBuffer_.append() + enableWriting()
    if (!faultError && (size_t)nwrote < sz) {
        const size_t remaining = sz - (size_t)nwrote;
        // hardLimitBytes 检查
        outputBuffer_.append(msg.data() + nwrote, remaining);
        if (!channel_->isWriting()) channel_->enableWriting();
        applyBackpressureAfterAppend();
    }
}
```

**代入实参**：`msg.size()=250`，channel_ 当前没启用 WRITE，outputBuffer_ 空 → canDirectWrite=true → `::write(42, ..., 250)` → 返回 250 → 早返回，**不经过 outputBuffer，零次 memcpy**。
**syscall**：`write()` 1 次。

**关键**：这就是为什么本项目的小响应延迟能压到 P50 0.21ms——一次 readv + parse + 一次 write，没有任何中间拷贝。

#### 第 8 步：keep-alive 复用准备

回到 `onRequest`：`resp.closeConnection()=false` → 返回 true。
回到 `onMessage` while 循环：`ctx->parser.reset()` → 状态机回到 `kStart`，`requestInProgress=false`。
buf->readableBytes()=0 → while 退出。
回到 `Business()`：`state_==kConnected` → 不调 close。
T4 回到 `loop()` 下一轮 `kevent()`，等下一个请求或 EOF。

### 5.4 本场景 ASCII 数据流图

```
   ┌──────────────────────────────────────────────────────────────┐
   │  T4 (sub-reactor) — 整个请求只在这一个线程中处理              │
   └──────────────────────────────────────────────────────────────┘

   client          fd=42         inputBuffer        HttpContext     handleApiUsers
     │              │               │                  │                  │
     │ ──132B──▶    │               │                  │                  │
     │              │ EVFILT_READ   │                  │                  │
     │              │──→ kevent     │                  │                  │
     │              │   返回         │                  │                  │
     │              │               │                  │                  │
     │              │ doRead:       │                  │                  │
     │              │  readv(42)    │                  │                  │
     │              │   ─132B──▶   appended            │                  │
     │              │  readv(42)    │                  │                  │
     │              │   ◀ EAGAIN ── │                  │                  │
     │              │               │                  │                  │
     │              │  HttpServer::onMessage:          │                  │
     │              │   parse(peek(),132) ────132B───▶ │  状态机推进        │
     │              │   isComplete=true                │                  │
     │              │   retrieve(132) → 0B             │                  │
     │              │                                  │                  │
     │              │  HttpServer::onRequest:          │                  │
     │              │   middleware chain ×5 → dispatch                    │
     │              │   ────────────────────────────────────────────────▶│
     │              │                                                    │
     │              │   resp 填好（statusCode=200, body=102B）             │
     │              │  ◀──────────────────────────────────────────────── │
     │              │                                                    │
     │              │  conn->send(resp.serialize())  → 250B              │
     │              │   ::write(42, ..., 250) → 250                      │
     │              │                                                    │
     │ ◀──250B──────│                                                    │
     │              │                                                    │
     │              │  ctx.reset() → 状态机回到 kStart                     │
     │              │  Business 返回，state_==kConnected                  │
     │              │  T4 回到 kevent() 等下一个事件                       │
```

### 5.5 本场景涉及函数职责一句话表

| 函数 | 调用时机 | 职责 |
|---|---|---|
| `Connection::Business` | client fd 可读时（readCallback） | 编排 doRead → onMessageCallback → 错误后 close |
| `Connection::doRead` | Business 内 | ET 模式循环 readv 到 EAGAIN，把字节灌入 inputBuffer |
| `Buffer::readFd` | doRead 内 | 1 次 readv，自己 + 64KB 栈 buffer 双缓冲 |
| `HttpServer::onMessage` | onMessageCallback_ 触发 | while 循环驱动 HttpContext 状态机，处理 pipeline |
| `HttpContext::parse` | onMessage 内 | 逐字符状态机，返回是否完整、消费了多少字节 |
| `HttpServer::onRequest` | parser.isComplete() 时 | 跑 middleware chain → dispatch → send → keep-alive 决策 |
| `Connection::send(string&&)` | onRequest 内 | 优先 ::write 直写；写不完降级到 outputBuffer + enableWriting |
| `Connection::send(const string&)` | 同上的 lvalue 版本 | 同样直写优先，失败时 outputBuffer.append |
| `Connection::doWrite` | EVFILT_WRITE 触发时 | 排空 outputBuffer，全部写完时 disableWriting |
| `Connection::applyBackpressureAfterAppend` | outputBuffer 增长后 | 超 high watermark 暂停读，超 hard limit 断连 |

---

## 6. 场景 C — 主动断开连接（peer close / 服务端 close / 空闲超时）

### 6.1 业务场景

> 客户端发完最后一个请求后调 `close()`，FIN 到达服务端。服务端的 `read()` 返回 0。我们需要：从 poller 中注销 fd → 关闭 socket → 从 connections_ map 中 erase Connection 对象 → 析构 Connection。**关键约束**：`~Connection()` 必须在该 Connection 的归属 sub-reactor 线程内执行，且必须在该 sub-reactor 本次 poll 的 events_ 缓存遍历完毕之后。

### 6.2 调用链总览（树形 + 关键代码）

> 这是**全项目最复杂的一段调用链**：T4 检测到 EOF → 投递到 T0 改 map → T0 再投递回 T4 真正析构。两次跨线程跳跃，三次 wakeup pipe 写入。把它读懂，UAF / 跨线程析构这一类 bug 就基本免疫了。

#### 整体调用树（三跳跨线程）

```
┌──────────────────── T4 (sub-reactor，连接归属线程) ────────────────────┐
│                                                                       │
│  Eventloop::loop() → poller_->poll()                                  │
│     ▲ fd=42 readable + EOF       // 客户端发了 FIN                    │
│  ch->handleEvent() → Connection::Business()                           │
│     ├─ doRead()                                                       │
│     │    └─ readFd(42) returns 0                                      │
│     │         └─ state_ = kClosed; break;                             │
│     └─ state_ != kConnected  →  close()                               │
│          └─ Connection::close()                                       │
│               └─ deleteConnectionCallback_(42)                        │
│                    └─ TcpServer::deleteConnection(42)                 │
│                         └─ mainReactor_->queueInLoop(                 │
│                                [this, fd=42]{ ... erase ... })        │
│                              └─ wakeup() → write(T0.wakeupWriteFd,1)  ═══ wakeup #1 ═══┐
│  Business() 返回 → handleEvent 返回                                                    │
│  T4 处理本批 events_ 中其他 channel（同帧 EVFILT_WRITE 等）                            │
│  T4 回到 poller_->poll() 阻塞                                                          │
│  ★ 此时 Connection 对象仍存活（unique_ptr 还挂在 connections_ map 上）                │
└────────────────────────────────────────────────────────────────────────────────────────┘
                                                                                        │
┌─────────────────────────── T0 (mainReactor) ───────────────────────────────────────────┘
│
│  poll() 因 wakeup 返回 → handleWakeup → doPendingFunctors
│  执行 lambda：
│    ├─ auto it = connections_.find(42);
│    ├─ Connection *raw = it->second.release();   // ★ unique_ptr 卸载所有权
│    ├─ connections_.erase(it);                   // map 中立即移除条目
│    └─ ioLoop->queueInLoop([raw]{ delete raw; }) // ★ 把"裸指针 + delete"投回 T4
│         └─ wakeup() → write(T4.wakeupWriteFd, 1)  ═══════════ wakeup #2 ═══════┐
│  T0 返回 poll() 阻塞                                                           │
└────────────────────────────────────────────────────────────────────────────────┘
                                                                                │
┌──────────────────── T4 (sub-reactor，再次被唤醒) ──────────────────────────────┘
│
│  poll() 因 wakeup 返回 → handleWakeup → doPendingFunctors
│  执行 lambda：
│    └─ delete raw;
│         └─ ~Connection()
│              ├─ *alive_ = false;             // 通知所有 weak_ptr<bool> 持有者
│              └─ loop_->deleteChannel(channel_.get())
│                   └─ poller_->deleteChannel(channel_)
│                        ├─ kevent(EV_DELETE, EVFILT_READ,  fd=42)
│                        └─ kevent(EV_DELETE, EVFILT_WRITE, fd=42)
│              ├─ ~unique_ptr<HttpConnectionContext>
│              ├─ ~Buffer outputBuffer_
│              ├─ ~Buffer inputBuffer_
│              ├─ ~unique_ptr<Channel>          // Channel 析构
│              └─ ~unique_ptr<Socket>
│                   └─ ::close(42)              // fd 归还内核
│  T4 回到 poll() 阻塞
└────────────────────────────────────────────
```

#### 关键代码块

**① EOF 触发点 —— `Connection::doRead`**

```cpp
ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);
...
if (n == 0) {
    state_ = State::kClosed;
    LOG_INFO << "[server] client fd " << sockfd << " disconnected.";
    // ★ 不在此处调 close()，由 Business() 在所有回调完成后统一触发
    // 防止 close() → queueInLoop(删除) 后 Main Reactor 立刻析构 Connection
    // 而 Business() 中 onMessageCallback_(this) 尚未执行完毕，导致 use-after-free
    break;
}
```

**② 跨线程 erase + 跨线程 delete —— `TcpServer::deleteConnection`**

```cpp
void TcpServer::deleteConnection(int fd) {
    mainReactor_->queueInLoop([this, fd]{                  // ── Hop 1: → T0
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        Eventloop *ioLoop = it->second->getLoop();
        Connection *raw   = it->second.release();          // ★ 卸 unique_ptr
        connections_.erase(it);                            // map 中删除条目
        ioLoop->queueInLoop([raw]{                         // ── Hop 2: → T4
            delete raw;                                    // ★ 析构发生在归属线程
        });
    });
}
```

> **为什么用 `release() + delete raw` 而不是 `shared_ptr`？**  
> 早期版本用 `shared_ptr` + lambda 按值捕获，调用站点的 `std::function` 临时副本可能未被 elided，引用计数最后归零时实际落在调用方线程，导致 `~Connection()` 跑在错误线程，`KqueuePoller::events_` 缓存的 `Channel*` 立即 UAF。改成 `release()+delete raw` 后，所有权唯一落在 lambda body 内，析构线程 100% 确定。

**③ 析构内的两件事 —— `~Connection`**

```cpp
Connection::~Connection() {
    *alive_ = false;                            // 通知所有 weak_ptr<bool> 持有者
    loop_->deleteChannel(channel_.get());       // 从 poller 注销，避免 events_ 缓存悬空 udata
    // 后面的 unique_ptr 成员按声明逆序析构：
    //   context_ → outputBuffer_ → inputBuffer_ → channel_ → sock_(close fd)
}
```

**④ poller 注销 —— `KqueuePoller::deleteChannel`**

```cpp
void KqueuePoller::deleteChannel(Channel *ch) {
    int fd = ch->getFd();
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, ev, 2, nullptr, 0, nullptr);   // 即使原本只 ADD 了 READ 也无妨
}
```

#### 三跳必要性速查

| 跳跃 | 谁→谁 | 为什么必须跨 |
|---|---|---|
| Hop 0（同线程） | T4 内 doRead → Business → close | EOF 处理必须在读出所有数据后再触发 |
| Hop 1 | T4 → T0 | `connections_` map 仅在 T0 单线程改写 |
| Hop 2 | T0 → T4 | `~Connection` 内 `kevent(EV_DELETE)` 必须在 fd 归属线程 |

> 想象 Hop 2 缺失：T0 直接 `delete raw` → T4 仍可能在本批 events_ 后续元素中持有 `Channel*` → 立刻 UAF。这就是开发日志里多次提到的"跨线程析构 race"。

### 6.3 逐函数代码追踪

#### 第 1 步：peer 关闭，doRead 检测到 EOF

来自 [HISTORY/day30/src/common/net/Connection.cpp](../HISTORY/day30/src/common/net/Connection.cpp)：

```cpp
void Connection::doRead() {
    while (true) {
        ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);
        if (n > 0) { continue; }
        else if (n == 0) {
            state_ = State::kClosed;
            LOG_INFO << "[server] client fd " << sockfd << " disconnected.";
            // 不在此处调用 close()，由 Business() 在所有回调完成后统一触发
            break;
        }
        // ... EAGAIN / 错误 ...
    }
}
```

#### 第 2 步：`Business()` 末尾统一触发 close

```cpp
void Connection::Business() {
    if (state_ != State::kConnected) return;
    doRead();
    if (state_ == State::kConnected) {
        if (onMessageCallback_) onMessageCallback_(this);
    } else {
        close();   // ← 走这里
    }
}
```

#### 第 3 步：`Connection::close()` → 触发删除回调

```cpp
void Connection::close() {
    if (deleteConnectionCallback_)
        deleteConnectionCallback_(sock_->getFd()); // 传 fd 不传指针
}
```

`deleteConnectionCallback_` 是 TcpServer 在 newConnection 中 setDeleteConnectionCallback 注入的：`std::bind(&TcpServer::deleteConnection, this, _1)`。**注意只传 fd，不传指针**——避免回调链中持有 Connection 裸指针造成生命周期混淆。

#### 第 4 步：`TcpServer::deleteConnection` — 跨线程 erase + 跨线程 delete

```cpp
void TcpServer::deleteConnection(int fd) {
    // 本函数由 sub-reactor 线程经 Connection::close() 回调链调用，
    // 需切换到 main-reactor 线程操作 connections_ map（逻辑隔离，无需 mutex）
    mainReactor_->queueInLoop([this, fd]() {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            Eventloop *ioLoop = it->second->getLoop();
            std::unique_ptr<Connection> conn = std::move(it->second);
            connections_.erase(it);
            LOG_INFO << "[TcpServer] connection fd=" << fd << " deleted.";

            // 关键：不能用 shared_ptr 包装 — std::function 拷贝/std::function
            // 临时副本会导致引用计数最后一次归零落在调用方（main-reactor）线程，
            // ~Connection 在错误线程触发 KqueuePoller events_ 缓存的 Channel* UAF。
            // 用 release() 拿到裸指针，lambda 内 delete：所有权唯一落在 lambda
            // body，无论 std::function 拷贝多少次，析构必发生在 ioLoop 线程。
            Connection *raw = conn.release();
            ioLoop->queueInLoop([raw]() { delete raw; });
        }
    });
}
```

**这是整个项目中最关键的一段。详细的 UAF 痛史见 §9.1。** 三跳跨线程：
```
T4 (检测 EOF) → T0 (操作 map) → T4 (析构 Connection)
```

#### 第 5 步：`~Connection()` 在归属 sub-reactor 线程执行

```cpp
Connection::~Connection() {
#ifdef MCPP_HAS_OPENSSL
    if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
#endif
    *alive_ = false;                           // 通知所有 weak_ptr 持有者：我没了
    loop_->deleteChannel(channel_.get());      // 从 poller 注销
}
```

接着按声明逆序析构成员：`context_(any)` → `outputBuffer_` → `inputBuffer_` → `channel_(unique_ptr)` → `sock_(unique_ptr)`。**`sock_` 析构时调 `close(42)` 把 fd 还给内核。**

#### 第 6 步：`KqueuePoller::deleteChannel`

```cpp
void KqueuePoller::deleteChannel(Channel *channel) {
    int fd = channel->getFd();
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr);  // 忽略错误
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd_, &ev, 1, nullptr, 0, nullptr);  // 忽略错误
    channel->setInEpoll(false);
}
```

**为什么必须先 deleteChannel 再 close fd**：close 后 fd 立即可被内核复用，如果之前 kqueue 关注列表里还有这个 fd，下一次新 fd 复用同号时会让 kqueue 错误地把事件归到旧 udata（已 dangling Channel*）→ UAF。先 EV_DELETE 把 fd 从 kqueue 摘掉，再 close。

### 6.4 ASCII 三跳跨线程图

```
   T4 (sub-reactor)        T0 (main-reactor)         T4 (sub-reactor)
      │                          │                         │
      ├ doRead → n==0            │                         │
      ├ state=kClosed            │                         │
      ├ Business: close()        │                         │
      ├ deleteConnectionCallback │                         │
      ├ TcpServer::deleteConn    │                         │
      ├   mainReactor.queueInLoop│                         │
      ├   wakeup ──pipe write──▶ │                         │
      ├ Business 返回，回到 loop │                         │
      ├ events_ 遍历下个 channel │                         │
      ├ 本轮 poll 结束           │                         │
      ├ kevent() 阻塞            ├ wakeup pipe readable    │
      │                          ├ doPendingFunctors:       │
      │                          │   find(42) → conn.release()│
      │                          │   ioLoop.queueInLoop({   │
      │                          │     delete raw;          │
      │                          │   })                     │
      │                          ├ wakeup ──pipe write──────▶
      │                          ├ kevent() 阻塞            │
      │                          │                         ├ wakeup pipe readable
      │                          │                         ├ doPendingFunctors:
      │                          │                         │   delete raw
      │                          │                         │   ~Connection:
      │                          │                         │     *alive_=false
      │                          │                         │     deleteChannel
      │                          │                         │       kevent(EV_DEL,RD)
      │                          │                         │       kevent(EV_DEL,WR)
      │                          │                         │     ~Channel
      │                          │                         │     ~Socket → close(42)
      │                          │                         ├ kevent() 阻塞
```

### 6.5 服务端主动 close 的两条变体

#### 变体 1：`HttpServer` 收到 `Connection: close` 头

`onRequest` 内 `closeConnection()=true` → `conn->close()` → 同 §6.3 第 3 步起。

#### 变体 2：`autoClose` 空闲超时

`HttpServer::scheduleIdleClose` 在 newConnection 时挂了一个 60s 后到期的定时器（来自 [HISTORY/day30/src/common/http/HttpServer.cpp](../HISTORY/day30/src/common/http/HttpServer.cpp)）：

```cpp
conn->getLoop()->runAfter(timeout, [weak, conn, this]() {
    auto alive = weak.lock();
    if (!alive || !*alive) return;            // ★ 用 weak_ptr<bool> 判活
    TimeStamp deadline = TimeStamp::addSeconds(conn->lastActive(), idleTimeout_);
    if (TimeStamp::now() < deadline) {
        scheduleIdleClose(conn);              // 仍活跃，重排下一次
    } else {
        conn->close();                        // 超时，主动关
    }
});
```

定时器和连接共属同一 sub-reactor，单线程，无竞态。`weak_ptr<bool>` 是规避"定时器到期时 conn 已经被析构（场景 C 已发生）"的安全网。

### 6.6 本场景涉及函数职责一句话表

| 函数 | 调用时机 | 职责 |
|---|---|---|
| `Connection::doRead` 末尾 EOF 分支 | readv 返回 0 时 | 设 state=kClosed，break |
| `Connection::Business` 末尾 | doRead 后 state ≠ kConnected | 调 close() |
| `Connection::close` | Business 触发 / onRequest 主动 / 超时 | 调 deleteConnectionCallback_(fd)，自身不删自己 |
| `TcpServer::deleteConnection` | close 回调 | 跨线程到 T0 操作 map，再跨线程到 sub-reactor delete raw |
| `~Connection` | sub-reactor 线程 doPendingFunctors 内 | 置 *alive_=false → deleteChannel → 成员逆序析构 |
| `KqueuePoller::deleteChannel` | ~Connection 内 | EV_DELETE 摘除 fd，必须在 close fd 前 |
| `~Socket` | Connection 成员析构最后一步 | close(fd) 把 fd 还给内核 |
| `HttpServer::scheduleIdleClose` lambda | 60s 定时器到期 | weak_ptr 判活 → 重排或主动 close |

---

## 7. 场景 D — 优雅停机（SIGINT → 全部退出 → 析构）

### 7.1 业务场景

> 用户按 `Ctrl+C`，进程收到 `SIGINT`。`SignalHandler` 注册的回调被异步信号安全地标记一个 atomic 标志位 → main 线程在下次 loop 迭代检测到 → 调 `srv.stop()` → 所有 reactor 退出 → main() 内栈对象按构造逆序析构（先 srv 再 asyncLog）→ ~TcpServer → joinAll → 成员逆序析构 → 内存归零，进程 exit。

### 7.2 调用链总览（树形 + 关键代码）

> 关键约束：**所有线程必须先停掉**，才能开始析构 `Connection` / `Eventloop`。否则 `~Connection` 内的 `loop_->deleteChannel` 会与还在 poll 的 sub-reactor race。`~TcpServer` 显式 `joinAll()` 就是为了把"线程结束"和"内存析构"严格切开。

#### 整体调用树

```
T_signal (信号处理)                T0 (main 线程)              T1..TN (sub-reactors)
─────────────────                ─────────────────            ─────────────────────
  收到 SIGINT
  └─ 在已注册 channel 上 wakeup ────► poll() 返回
                                     handleWakeup
                                     doPendingFunctors
                                       └─ srv.stop()
                                            └─ HttpServer::stop()
                                                 └─ TcpServer::stop()
                                                      ├─ threadPool_->stopAll()
                                                      │    └─ for each subLoop:
                                                      │         ├─ subLoop->setQuit()
                                                      │         └─ subLoop->wakeup() ─► poll() 返回
                                                      │                                  loop 检查 quit_=true
                                                      │                                  while 退出
                                                      │                                  threadFunc 返回
                                                      │                                  std::thread 进入 joinable
                                                      ├─ mainReactor_->setQuit()
                                                      └─ mainReactor_->wakeup()
                                     loop 下一轮检查 quit_=true → 退出 while
                                     mainReactor_->loop() 返回
                                     TcpServer::Start() 返回
                                     HttpServer::start() 返回
                                     main() 继续
                                     ├─ asyncLog.stop()         // 阻塞等待 flush
                                     └─ main 函数返回
                                           ↓ 栈对象逆序析构
                                     ~HttpServer (默认)
                                     ~TcpServer
                                       ├─ stop()           // 幂等，no-op
                                       ├─ threadPool_->joinAll()   // 线程已 quit，立即返回
                                       │     ★ 自此再无任何 IO 线程在跑
                                       └─ 成员逆序析构：
                                            ├─ connections_   ◄── 最先析构
                                            │    └─ for each Connection:
                                            │         └─ ~Connection
                                            │              ├─ *alive_ = false
                                            │              └─ loop_->deleteChannel(channel_)
                                            │                   └─ kevent(EV_DELETE, fd)
                                            │              ├─ ~Channel
                                            │              └─ ~Socket → close(fd)
                                            ├─ threadPool_
                                            │    └─ ~vector<EventLoopThread>
                                            │         └─ for each:
                                            │              ├─ thread.join() // no-op，已退出
                                            │              └─ ~Eventloop
                                            │                   ├─ ~unique_ptr<Channel> evtChannel_
                                            │                   ├─ ~Poller
                                            │                   └─ close(wakeupReadFd_/wakeupWriteFd_)
                                            ├─ acceptor_
                                            │    └─ ~Acceptor → ~Channel → ~Socket → close(listen_fd)
                                            └─ mainReactor_
                                                 └─ ~Eventloop（同上）
                                     ~AsyncLogging   // 后端线程已停
                                     return 0; OS 回收剩余资源
```

#### 关键代码块

**① `Eventloop::loop` 的退出检查**

```cpp
void Eventloop::loop() {
    while (!quit_) {
        auto channels = poller_->poll(kPollTimeoutMs);
        for (auto *ch : channels) ch->handleEvent();
        doPendingFunctors();
    }
    // ★ quit_ 被 stop() 设为 true 后，需要 wakeup 让正在阻塞的 poll() 立刻返回
}

void Eventloop::setQuit() { quit_ = true; }
```

**② `TcpServer::~TcpServer` 的"join 然后析构"契约**

```cpp
TcpServer::~TcpServer() {
    stop();                       // 幂等：再次 setQuit + wakeup（防外部没调过 stop）
    threadPool_->joinAll();       // ★ 必须先 join，确保所有 IO 线程已退出
    // 此后所有成员按逆序析构（声明顺序见 TcpServer.h 上方注释）：
    //   connections_(unique_ptr<Connection> map) → 在此析构期间 IO 线程已不存在
    //   threadPool_(EventLoopThreadPool)         → 析构 EventLoopThread → ~Eventloop
    //   acceptor_                                → ~Channel + close(listen_fd)
    //   mainReactor_                             → ~Eventloop
}
```

**③ 析构顺序为什么"刚好对"**

声明顺序与析构顺序的细节，见 [src/include/net/TcpServer.h](../src/include/net/TcpServer.h)：

```cpp
// 声明顺序：mainReactor_(1) → acceptor_(2) → threadPool_(3) → connections_(4)
// 析构顺序：connections_(1st) → threadPool_(2nd) → acceptor_(3rd) → mainReactor_(4th)
```

> `connections_` 排在最先析构。`~Connection` 内要调 `loop_->deleteChannel()`，此时 `loop_` 必须仍然活着——所以 `threadPool_`（持有 sub-reactor 的 `Eventloop`）排在 `connections_` 之后析构。`mainReactor_` 排到最后，`acceptor_` 才能在销毁前完成 channel 注销。这是**靠成员声明顺序"硬编码"出来的析构契约**，不是靠运行期逻辑。

#### 与场景 C 的关键差异

| 维度 | 场景 C（运行期单连接 close） | 场景 D（停机时批量析构） |
|---|---|---|
| ~Connection 触发线程 | 归属 sub-reactor 线程（通过 Hop 2 投递） | T0（main 线程，因为 sub-reactor 已退出） |
| 是否需要 queueInLoop | 需要（sub-reactor 还在跑） | **不需要**（IO 线程已 join，无并发） |
| poller_->deleteChannel 的安全性 | 跨线程调 kevent 不安全 → 必须切线程 | 没有人在 poll，安全 |

> 这就是"为什么 stop 必须 join 在前、析构在后"——一旦 join 完成，所有 sub-reactor 的 `kqueueFd_` 没人在 read，可以由任意线程安全地 `EV_DELETE`。

### 7.3 逐函数代码追踪

#### 第 1 步：信号处理 → setQuit + wakeup

来自 [HISTORY/day30/src/common/net/TcpServer.cpp](../HISTORY/day30/src/common/net/TcpServer.cpp)：

```cpp
void TcpServer::stop() {
    threadPool_->stopAll();
    mainReactor_->setQuit();
    mainReactor_->wakeup();
}
```

来自 [HISTORY/day30/src/common/net/EventLoopThreadPool.cpp](../HISTORY/day30/src/common/net/EventLoopThreadPool.cpp)：

```cpp
void EventLoopThreadPool::stopAll() {
    for (auto *loop : loops_) {
        loop->setQuit();
        loop->wakeup();   // 唤醒阻塞在 poll() 中的线程，使其检测到 quit_ 后退出
    }
}
```

**N+1 次 wakeup**：sub-reactor N 次 + mainReactor 1 次。每个 wakeup 是 1 次 `write(pipe, 1B)` syscall。

#### 第 2 步：sub-reactor loop() 退出

`Eventloop::loop()` 的 while 检查 `quit_`：

```cpp
while (!quit_) {
    int timeout = timerQueue_->nextTimeoutMs();
    std::vector<Channel *> channels = poller_->poll(timeout);
    for (auto *ch : channels) ch->handleEvent();
    timerQueue_->processExpiredTimers();
    doPendingFunctors();
}
```

被 wakeup 时 `quit_` 已为 true → 处理本轮 wakeup channel（handleWakeup 排空 pipe）→ while 退出 → loop() 返回。

#### 第 3 步：~TcpServer 的析构契约

```cpp
TcpServer::~TcpServer() {
    stop();                    // ① 通常已在 SIGINT 时调过，幂等
    threadPool_->joinAll();    // ② 等所有子线程实际退出
    // ③ 成员按声明逆序析构（见 .h 注释）
}
```

`joinAll()`：

```cpp
void EventLoopThreadPool::joinAll() {
    for (auto &t : threads_)
        t->join();
}
```

**注意 joinAll 不销毁 EventLoop 对象**——`EventLoopThread::loop_` 仍持有 `unique_ptr<Eventloop>`。这是 §2.3 那段"Connection::~Connection 调 loop_->deleteChannel 时 loop_ 还活着"的关键保证。

#### 第 4 步：connections_ 析构链条

```cpp
// TcpServer 成员析构（自动）：
~unordered_map<int, unique_ptr<Connection>>
   for each kv:
       ~unique_ptr<Connection> → ~Connection
           *alive_ = false
           loop_->deleteChannel(channel_.get())   ← loop_ 仍活在 threadPool_.threads_[i].loop_
               KqueuePoller::deleteChannel
                   kevent(EV_DELETE, READ)
                   kevent(EV_DELETE, WRITE)
           ~Connection 成员逆序：
               ~any context_   → ~HttpConnectionContext → ~HttpContext (清 std::string/std::map)
               ~Buffer outputBuffer_ → 释放 vector<char>
               ~Buffer inputBuffer_
               ~unique_ptr<Channel> → ~Channel (空)
               ~unique_ptr<Socket> → close(fd)   ← fd 真正归还内核
~unique_ptr<EventLoopThreadPool> → ~EventLoopThreadPool
   ~vector<unique_ptr<EventLoopThread>>
       ~EventLoopThread:
           join()  (no-op, 已 join 过)
           ~unique_ptr<Eventloop> → ~Eventloop
               ~unique_ptr<Channel> evtChannel_  ← Channel 析构空
               ~unique_ptr<Poller>               ← KqueuePoller 析构 → close(kqueueFd_)
               close(wakeupReadFd_/wakeupWriteFd_)
~unique_ptr<Acceptor> → ~Acceptor
   ~unique_ptr<Channel> acceptChannel_
   ~unique_ptr<Socket> sock_ → close(listen fd)
~unique_ptr<Eventloop> mainReactor_  ← 同 sub-reactor 析构
```

### 7.4 ASCII 析构顺序图

```
   main() return 触发：
                    ┌─────────────┐
                    │  asyncLog   │← 栈对象 #2，后构造
                    └─────────────┘
                    ┌─────────────┐
                    │  srv        │← 栈对象 #1，先构造，先析构
                    └─────────────┘
                          ▼ ~HttpServer
                    ┌─────────────┐
                    │  server_    │← unique_ptr<TcpServer>
                    └─────────────┘
                          ▼ ~TcpServer
                          │  ① stop()
                          │  ② threadPool_->joinAll()
                          │  ③ 成员声明逆序：
                          ▼
       ┌───────────┬───────────┬───────────┬───────────┐
       │mainReactor│ acceptor  │ threadPool│connections│
       └───────────┴───────────┴───────────┴───────────┘
              (4)        (3)        (2)        (1)   ← 析构顺序

   (1) connections_ 析构，每个 ~Connection:
       *alive_ = false
       loop_->deleteChannel  ← loop_ 仍活，threadPool_ 还没析构 ✓
       ~成员

   (2) threadPool_ 析构，每个 ~EventLoopThread:
       join() (已 done)
       ~unique_ptr<Eventloop> → ~Eventloop
                ↑ 此时 (1) 已经访问完它

   (3) acceptor_ 析构  → close(listen_fd)

   (4) mainReactor_ 析构

   栈帧返回 main()，asyncLog 析构（已 stop()，立即结束）
   main() return 0，进程 _exit
```

### 7.5 本场景涉及函数职责一句话表

| 函数 | 调用时机 | 职责 |
|---|---|---|
| `SignalHandler::handler` | OS 投递 SIGINT 时 | 异步信号安全地写 wakeup pipe + 设 flag |
| `TcpServer::stop` | 信号 lambda 内 | stopAll sub-reactor + setQuit/wakeup main-reactor |
| `Eventloop::setQuit` + `wakeup` | stop 内 | 让 loop 在下次 while 判定退出 |
| `~TcpServer` | main 栈展开时 | stop（幂等）+ joinAll + 成员逆序析构 |
| `EventLoopThreadPool::joinAll` | ~TcpServer 内 | 等所有 IO 线程实际退出，**不销毁 EventLoop** |
| `~Connection` | T0 上 connections_ 析构遍历内 | *alive_=false → deleteChannel → 成员逆序 |
| `~Eventloop` | EventLoopThread 析构内 | evtChannel_ 先析构 → poller_ 析构 → close(wakeupFd) |
| `~Socket` | Connection/Acceptor 成员析构 | close(fd)，是唯一释放 fd 的地方 |

---

## 8. 跨线程通信备忘录：3 道闸门

整个项目的"线程安全"建立在如下 3 个原语上。任何"我想让 sub-reactor 干一件事"的需求都必须经过它们之一。

### 8.1 闸门 A：`Eventloop::queueInLoop`

**何时用**：调用方**可能不在**目标 loop 线程，且任务不需要立即返回结果。
**机制**：`pendingFunctors_.emplace_back(move(func))` + `wakeup()`。
**syscall 代价**：1 次 `write(pipe, 1)` + 目标线程 1 次 `read(pipe)` 排空。
**典型用例**：
- §4 中 T0 让 T4 `enableInLoop`
- §6 中 T4 让 T0 操作 map、T0 让 T4 delete raw
- HttpServer::scheduleIdleClose 内 runAfter 投递

### 8.2 闸门 B：`Eventloop::runInLoop`

**何时用**：调用方**可能在**目标 loop 线程，希望"在线程内则同步执行、跨线程则排队"。
**机制**：`isInLoopThread() ? func() : queueInLoop(move(func))`。
**典型用例**：定时器接口 `runAt/runAfter/runEvery` 内部，因为业务可能在自己的线程或在 loop 线程内调用。

### 8.3 闸门 C：`Channel::set*Callback`

**何时用**：注册"事件来了我要做什么"的回调。
**约束**：所有 `enableReading/enableWriting/disableXxx/disableAll` 都会触发 `loop_->updateChannel`，必须在 loop 归属线程内调用——这是为什么 §4 必须用 queueInLoop 把 enableInLoop 投递到 sub-reactor 线程。

### 8.4 三道闸门违反检测

写代码时如果你看到下面这些迹象，**立刻警觉**：

| 反模式 | 后果 | 正确写法 |
|---|---|---|
| 在 T0 直接 `conn->channel_->enableReading()` 而 conn 归属 T4 | KqueuePoller 数据结构竞态 | `T4.queueInLoop([conn]{ conn->channel_->enableReading(); })` |
| 在 lambda 里捕获 `shared_ptr<Connection>` | std::function 拷贝把析构落到错误线程 → UAF | `release()` 后裸指针 + `delete` |
| sub-reactor 内 `connections_.find(...)` | map 是 T0 单线程，sub-reactor 读非法 | 切到 T0：`mainReactor_->queueInLoop(...)` |
| 定时器回调直接拿 raw `Connection*` | conn 可能已析构 → UAF | 用 `weak_ptr<bool>` 判活 |

---

## 10. 实地观察：让本文每一步亲眼可见

### 10.1 lldb 断点单步追踪一次完整请求

```bash
# Terminal 1：跑服务器
cd build && lldb examples/http_server
(lldb) breakpoint set -n Acceptor::acceptConnection
(lldb) breakpoint set -n TcpServer::newConnection
(lldb) breakpoint set -n Connection::Business
(lldb) breakpoint set -n HttpServer::onMessage
(lldb) breakpoint set -n HttpServer::onRequest
(lldb) run

# Terminal 2：发一个请求
curl http://127.0.0.1:18888/health
```

每命中一个断点 `bt 6` 看调用栈、`thread info` 看线程 ID。你会清晰看到：
- acceptConnection / newConnection 在主线程
- Business / onMessage / onRequest 在某个 sub-reactor 线程（Worker-N）
- 同一个请求里，Business → onMessage → onRequest 一定是同一个 sub-reactor 线程

### 10.2 dtrace 计数 syscall

```bash
# 30s wrk 压测期间，统计 http_server 进程的 syscall 分布
sudo dtrace -n 'syscall:::entry /pid == $target/ { @[probefunc] = count(); }' \
  -p $(pgrep -n http_server)
```

预期 top 5：`kevent` → `read/readv` → `write` → `accept` → `close`。
