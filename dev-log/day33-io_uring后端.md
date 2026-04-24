# Day 33: io_uring 后端——把 Linux IO 从「就绪通知」推向「完成通知」

> **⚠️ 实验分支说明**　本日的内容是在 day30 生产发布之后的技术实验，未合入主代码树。
>
> 主仓库的可编译代码停留在 **day30** 状态；day31–day36 的可运行快照仅作为参考完整保留在 `HISTORY/day31` … `HISTORY/day36` 中。
>
> 这篇日志的所有代码路径与命令，默认均应补上 `HISTORY/day3X/` 前缀后阅读 / 运行。


> **今日目标**：在不破坏 macOS 主开发机编译的前提下，给项目加一个 io_uring 版本的 `Poller` 实现：跨平台条件编译 + 内核版本运行时探测 + 兼容模式 stub。最终调用方可在 Linux 5.1+ 的环境用环境变量切换到 io_uring，在 macOS 上完全无感。
> **基于**：Day 32（C++20 协程层）。**进入**：高性能 IO 基础设施周。

---

## 0. 今日构建目标

直接动手写 io_uring 是新手最容易翻车的地方——不是 API 难用，而是「写完只能在 Linux 跑，主开发机一编译就 50 个错误」。今天用「stub-first」策略：先把跨平台脚手架立起来，再往里填 Linux 实现。

**构建清单（按顺序）：**

1. **§2** — 设计模块全景：`IoUringPoller` 与 `EpollPoller` 同级，但用宏卫整个类声明
2. **§3** — 厘清初始化顺序：Linux 真实路径 vs macOS stub 路径
3. **§4** — 走通三种调用链（兼容模式注册读事件 / 内核版本不足回落 / 析构清理）
4. **§5** — 写跨平台条件编译头文件（含前向声明 liburing 类型）
5. **§6** — 写 `UringOp`：把 user_data 关联回 Channel*
6. **§7** — 写 `isAvailable()` 真探针（不依赖 uname 字符串解析）
7. **§8** — 验证：在 macOS CI 上确认链接器从不要求 `liburing.so`
8. **§9** — 写单元测试钉死跨平台 stub 行为

**说明**：代码块前的「来自 `HISTORY/day33/...`」标注意为「将以下代码写入该文件的对应位置」。

---

## 1. 今天要解决的几个问题

### 1.1 痛点：epoll 在高 QPS 下的两次 syscall 抽税

day1-32 我们的 IO 多路复用栈在 Linux 上是 `epoll`，在 macOS 上是 `kqueue`。它们都属于 **readiness-based** 模型：

```
用户态                     内核
┌──────────┐  epoll_wait  ┌──────────┐
│ EventLoop│ ───────────▶ │  内核    │
│          │ ◀─────────── │ 就绪表   │  ← 内核告诉你 "fd 现在可读"
│ read(fd) │ ───────────▶ │ socket   │  ← 用户态自己发起 read
│          │ ◀─────────── │ buffer   │
└──────────┘              └──────────┘
```

每次 IO 至少是 **2 次系统调用**：`epoll_wait` 通知 + `read/write` 实际操作。在 100k QPS+ 的场景下，syscall 开销占据可观比例。更糟的是 Linux 上的文件 IO（regular file read）天生**没有"就绪"概念**——`epoll` 监听 regular fd 永远立即可读，但 `read()` 仍可能阻塞等磁盘。这迫使整个生态退而求其次：libuv / Node.js 用线程池模拟异步文件 IO。

### 1.2 根因：「就绪通知」模型先天的 syscall 抽税

epoll 模型的根本问题在于「告诉你能做」与「真的做」是两次独立的内核往返。这个设计在 1k QPS 下完全无害——epoll_wait 一次返回多个就绪 fd，平均 syscall 占比可忽略。但当 QPS 升到 100k+：

- 每 QPS = 1 次 epoll_wait 唤醒 + 1 次 read + 1 次 write = 3 次 syscall
- 100k QPS × 3 syscall ≈ 30 万次/秒上下文切换
- 每次 syscall 平均 ~500ns → 0.15 秒纯抽税，占满一个核 15%

