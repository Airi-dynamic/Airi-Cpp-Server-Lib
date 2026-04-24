# Day 34: 无锁队列与 Work-Stealing 线程池——把 mutex 从热路径上拿掉

> **⚠️ 实验分支说明**　本日的内容是在 day30 生产发布之后的技术实验，未合入主代码树。
>
> 主仓库的可编译代码停留在 **day30** 状态；day31–day36 的可运行快照仅作为参考完整保留在 `HISTORY/day31` … `HISTORY/day36` 中。
>
> 这篇日志的所有代码路径与命令，默认均应补上 `HISTORY/day3X/` 前缀后阅读 / 运行。


> **今日目标**：在不动 Day 12 旧 `ThreadPool` 调用点的前提下，另外叠一套「无锁队列 + Work-Stealing 线程池」全新代。SPSC/MPMC 两种队列都是 header-only 零依赖、线程池提供 `submit() → future<T>` 接口。验证目标：SPSC 吞吐从旧实现的 ~3M ops/s 拉到 ~120M ops/s。
> **基于**：Day 33（io_uring 后端脚手架）。**进入**：高吞吐任务调度设施。

---

## 0. 今日构建目标

无锁代码不是「把 mutex 删了就行」——一字之差（acquire vs release、relaxed vs seq_cst）能让 ThreadSanitizer 沉默但生产环境随机丢任务。今天按「先 SPSC 后 MPMC、先队列后线程池」的顺序从简到繁建起，每一步都能独立跑一个复现 race 的压测验证。

**构建清单（按顺序）：**

1. **§2** — 全景与所有权树：两种队列模板 + 三层池指针结构
2. **§3** — 厘清初始化顺序（power-of-2 capacity / cell 初始 sequence / thread_local id 注入）
3. **§4** — 走通三种调用链（跨线程提交 / worker 窃取 / 优雅关闭）
4. **§5** — 写 `SPSCQueue::tryPush`：必要且仅必要的 acquire/release 配对
5. **§6** — 写 `WorkStealingDeque`：owner 走 LIFO bottom、stealer 走 FIFO top
6. **§7** — 写 `WorkStealingPool::workerLoop`：三层优先级 + relaxed stats
7. **§8** — 验证析构顺序与 join 不变量（透开这个错误会 `terminate`）
8. **§9** — 看微基准，验证 SPSC 是否真的快 40×

**说明**：代码块前的「来自 `HISTORY/day34/...`」标注意为「将以下代码写入该文件的对应位置」。

---

## 1. 今天要解决的几个问题

### 1.1 痛点：旧 ThreadPool 的 mutex 在 100k QPS 上是热点

day12 我们实现了 `ThreadPool`，它的核心是一个 `std::queue<Task>` + `std::mutex` + `std::condition_variable`：

```cpp
// day12 ThreadPool 简化版
void add(Task task) {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.push(std::move(task));
    cond_.notify_one();
}
Task take() {
    std::unique_lock<std::mutex> lk(mutex_);
    cond_.wait(lk, [&]{ return !queue_.empty() || stop_; });
    Task t = std::move(queue_.front()); queue_.pop();
    return t;
}
```

在 day26 的基准测试中（4 IO 线程 + 8 worker），这套实现暴露了两个严重瓶颈：

1. **全局锁竞争**：所有 IO 线程都向同一个 mutex 推任务，4 线程时可见 ~30% 的时间花在 futex 系统调用上（perf 显示）
2. **任务分配不均**：某个 worker 的任务长时间运行时，其他 worker 即使空闲也只能等下一个新任务被 push 才被唤醒

100k+ QPS 场景下，单 mutex 已经接近上限。

### 1.2 根因：全局锁 + condvar 的入出都串行化了

问题不是「mutex 本身慢」——`pthread_mutex_lock` 未争用时只是几条 atomic CAS。问题是「被争用的 mutex」：

- 4 个 IO 线程同时 push，有 3 个会 futex 睡眠等锁
- 1 个 worker 要 take，也要进 futex 睡眠等 condvar
- 错错 wakeup + 调度延迟 ≈ 几 µs，100k QPS 下每秒广 100k × 几 µs = 几百毫秒 CPU 抽税

更隐蔽的问题是**任务分配不均**：condvar 只 `notify_one` 一个 worker，被选中的 worker 可能手上还有长任务在跑，而其他 worker 空闲。调度器不知道谁忙谁闲。

### 1.3 解法：按「生产者-消费者拓扑」拆队列 + 让闲 worker 主动找活