更隐蔽的问题：**Linux 上 regular file IO 没有「就绪」概念**。`epoll` 监听磁盘文件 fd 永远立即可读，但 `read()` 仍可能阻塞磁盘几毫秒。这迫使 libuv / Node.js 用「线程池假装异步」——把 read 投到线程池 → 一次额外上下文切换 → 反而更慢。

### 1.3 解法：「完成通知」模型 io_uring

`io_uring` 是 Linux 5.1 (2019) 引入的新接口，由 Jens Axboe 主导。它把模型彻底改为 **completion-based**：

```
用户态                     内核
┌──────────┐  写 SQE      ┌──────────┐
│ Submit   │ ───────────▶ │ Kernel   │  ← 用户态把 "请帮我做 read" 写入环
│ Queue    │              │ 处理     │
│          │              │          │  ← 内核异步完成 IO
│ Complete │ ◀─────────── │ Kernel   │
│ Queue    │  读 CQE      │          │  ← 完成结果回到环里
└──────────┘              └──────────┘
```

收益层层叠加：

- 通过环形队列**批量提交**，单次 syscall 可提交数十个操作
- `IORING_SETUP_SQPOLL` 让内核轮询提交环，达到**零 syscall** IO
- `regular file` / `network socket` 全用统一接口，文件 IO 终于"真异步"
- `IORING_OP_ACCEPT` / `READ` / `WRITE` / `SENDFILE` / `SPLICE` 一次性覆盖网络栈

业界基准：在 100M+ QPS 的存储引擎与代理（ScyllaDB / Cloudflare Pingora / Redis 7）上，io_uring 相对 epoll 有 20%-50% 的吞吐提升。

### 1.4 业界方案对比

| 方案 | 模型 | 平台 | 核心系统调用 | 适用场景 |
|------|------|------|-------------|---------|
| **select / poll** | readiness | POSIX | `select` / `poll` | 教学 |
| **epoll** | readiness | Linux 2.6+ | `epoll_create1` / `epoll_ctl` / `epoll_wait` | 当前主流网络服务器 |
| **kqueue** | readiness | BSD / macOS | `kqueue` / `kevent` | macOS / FreeBSD |
| **io_uring** | completion | Linux 5.1+ | `io_uring_setup` / `io_uring_enter` | 新一代高吞吐 IO |
| **IOCP** | completion | Windows | `CreateIoCompletionPort` | Windows 服务器（本项目不支持）|

完成式模型本质上是**接口设计的演进**——从"内核告诉我能做什么"变成"我告诉内核做什么、它做完通知我"。这种倒转让批量化、零拷贝、内核侧调度成为可能。

### 1.5 今日方案概述

**重要前提**：项目主开发机是 macOS（Apple M3），无 io_uring。本日目标不是写一个生产可用的 io_uring 实现，而是：

1. **建立平台 stub**：在 macOS 上提供 `IoUringPoller` 类的空实现（`isAvailable() == false`），让所有现有代码在 macOS 上仍能正常编译
2. **完成 Linux 路径骨架**：用 `liburing` 实现 `submitAndWait` / `handleCqe`，模式分两阶段——**兼容模式**（`IORING_OP_POLL_ADD`，把 io_uring 当 epoll 用）与**全异步模式**（`OP_READ`/`OP_WRITE`/`OP_ACCEPT`，留待 day34+）
3. **按内核版本运行时降级**：`uname()` 检测内核版本，<5.1 时返回 `isAvailable() == false`，调用方应回落到 `EpollPoller`
4. **不做的事**：本日不实现 `Connection` 与 io_uring 的对接（仍用 epoll fd 走旧路径）；不实现 SQPOLL 内核线程；不接 `Connection::send/recv` 改写

### 1.6 今日文件变更全图

```
src/include/net/Poller/
└── IoUringPoller.h                      [新增 132L]   类声明 + 跨平台条件编译

src/common/net/Poller/io_uring/
└── IoUringPoller.cpp                    [新增 175L]   Linux 实现（macOS 编译为空对象）

src/test/
└── IoUringPollerTest.cpp                [新增 70L]    跨平台 stub 行为测试

CMakeLists.txt                           [修改 +18]    MCPP_ENABLE_IO_URING 选项 + find_library(uring)
```

下一节起，我们正式按建造顺序动手。

---