业界的标准解药是 **per-thread 队列 + work-stealing**：

```
                     全局队列（少用）
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
   Worker 0           Worker 1          Worker 2
  ┌────────┐        ┌────────┐        ┌────────┐
  │ deque  │        │ deque  │        │ deque  │
  │ │push  │        │ │push  │        │ │push  │
  │ │pop   │ ◀──偷── │ │pop   │ ◀──偷── │ │pop   │
  └────────┘        └────────┘        └────────┘
   (本地 LIFO)                           (空闲时偷其他 worker 的 FIFO 端)
```

设计要点：

- 每个 worker **本地** push/pop（无锁或极短临界区）
- 空闲 worker 从其他 worker 队列**底端**（top）偷任务
- 本地 LIFO 维持 cache locality，远程 FIFO 减少冲突

进一步，IO 线程把任务投递给 worker 时，需要**生产者多消费者多**的无锁队列（MPMC Queue）。这是无锁数据结构的经典应用，也是高性能 C++ 的必修课。

### 1.4 业界方案对比

| 数据结构 | 算法 | 适用场景 | 代表实现 |
|---------|------|---------|---------|
| **SPSC Queue** | head/tail 双 cache line + acquire/release | 单生产者→单消费者，最快 | folly::ProducerConsumerQueue |
| **MPMC Queue (Vyukov)** | 每槽位 sequence atomic + CAS | 多生产者多消费者，bounded | moodycamel::ConcurrentQueue |
| **MPMC Queue (Lock-free linked)** | hazard pointer / RCU | unbounded，但回收复杂 | folly::MPMCQueue |
| **Work-Stealing Deque** | Chase-Lev 算法（top/bottom 双指针） | 任务调度 | TBB / folly::ThreadPoolExecutor |

调度框架：

| 框架 | 模型 | 备注 |
|------|------|------|
| **Intel TBB** | 严格的 Chase-Lev | 工业级，但依赖 Intel ABI |
| **Go runtime** | M:N 调度 + work-stealing | 启发式偷取策略 |
| **Tokio (Rust)** | 多 reactor + work-stealing | Rust 异步生态标准 |
| **folly Executor** | 多种 executor 可选 | Facebook 生产 |

### 1.5 今日方案概述

实现两条独立、互补的能力：

1. **无锁队列模板库**（`LockFreeQueue.h`）
   - `SPSCQueue<T>`：单生产者单消费者，环形缓冲，cache line 对齐避免 false sharing
   - `MPMCQueue<T>`：Vyukov 算法，bounded，每槽位用 sequence atomic 实现等待
   - 都是 header-only、无 std::mutex 依赖

2. **Work-Stealing 线程池**（`WorkStealingPool.h`）
   - 每个 worker 一个本地 deque（当前用 mutex 保护，未来可换 Chase-Lev）
   - 一个全局任务队列（fallback）
   - 偷取策略：本地 → 全局 → 随机偷其他 worker
   - 提供 `submit()` 返回 `std::future<T>`，与现有 `ThreadPool::add()` 兼容

3. **不做的事**：本日不替换现有 ThreadPool 的所有调用点（保留兼容）；不实现 Chase-Lev 完整版（mutex deque 已足够 demo）；不引入 hazard pointer

### 1.6 今日文件变更全图

```
src/include/async/
├── LockFreeQueue.h         [新增 266L]   SPSCQueue + MPMCQueue 模板
└── WorkStealingPool.h      [新增 267L]   Deque + Pool + Stats

src/test/
└── LockFreeQueueTest.cpp   [新增 275L]   17 个 GTest 用例

CMakeLists.txt              [修改 +3]    注册 LockFreeQueueTest
```

下一节起，我们正式按建造顺序动手。

---

## 2. 第 1 步 — 设计模块全景与所有权树

```
src/include/async/LockFreeQueue.h
├── kCacheLineSize : constexpr size_t            ← 64 或 std::hardware_destructive_interference_size
│
├── template<typename T> class SPSCQueue
│   ├── alignas(kCacheLineSize) std::atomic<size_t> head_     ← 消费者写
│   ├── alignas(kCacheLineSize) std::atomic<size_t> tail_     ← 生产者写
│   ├── std::vector<std::optional<T>> buffer_                  ← 大小为 power-of-2
│   └── const size_t mask_                                     ← capacity - 1，用于位运算取模
│
└── template<typename T> class MPMCQueue
    ├── struct Cell { std::atomic<size_t> sequence; T data; }
    ├── alignas(kCacheLineSize) std::atomic<size_t> enqueuePos_
    ├── alignas(kCacheLineSize) std::atomic<size_t> dequeuePos_
    └── std::vector<Cell> buffer_                              ← cell 间靠 sequence 自描述

src/include/async/WorkStealingPool.h
├── class WorkStealingDeque
│   ├── std::deque<Task> tasks_
│   ├── mutable std::mutex mtx_                               ← TODO: 换 Chase-Lev 后可移除
│   ├── pushBottom(Task)   ← 本线程 LIFO push
│   ├── popBottom() → optional<Task>   ← 本线程 LIFO pop
│   └── stealTop() → optional<Task>    ← 其他线程 FIFO steal
│
└── class WorkStealingPool
    ├── std::vector<std::unique_ptr<WorkStealingDeque>> localQueues_   ← 每 worker 一个
    ├── MPMCQueue<Task> globalQueue_                                    ← 跨线程 fallback
    ├── std::vector<std::thread> workers_
    ├── std::atomic<bool> stop_{false}
    ├── std::atomic<size_t> tasksExecuted_, tasksStolen_, tasksFromGlobal_
    │
    ├── thread_local size_t currentWorkerId_                            ← 由构造函数注入
    │
    └── workerLoop(id):
        loop {
          if local.popBottom() → 执行
          elif global.dequeue() → 执行
          else: 随机选 victim，stealTop() → 执行
          else: condvar wait_for(10ms)
        }
```

**所有权规则**：

- `WorkStealingPool` 持有 `localQueues_`、`globalQueue_`、`workers_`，构造时一并创建，析构时 join
- Task 是 `std::function<void()>`，由提交者构造，所有权在队列中流转
- `currentWorkerId_` 是 `thread_local`，每个 worker 自己读自己写，无竞争
- 统计 atomic 是只读监控接口（`stats()`），不参与正确性

**为什么要画三层拓扑图**：拥有 `localQueues_` 与 `globalQueue_` 两道底是为了退路——跨线程提交必须走 MPMC，但 worker 自己产生的子任务走本地 deque 零竞争。三层优先级 = 本地 LIFO → 全局 → 随机窃取，是 Tokio / Go runtime / TBB 全部走过的路。

---

## 3. 第 2 步 — 厘清初始化顺序

### 3.1 SPSCQueue / MPMCQueue（值类型）

```
[任意线程，例如主线程构造时]

① SPSCQueue<T>(capacity)
   ├── 计算 next_power_of_2(capacity) = N
   ├── buffer_.resize(N)
   ├── mask_ = N - 1
   ├── head_.store(0, relaxed)
   └── tail_.store(0, relaxed)

② MPMCQueue<T>(capacity) 类似，初始化每个 cell 的 sequence_ = i
```

### 3.2 WorkStealingPool

```
[主线程]

① WorkStealingPool(numWorkers)
   ├── localQueues_.reserve(numWorkers)
   ├── for i in [0, numWorkers): localQueues_.emplace_back(make_unique<Deque>())
   ├── globalQueue_(1024)
   └── for i in [0, numWorkers):
        workers_.emplace_back([this, i] {
            currentWorkerId_ = i;       ← 必须在 worker 线程内部赋值
            workerLoop(i);
        })

② 此后所有 worker 立即进入 workerLoop，开始扫描自己的 deque
```

**为什么容量必须 power-of-2**：`mask_ = N - 1` 在 N=power-of-2 时是「全 1 低位掩码」，`pos & mask_` 等价 `pos % N` 但快一个数量级。这是高性能环形队列的标准技术。

**为什么 `currentWorkerId_` 必须在 worker 线程内赋值**：`thread_local` 是「每线程一份」，主线程赋的值在 worker 线程看不见。这是如果将来 producer 也是 pool 内 worker，快路径能跳过全局队列的关键。

---

## 4. 第 3 步 — 走通三种典型调用链

### 4.1 场景 A：跨线程提交任务并被本地执行