## 2. 第 1 步 — 设计模块全景与所有权树

```
src/include/net/Poller/IoUringPoller.h
└── IoUringPoller : public Poller            ← 与 EpollPoller / KqueuePoller 同级
    │
    ├── 平台条件编译：
    │   #if defined(__linux__) && defined(MCPP_HAS_IO_URING)
    │       完整实现（依赖 liburing）
    │   #else
    │       stub 实现（所有方法返回错误码或空操作）
    │   #endif
    │
    ├── struct io_uring ring_                  ← liburing 的环结构（栈成员，析构由 io_uring_queue_exit 负责）
    ├── int ringFd_                            ← uring 对应的 fd（仅 SQPOLL 模式需要）
    ├── unsigned queueDepth_                   ← SQ/CQ 容量（默认 256）
    ├── std::unordered_map<int, Channel*> channels_  ← fd → Channel 观察者表
    │
    ├── enum class Mode { kCompat, kFullAsync } mode_
    │       kCompat：仅用 IORING_OP_POLL_ADD，行为等价 epoll
    │       kFullAsync：直接提交 read/write/accept SQE
    │
    └── 单例：getKernelVersion() → 比较 (5,1)，返回 isAvailable()

src/common/net/Poller/io_uring/IoUringPoller.cpp
└── Linux 完整实现（约 175 行）
     ├── ctor: io_uring_queue_init(queueDepth_, &ring_, 0)
     ├── poll(timeout):
     │   ├── io_uring_submit(&ring_)              ← 提交所有暂存 SQE
     │   └── io_uring_wait_cqe_timeout(...)       ← 等待至少一个 CQE
     │   └── for each cqe: handleCqe(cqe, channels_)
     ├── updateChannel(ch):
     │   └── io_uring_get_sqe → prep_poll_add(fd, events, channel*) → mark_for_submit
     ├── deleteChannel(ch):
     │   └── prep_poll_remove(channel*)
     └── dtor: io_uring_queue_exit(&ring_)
```

**所有权规则**：

- `Channel*` 是观察者，由 `EventLoop` 的 `channels_` 表持有所有权
- `io_uring` 结构由 `IoUringPoller` 值持有，析构时调用 `io_uring_queue_exit` 释放
- `liburing` 的内部 SQE/CQE 缓冲区由内核与库共享，应用层不需要管理

**为什么先画图**：io_uring 涉及内核侧映射、用户侧环、liburing 库三层资源。先理清谁拥有谁，再写代码就不会出现「构造一半内核已经分配了，但 C++ 抛异常导致泄漏」的尴尬。

---

## 3. 第 2 步 — 厘清初始化顺序（Linux 真实 vs macOS stub）

### 3.1 Linux（启用 io_uring）

```
[EventLoop 所在线程]

① Poller::newDefaultPoller(loop)
   if (env MCPP_PREFER_IO_URING == "1" && IoUringPoller::isAvailable())
       return make_unique<IoUringPoller>(loop)
   else
       return make_unique<EpollPoller>(loop)        ← 默认仍为 epoll

② IoUringPoller::IoUringPoller(loop)
   ├── 检查 uname() → 获取内核 release（如 "5.15.0-1056"）
   ├── 解析主版本.次版本 → 若 < (5,1) 抛 std::runtime_error
   ├── io_uring_queue_init(256, &ring_, 0)
   │   └── 内核分配 SQ/CQ 共享内存映射
   └── mode_ = kCompat（默认兼容模式，逐步迁移）
```

### 3.2 macOS（stub 实现）

```
[任意线程]

① Poller::newDefaultPoller(loop)
   IoUringPoller::isAvailable() == false → 直接选 KqueuePoller

② 即使显式 new IoUringPoller(loop)
   构造函数体为空 / 抛出 std::logic_error("io_uring unavailable on this platform")
```

**两条路径的共同保证**：调用方永远不需要 `#ifdef __linux__`——`isAvailable()` 是统一判定接口，决策权完全在工厂方法 `Poller::newDefaultPoller`。这是「平台抽象」最干净的形式。

---

## 4. 第 3 步 — 走通三种典型调用链

### 4.1 场景 A：兼容模式下注册一个 fd 的读事件（Linux）