```
① [Producer 线程] pool.submit([]{ doWork(); })
   ├── 包装为 packaged_task → shared_ptr 持有
   ├── 创建 Task lambda：[task]{ (*task)(); }
   ├── 检查 currentWorkerId_：若 producer 也是 pool 内 worker，pushBottom 到本地
   └── 否则 globalQueue_.enqueue(task)         ← MPMC enqueue 走 CAS 循环

② [MPMCQueue::enqueue]
   loop {
     pos = enqueuePos_.load(relaxed)
     cell = &buffer_[pos & mask_]
     seq = cell->sequence.load(acquire)
     dif = seq - pos
     if dif == 0:                              ← 本槽空闲
       if enqueuePos_.compare_exchange_weak(pos, pos+1, relaxed) break
     elif dif < 0:
       return false                            ← 队列满
     else:
       pos = enqueuePos_.load(relaxed)         ← 其他线程已抢走，重试
   }
   cell->data = std::move(task)
   cell->sequence.store(pos + 1, release)      ← 通知消费者

③ [Worker 线程 j] workerLoop(j)
   ├── localQueues_[j]->popBottom() → empty
   ├── globalQueue_.dequeue(out) → 成功
   └── 执行 task()
   └── tasksExecuted_++; tasksFromGlobal_++

④ task 内部如果 throw：
   packaged_task 捕获异常 → set_exception 到 future
   future.get() 时由调用者 rethrow
```

### 4.2 场景 B：worker 空闲时窃取

```
① [Worker 0] popBottom → empty
② [Worker 0] globalQueue_.dequeue → empty
③ [Worker 0] 随机选 victim_id ∈ [0, N) \ {0}
   ├── 假设 victim = 2
   ├── localQueues_[2]->stealTop() → 成功拿到任务
   └── tasksStolen_++; 执行任务

stealTop 实现细节（mutex 版）：
   std::lock_guard<std::mutex> lk(victim->mtx_)
   if victim->tasks_.empty() return nullopt
   Task t = std::move(victim->tasks_.front())
   victim->tasks_.pop_front()
   return t

未来切换 Chase-Lev 时，stealTop 用 atomic CAS 实现，与本地 popBottom 共享 top/bottom 两个 atomic。
```

### 4.3 场景 C：优雅关闭

```
① [主线程] pool.shutdown()  或  pool 析构
   ├── stop_.store(true, release)
   ├── condvar 通知所有 worker
   └── 不清空队列：剩余任务由 worker 在退出循环前执行完

② [每个 Worker]
   workerLoop 检查 stop_ + 队列均空 → break
   线程函数返回

③ [主线程] 析构 workers_ vector
   每个 std::thread 析构前应已 join；shutdown() 内显式 join 后置 stop=true
```

**幂等性**：`shutdown()` 内置 `std::call_once` 保护，多次调用安全。

**三场景的共同心得**：MPMC enqueue 的 CAS 循环贩看似复杂，但实际在 4×4 竞争下 retry 平均 < 2 次、运行时间 < 50ns——比 mutex 最低要 100ns 的 acquire 还是快一个量级。这就是「无锁」为什么值得。

---

## 5. 第 4 步 — 写 SPSCQueue::tryPush（acquire/release 的最小可行集）

这段代码论证了：**为什么仅靠一对原子变量 `head_` / `tail_` 就能保证生产者-消费者间的可见性**，不需要 mutex。

```cpp
// src/include/async/LockFreeQueue.h —— SPSCQueue<T>::tryPush
bool tryPush(T item) {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = head + 1;

    // 检查队列是否满（用 acquire 同步消费者最近一次的 tail_ 提升）
    if (next - tail_.load(std::memory_order_acquire) > capacity_) {
        return false;
    }

    new (&buffer_[head & mask_].data) T(std::move(item));   // ← 写入数据
    head_.store(next, std::memory_order_release);            // ← 数据写入对 pop 可见
    return true;
}
```

关键点：`head_.store(release)` 与 `tryPop` 中的 `head_.load(acquire)` 构成 happens-before——release 之前的所有写（包括 `placement new`）必然在 acquire 之后被消费者看到。这是 acquire/release 的最经典用法。`mask_ = capacity_ - 1` 配合「容量必须 2 的幂」让 `head & mask_` 替代取模，是 SPSC 性能能跑到 ~120 M ops/s 的原因之一。

**踩过的坑——为什么不能用 relaxed**：初版把两边都改 relaxed 试跑 TSAN，马上报 data race on `buffer_[0].data`——因为没有 release，消费者看到 head 推进了但 `placement new` 的写入可能还没提交到主存。**release 的价值在于「同步之前的所有写」而不是「指针本身的可见性」**。这是 acquire/release 初学者最容易误解的一点。

---

## 6. 第 5 步 — 写 WorkStealingDeque（owner LIFO bottom、stealer FIFO top）