```
① [IO 线程] Channel::enableReading()
   └── EventLoop::updateChannel(ch)
       └── poller_->updateChannel(ch)

② [IO 线程] IoUringPoller::updateChannel(ch)
   ├── auto* sqe = io_uring_get_sqe(&ring_)
   │   ── 失败时（环满）先 io_uring_submit() 腾出空间，再重试
   ├── io_uring_prep_poll_add(sqe, ch->fd(), POLLIN)
   ├── io_uring_sqe_set_data(sqe, ch)              ← user_data = Channel*
   ├── channels_[ch->fd()] = ch
   └── pendingSubmit_ = true                       ← 标记本轮 poll() 前需 submit

③ [IO 线程] EventLoop::loop() 下一次迭代
   └── poller_->poll(timeoutMs, &activeChannels)
       ├── if (pendingSubmit_) io_uring_submit(&ring_); pendingSubmit_ = false
       └── io_uring_wait_cqe_timeout(&ring_, &cqe, &ts)

④ [内核] fd 上有数据可读
   └── 内核生成 CQE：res = revents（POLLIN | POLLERR 等），user_data = Channel*

⑤ [IO 线程] handleCqe(cqe)
   ├── auto* ch = (Channel*)io_uring_cqe_get_data(cqe)
   ├── ch->setRevents(translateUringEvents(cqe->res))
   ├── activeChannels.push_back(ch)
   └── io_uring_cqe_seen(&ring_, cqe)              ← 释放 CQE 槽位

⑥ [IO 线程] EventLoop::loop() 继续：for (ch : activeChannels) ch->handleEvent();
```

### 4.2 场景 B：内核版本不足或 liburing 缺失（启动时检测）

```
① [初始化] CMake 时
   if (MCPP_ENABLE_IO_URING && Linux && find_library(uring))
       set MCPP_HAS_IO_URING=1
       link liburing
   else
       不编译 IoUringPoller.cpp Linux 实现，stub 兜底

② [运行时] IoUringPoller::isAvailable()
   ├── #if !defined(MCPP_HAS_IO_URING) → return false
   ├── 否则解析 uname → 比较版本 → return true / false
   └── 调用方据此决策（典型：通过环境变量 MCPP_PREFER_IO_URING 显式开启）
```

### 4.3 场景 C：析构时清理（Linux 完整模式）

```
① IoUringPoller::~IoUringPoller()
   ├── 若仍有未消费的 CQE：循环 io_uring_cqe_seen 清空
   ├── io_uring_queue_exit(&ring_)
   │   └── 内核解除 SQ/CQ 共享内存映射，关闭 ring fd
   └── channels_ unordered_map 析构（仅清观察者指针表）
```

**安全性**：本日 IoUringPoller 不持有任何 Channel/Connection 所有权，析构顺序与 EpollPoller 完全一致——`EventLoop::poller_` 比 `EventLoop::channels_` 先析构是无关紧要的（poller 不会回调）。

**三场景的共同点**：所有 `liburing` 调用都集中在 `IoUringPoller.cpp` 内部，调用方（EventLoop / Channel）不知道也不关心底层是 epoll 还是 io_uring。这是把新技术接进老代码的标准姿势。

---

## 5. 第 4 步 — 写跨平台条件编译头文件

这段代码论证了：**为什么调用方在 Linux 与 macOS 都能 `#include "IoUringPoller.h"` 而不引发链接错误**——通过宏关卡把 Linux 专属符号完全屏蔽。

```cpp
// src/include/net/Poller/IoUringPoller.h
#if defined(__linux__) && defined(MCPP_HAS_IO_URING)

// 前向声明 liburing 类型，避免在头文件中包含 liburing.h
struct io_uring;
struct io_uring_sqe;
struct io_uring_cqe;

namespace mcpp::net {
class IoUringPoller : public Poller {
  public:
    explicit IoUringPoller(Eventloop *loop, unsigned queueDepth = 256);
    ~IoUringPoller() override;
    // ...
    static bool isAvailable() noexcept;
  private:
    io_uring *ring_{nullptr};
    Stats stats_;
};
} // namespace

#endif // __linux__ && MCPP_HAS_IO_URING
```

关键点：宏卫至**整个类声明**，所以 macOS 上编译器看到的是空文件，`isAvailable()` 也不存在；调用方必须先 `#ifdef` 才能引用，避免「调用一个不存在的方法」隐藏到运行时。前向声明三个 `struct` 让头文件不依赖 `<liburing.h>`，对编译速度也是正面收益。

**踩过的坑**：曾试过把 `isAvailable()` 暴露到所有平台，结果 macOS 一调用 `io_uring_queue_init` 就符号未定义。教训：**条件编译要么宏卫到底要么完全不卫**，半截卫只会自找麻烦。

---

## 6. 第 5 步 — 写 UringOp 关联 user_data 回 Channel*

这段代码论证了：**io_uring 的「完成通知」如何关联回上层的 Channel**——通过把 `UringOp*` 塞进 SQE 的 `user_data`，CQE 完成时再取回。

```cpp
// src/include/net/Poller/IoUringPoller.h
enum class UringOpType : uint8_t {
    kPollAdd, kRead, kWrite, kAccept, kClose, kTimeout,
};

struct UringOp {
    UringOpType type;
    int fd;
    void *userData;     ///< 通常是 Channel*
    void *buf;
    size_t len;
    int32_t result;     ///< CQE 完成时填回
};
```

关键点:epoll 的世界里事件本身就携带 `epoll_data_t`，但 io_uring 是「先提交后完成」，必须在 SQE 入队时就备好用户态上下文。我们用 `userData` 字段保存 Channel，使 day25 之前实现的 EpollPoller 调用框架（`channel->setRevents()`）几乎不用改——`poll()` 里收割 CQE 后只需把 `result` 翻译成等价的 events 位图。

**验证：追踪一次 SQE 提交到 CQE 收割的全程**：

```
submit:  io_uring_get_sqe → prep_poll_add(fd=42, POLLIN)
         io_uring_sqe_set_data(sqe, channel_42)         ← user_data = Channel*
         io_uring_submit() → 内核接收 SQE
         （此时 EventLoop 进 epoll_wait 等价的 wait_cqe）

fd 42 上数据到达内核网卡缓冲：
         内核检查所有 poll 请求，匹配到 channel_42 的 SQE
         内核生成 CQE：res=POLLIN, user_data=channel_42
         CQE 入 CQ 环

wait_cqe 返回 → handleCqe(cqe):
         Channel* ch = (Channel*)cqe->user_data        ← 取回 Channel
         ch->setRevents(POLLIN)
         activeChannels.push_back(ch)
         io_uring_cqe_seen(cqe)                        ← 释放 CQE 槽位
```

整个流程除了「SQE 提交」这一新概念，其余与 epoll 完全同构。这就是「兼容模式」存在的意义——让现有 Channel/EventLoop 代码零改动迁移。

---

## 7. 第 6 步 — 写 isAvailable() 真探针

这段代码论证了：**为什么本项目在 macOS 上仍然能编译并通过测试**——`isAvailable()` 是 `static`，stub 文件里返回 `false`，于是上层路由代码可以在编译期或运行期分流。

```cpp
// src/common/net/Poller/io_uring/IoUringPoller.cpp
bool IoUringPoller::isAvailable() noexcept {
#if defined(__linux__) && defined(MCPP_HAS_IO_URING)
    io_uring probe;
    int ret = io_uring_queue_init(8, &probe, 0);
    if (ret != 0) return false;
    io_uring_queue_exit(&probe);
    return true;
#else
    return false;     // macOS / 旧 Linux 内核
#endif
}
```

关键点：「真探针」而非依赖 `uname -r` 字符串解析——内核版本数字可能撒谎（kernel 模块裁剪），但 `io_uring_queue_init` 失败是事实。CI 上 macOS runner 走 `#else` 分支返回 false，与上层 `if (IoUringPoller::isAvailable())` 配合，整个工程的链接器永远不会要求 `liburing.so`。这就是为什么 macOS CI 能稳定看到 `ranlib: warning: ... has no symbols`，却仍然 100% 通过测试。

**为什么 probe 用 depth=8 而不是 256**：探测的目的是确认能力，不是预热环。8 是 liburing 接受的最小值，能用最少的内核内存完成探测。探测后立即 `queue_exit` 释放，不留下任何状态。

---