这段代码论证了：**为什么 owner 与 stealer 操作同一个 deque 却能做到「最大化缓存命中、最小化竞争」**——owner 走 LIFO（bottom 端），stealer 走 FIFO（top 端）。

```cpp
// src/include/async/WorkStealingPool.h —— WorkStealingDeque
bool pop(Task &out) {                          // owner（LIFO）
    std::lock_guard<std::mutex> lock(mu_);
    if (deque_.empty()) return false;
    out = std::move(deque_.back());
    deque_.pop_back();
    return true;
}

bool steal(Task &out) {                        // stealer（FIFO）
    std::lock_guard<std::mutex> lock(mu_);
    if (deque_.empty()) return false;
    out = std::move(deque_.front());
    deque_.pop_front();
    return true;
}
```

关键点：owner 总从 back 端取最近 push 的任务——这是「最热的缓存行」，命中率最高。stealer 从 front 端取最早入队的任务——这通常是较粗粒度的工作单元，被偷走也不影响 owner 当下的局部性。这里我们诚实地用了 `mutex` 而非 Chase-Lev 算法（见 day34 已知限制），但「两端不同方向」的设计哲学已经能跑出 work-stealing 的核心收益。

**为什么 owner 走 LIFO 而不是 FIFO**：任务往往产生子任务（fork-join），刚 push 的子任务在父任务的变量还在 L1 缓存里。取最近 push 的 = 取最热的。这是 Cilk 调度器论文 1995 年就证明过的拓扑优势。

---

## 7. 第 6 步 — 写 WorkStealingPool::workerLoop（三层优先级 + relaxed stats）

这段代码论证了：**为什么 `tasksExecuted` 用 `memory_order_relaxed` 即可**——它只是统计量，不参与 happens-before，宽松内存序避免无谓的 barrier。

```cpp
// src/include/async/WorkStealingPool.h —— workerLoop 摘录
while (running_.load(std::memory_order_acquire)) {
    Task task;
    if (workerDeques_[id]->pop(task)) {                       // ① 本地 LIFO
        task();
        stats_.tasksExecuted.fetch_add(1, std::memory_order_relaxed);
        continue;
    }
    if (popFromGlobal(task)) {                                // ② 全局队列
        stats_.tasksFromGlobal.fetch_add(1, std::memory_order_relaxed);
        task();
        stats_.tasksExecuted.fetch_add(1, std::memory_order_relaxed);
        continue;
    }
    if (stealFromOthers(id, rng, task)) {                     // ③ 偷别人的
        stats_.tasksStolen.fetch_add(1, std::memory_order_relaxed);
        task();
        stats_.tasksExecuted.fetch_add(1, std::memory_order_relaxed);
        continue;
    }
    // ④ 等待：cv_.wait_for(...)
}
```

关键点：三条 `if` 严格按「本地 → 全局 → 偷他人」的优先级排序，前一档命中即 `continue`，避免无谓尝试。`stats_.tasksExecuted` 在 `task()` 之后才递增——这意味着「future.get() 返回」与「stats 更新」之间没有 happens-before：`StatsTracksExecution` 测试因此必须**轮询等待**而非直接断言（这正是 day36 之后修复 CI 时调整测试逻辑的原因）。

**为什么 stats 用 relaxed**：stats 只是「谁也不依赖它」的计数器，不参与任何同步。用 relaxed 避免了 release 的 store buffer flush 抽税，热路径每份任务快几个 ns——100k QPS 上全局看就是 1ms CPU。「在必要处不加 barrier」是高性能 C++ 的核心太极。

---

## 8. 第 7 步 — 验证：析构顺序与 join 不变量

`WorkStealingPool` 成员的声明顺序：

| 声明顺序 | 析构顺序 | 安全性分析 |
|---------|---------|-----------|
| `std::vector<std::unique_ptr<Deque>> localQueues_` | 最后 | 此时 worker 已 join，deque 安全析构 |
| `MPMCQueue<Task> globalQueue_` | 第三 | 无线程访问，安全 |
| `std::vector<std::thread> workers_` | 第二 | **关键**：触发 join，确保 worker 已退出 |
| `std::atomic<bool> stop_` | 第一（先析构）| 普通 atomic，但 worker 必须**先**看到 stop 后退出 |

**关键约束**：用户必须先调用 `shutdown()`（或确认 stop 已被通知）再让 pool 析构，否则 std::thread 析构时仍 joinable 会触发 `std::terminate`。当前实现中 `~WorkStealingPool()` 内首先 `shutdown()`，幂等保护。