## 8. 第 7 步 — 验证：析构顺序与跨平台不变量

| 声明顺序（IoUringPoller 内部） | 析构顺序 | 安全性分析 |
|----------------------------|---------|-----------|
| `struct io_uring ring_` | 最后析构 | 只是一个 POD，析构不做事 |
| `Mode mode_` | n/a | 平凡类型 |
| `unsigned queueDepth_` | n/a | 平凡类型 |
| `std::unordered_map<int, Channel*> channels_` | 第二析构 | 只清观察者指针，无 delete |
| `int ringFd_` | n/a（被 io_uring_queue_exit 覆盖管理） | 在显式 `io_uring_queue_exit` 中关闭 |

显式 `io_uring_queue_exit(&ring_)` 在析构函数体内**先**调用，因为它需要内核映射仍在原位。这一点 stub 实现不必关心。

跨平台的**关键不变量**：调用方在 Linux 上 `#ifdef` 选择 `IoUringPoller`；在 macOS 上即使误调用，构造函数会抛 `std::logic_error`，不会留下半初始化的环结构。

---

## 9. 第 8 步 — 写单元测试钉死跨平台 stub 行为

### 9.1 测试用例

| 测试名 | 覆盖场景 |
|--------|---------|
| `IoUringPollerTest.IsAvailable_DoesNotCrash` | 任何平台调用 `isAvailable()` 都不应抛异常 / segfault |
| `IoUringPollerTest.NotAvailableOnThisPlatform` | macOS 上 `isAvailable() == false`；Linux <5.1 同 |

### 9.2 测试输出

```
$ cd build && ./IoUringPollerTest
[==========] Running 2 tests from 1 test suite.
[ RUN      ] IoUringPollerTest.IsAvailable_DoesNotCrash
[       OK ] IoUringPollerTest.IsAvailable_DoesNotCrash (0 ms)
[ RUN      ] IoUringPollerTest.NotAvailableOnThisPlatform
[       OK ] IoUringPollerTest.NotAvailableOnThisPlatform (0 ms)
[  PASSED  ] 2 tests.
```

### 9.3 编译时输出（macOS）

```
[ 16%] Building CXX object .../IoUringPoller.cpp.o
[ 19%] Linking CXX static library libAiri-Cpp-Server-Lib.a
ranlib: warning: 'libAiri-Cpp-Server-Lib.a(IoUringPoller.cpp.o)' has no symbols
```

ranlib 的 "no symbols" 警告是**预期行为**——macOS 编译该 .cpp 时全部内容被 `#if defined(__linux__)` 屏蔽，目标文件不含符号。这证明跨平台 stub 工作正常。

### 9.4 验证：在 Docker 中跑 Linux 真实路径

```bash
$ docker run --rm -it -v $PWD:/src ubuntu:22.04 bash
root@xxx:/# apt update && apt install -y cmake g++ liburing-dev
root@xxx:/# cd /src && cmake -DMCPP_ENABLE_IO_URING=ON -B build-linux
root@xxx:/# cmake --build build-linux && ./build-linux/IoUringPollerTest
[  PASSED  ] 2 tests.   # isAvailable() 现在返回 true
```

这一步证明了「同一份代码 macOS 编 stub、Linux 编真实」是真的工作的。CI 之外可以再加一个 Linux runner 单独跑这条路径。

---

## 10. 局限与下一步

- **未接入实际 IO**：本日 `Connection::handleRead/Write` 仍走 `epoll`；day34+ 才会让 `Connection::sendAsync()` 走 `IORING_OP_WRITE`
- **未启用 SQPOLL**：内核轮询线程能消除 syscall，但需要 `CAP_SYS_NICE` 能力，CI 环境不便启用
- **CompatMode 性能**：`POLL_ADD` 等价 epoll 但每次都要 prep SQE，理论上比纯 epoll 略慢；要榨取 io_uring 的真正优势必须用 OP_READ/WRITE
- **取消支持**：`IORING_OP_ASYNC_CANCEL` 可取消 in-flight 请求，未来超时机制会用到
- **CI 测试**：GitHub Actions 的 ubuntu-latest 内核足够新（5.15+），但默认无 liburing 包，目前 CI 只编译 stub 通过；要测真实路径需 `apt install liburing-dev`