**SPSC/MPMC 的析构**：buffer_ vector 自动释放，cell 内 T 调用析构。无 atomic 访问，因为已无并发线程。

**lock-free 不等于 wait-free**：MPMCQueue 在极端竞争下 CAS 可能多次重试，理论上有饥饿风险。本日不解决（生产场景通过限流 + 队列容量监控规避）。

---

## 9. 第 8 步 — 写单元测试 + 看微基准

### 9.1 单元测试分布

| 套件 | 测试名 | 验证要点 |
|------|--------|---------|
| `SPSCQueueTest` | `BasicPushPop` | 基本入队出队语义 |
| | `CapacityRoundedToPowerOf2` | 任意 capacity 向上取整 |
| | `FullQueueRefusesPush` | 满队列 push 返回 false |
| | `ConcurrentProducerConsumer` | 单生产者写 100k 项 + 单消费者读全部，无丢失 |
| | `MoveOnlyType` | 支持 unique_ptr 等 move-only 类型 |
| `MPMCQueueTest` | `BasicPushPop` | 单线程语义 |
| | `MultipleProducersConsumers` | 4 producer × 4 consumer × 10k = 160k 项，CRC 校验 |
| | `FullQueueRefuses` | 满队列正确拒绝 |
| `WorkStealingPoolTest` | `ConstructWithDefaultThreads` | 默认 hardware_concurrency() |
| | `SubmitAndGetResult` | future 同步等待结果 |
| | `SubmitWithArgs` | 完美转发参数 |
| | `SubmitVoidTask` | void 任务 future |
| | `ManyTasks` | 1000 个微任务 |
| | `ParallelSum` | 并行 reduce，结果正确 |
| | `ShutdownIsIdempotent` | 多次 shutdown 不死锁 |
| | `ExceptionPropagation` | task throw 经 future 透传 |
| | `StatsTracksExecution` | 计数器正确 |

### 9.2 测试输出

```
$ cd build && ./LockFreeQueueTest
[==========] Running 17 tests from 3 test suites.
[----------] 5 tests from SPSCQueueTest
[       OK ] SPSCQueueTest.ConcurrentProducerConsumer (10 ms)
[----------] 3 tests from MPMCQueueTest
[       OK ] MPMCQueueTest.MultipleProducersConsumers (23 ms)
[----------] 9 tests from WorkStealingPoolTest
[       OK ] WorkStealingPoolTest.ParallelSum (4 ms)
[==========] 17 tests from 3 test suites ran. (56 ms total)
[  PASSED  ] 17 tests.
```

### 9.3 验证：微基准证实 SPSC 收益

在 macOS M3 上跑了一次 SPSC 1M 项吞吐：

| 实现 | 吞吐 | 说明 |
|------|------|------|
| `std::queue + mutex` | ~3 M ops/s | day12 实现 |
| `SPSCQueue` (本日) | ~120 M ops/s | 单生产单消费 |
| `MPMCQueue` (本日) | ~25 M ops/s | 4×4 线程，CAS 竞争 |

**得到 ~40× 提升**——与 SPSC 的理论预期一致。这证明 cache line 对齐 + acquire/release 的设计是有效的。下一步（day35）要把内存分配也从热路径上拿掉。

### 9.4 验证：跑 ThreadSanitizer 确认无 race

```bash
$ cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" -B build-tsan && cmake --build build-tsan
$ ./build-tsan/LockFreeQueueTest
[  PASSED  ] 17 tests.    # 零 race 报告
```

这是验证无锁代码正确性的最低门槛。任何一次 acquire/release 错位都会被 TSAN 闪点报出。

---

## 10. 局限与下一步

- **WorkStealingDeque 仍用 mutex**：未来切换到 Chase-Lev 算法（top atomic + bottom atomic + buffer atomic swap）后才能算"严格无锁"
- **MPMCQueue 是 bounded**：满队列直接拒绝，调用方需要重试或丢弃。未来可考虑 unbounded 版本（folly::MPMCQueue 用 segment list）
- **没有亲和性绑定**：worker 没有 pin 到具体 CPU，cache locality 不如 TBB。Linux 下可加 `pthread_setaffinity_np`
- **steal 策略简单**：随机选 victim，没有按队列长度排序。可对照 Tokio runtime 引入更智能的启发式
- **未替换现有 ThreadPool**：`HttpServer` 等仍用旧 `ThreadPool`，只是新工具备好。day35+ 才会考虑迁移

