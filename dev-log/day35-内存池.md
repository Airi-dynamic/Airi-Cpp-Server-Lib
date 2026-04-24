# Day 35: 内存池——把 `new` / `delete` 从热路径上拿掉

> **⚠️ 实验分支说明**　本日的内容是在 day30 生产发布之后的技术实验，未合入主代码树。
>
> 主仓库的可编译代码停留在 **day30** 状态；day31–day36 的可运行快照仅作为参考完整保留在 `HISTORY/day31` … `HISTORY/day36` 中。
>
> 这篇日志的所有代码路径与命令，默认均应补上 `HISTORY/day3X/` 前缀后阅读 / 运行。


> **今日目标**：为「已知类型、高频分配、生命周期短」的对象提供三层内存池。单线程 `FixedSizePool<T>` 走 free-list O(1)、多线程 `ConcurrentFixedSizePool<T>` 加「最薄锁」、按 size class 聚合为 `SlabAllocator`，并带一套 `SlabSTLAllocator<T>` 能直接插进 `std::vector` / `std::list`。验证目标：FixedPool 比 `new/delete` 快 1.15×（jemalloc 越快增幅越小，glibc 上社区数据 2–3×）。
> **基于**：Day 34（无锁队列 + WorkStealingPool）。**进入**：Day 36 与 muduo 横向对比。

---

## 0. 今日构建目标

内存池是「谁都会写但一写就错」的组件——union 不对齐、`placement new` 忘了手写析构、跨 allocator 丢 deallocate。今天从「最简的单线程池」动手，每一层只多加一点资源控制：

**构建清单（按顺序）：**

1. **§2** — 全景与所有权树：三层池与 free-list / mutex / size-class table 的嵌套关系
2. **§3** — 厘清初始化顺序（延迟分配的 expand / SlabAllocator 的 10 个子池）
3. **§4** — 走通三种调用链（同类型高频 / 跨线程不同 size / 超 4KB fallback）
4. **§5** — 写 `FixedSizePool::allocate` + `deallocate`：free-list 的头插与 LIFO 缓存友好
5. **§6** — 写 `allocateBlock`：`max(sizeof(T), sizeof(FreeNode))` 这个 max 是必须的
6. **§7** — 写 `ConcurrentFixedSizePool`：为什么「如果一定要加锁，要加到足够薄」
7. **§8** — 验证析构顺序：为什么池不会调 `T::~T()`、跨 allocator deallocate 的 UB
8. **§9** — 写 14 个 GTest（含 ASAN 验证、stress no-leak）与看微基准

**说明**：代码块前的「来自 `HISTORY/day35/...`」标注意为「将以下代码写入该文件的对应位置」。

---

## 1. 今天要解决的几个问题

### 1.1 痛点：`malloc` 在 100k QPS 上吃掉 8–15% CPU

day14-22 实现 HTTP / WebSocket 协议解析后，单条请求的生命周期里会发生大量小对象分配：

```
一次 HTTP 请求处理流程（粗略）：
├── new HttpRequest                      ← 每个请求一份
├── new HttpResponse
├── new std::vector<char>(headerBuf)     ← 解析中转
├── std::shared_ptr<TcpConnection>       ← 控制块 + 对象，两次分配
├── std::function<void()> 回调闭包       ← 大小变化时堆分配
└── 业务逻辑里的 std::string / json 对象 ……
```

`malloc` 在 macOS 上是 jemalloc 的变种，Linux 通常是 glibc ptmalloc。它们都需要：

1. 进入分配器加锁（多线程争用）
2. 查 size class、可能触发 mmap / sbrk
3. 维护 metadata（free list、bins、size headers）

在 100k+ QPS 下，perf 显示 `malloc` + `free` 占 CPU 时间 **8%-15%**。这部分对真正的业务逻辑没有任何贡献。

### 1.2 根因：通用分配器没有「领域知识」

`malloc` 不知道你的 `HttpRequest` 是 320 字节还是 8 字节，为了通用性必须：

- 在每块内存前装 8–16 字节的 size header（为了 `free` 能查到大小）
- 走通用 size class 查找表、可能跨 bin 调整
- 多线程下进 arena 锁、甚至跨 arena 迁徙

但我们知道：`HttpRequest` 总是 320 字节，生命周期是一次请求，释放后下一个请求并能复用。这个领域知识能跳过上面所有过程。

### 1.3 解法：预分一大块、切碎挂 free-list、LIFO 复用

业界共识：**已知大小、生命周期短、高频分配**的对象应该使用专用内存池，而不是通用分配器。带来的收益：

- **去锁**：单线程内的 free list pop / push 是 O(1) 无原子操作
- **零碎片**：固定块大小，无需 size header，分配器内部不保留外碎片
- **缓存友好**：连续分配的对象内存上相邻，遍历时 cache miss 显著下降
- **可统计**：自带分配/回收计数，便于发现内存泄漏

### 1.4 业界方案对比

| 方案 | 模型 | 适用场景 | 代表实现 |
|------|------|---------|---------|
| **Object Pool** | 固定大小，固定类型 | 高频小对象（连接、请求） | boost::object_pool |
| **Slab Allocator** | 多 size class，每 class 一个池 | 内核场景，通用分配 | Linux SLUB / Solaris slab |
| **Arena Allocator** | 一次性 bump 分配，整体释放 | 短生命周期批量对象 | absl::Arena, protobuf::Arena |
| **TLAB** | thread-local arena | 高并发短对象 | JVM TLAB, tcmalloc |
| **PMR** | C++17 标准 polymorphic allocator | 标准库容器适配 | std::pmr::monotonic_buffer_resource |

各家通用分配器 (`tcmalloc` / `jemalloc` / `mimalloc`) 也内置了 size class，但它们是**通用**方案；专用内存池在已知场景下能比通用分配器再快 30%-200%。

### 1.5 今日方案概述

实现三个层次的内存池：

1. **`FixedSizePool<T, BlockSize>`**：模板，单线程使用，针对类型 T 预分配 BlockSize 个对象的连续 block，按需扩展更多 block；free list 用 union 复用对象内存
2. **`ConcurrentFixedSizePool<T>`**：在 1 的基础上加 mutex，支持多线程
3. **`SlabAllocator`**：维护 10 个 size class（8B / 16B / 32B / ... / 4KB），每个 class 一个 ConcurrentFixedSizePool；提供 `allocate(size)` / `deallocate(ptr, size)`；附带 `SlabSTLAllocator<T>` 适配器，可直接给 `std::vector` / `std::list` 用

**不做的事**：本日不替换全局 `new` / `delete`（侵入式过强）；不实现完整的 PMR memory_resource（接口契约繁琐，留待 day37）；不实现 thread-cache（`SlabAllocator` 的 mutex 已能扛 100k QPS）

### 1.6 今日文件变更全图

```
src/include/memory/
└── MemoryPool.h            [新增 396L]   FixedSizePool / Concurrent / Slab / STLAdapter 全部 header-only

src/test/
└── MemoryPoolTest.cpp      [新增 277L]   14 个 GTest 用例

CMakeLists.txt              [修改 +3]    注册 MemoryPoolTest
benchmark/
└── conn_scale_test.cpp     [关联]       后续可拿来对比池 vs malloc
```

下一节起，我们正式按建造顺序动手。

---

## 2. 第 1 步 — 设计模块全景与所有权树

```
src/include/memory/MemoryPool.h
│
├── template<typename T, size_t BlockSize = 4096>
│   class FixedSizePool                       ← 单线程
│   ├── union FreeNode { T storage; FreeNode* next; }   ← 复用对象槽
│   ├── struct Block { unique_ptr<FreeNode[]> data; }
│   ├── std::vector<Block> blocks_            ← 持有所有内存块
│   ├── FreeNode* freeHead_                   ← 单向 free list
│   ├── 统计：allocated_, used_, peak_
│   │
│   ├── allocate() → T*:
│   │     若 freeHead_ 空 → expand() 分配新 Block
│   │     否则 pop 一个 FreeNode 返回
│   │
│   └── deallocate(T* p):
│         reinterpret 为 FreeNode → push 到 freeHead_
│
├── template<typename T> class ConcurrentFixedSizePool
│   ├── FixedSizePool<T> impl_
│   └── std::mutex mtx_                        ← 包裹 allocate/deallocate
│
└── class SlabAllocator                        ← 聚合多个 size class
    ├── struct SizeClass { size_t blockSize; ConcurrentFixedSizePool<...>* pool; }
    │   实际通过 std::byte 数组模拟，每 size class 一个池
    ├── 预设 size classes: 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    │
    ├── allocate(size) → void*:
    │     找最小 ≥ size 的 size class
    │     pool.allocate()
    │     若 size > 4096 → fallback 到 ::operator new
    │
    └── deallocate(p, size) → void:
          按 size 找对应 pool 释放；> 4096 走 ::operator delete

可选：SlabSTLAllocator<T>
└── 实现 std::allocator 接口，内部转发到全局 SlabAllocator 单例
```

**所有权规则**：

- `FixedSizePool::blocks_` **拥有**所有内存（unique_ptr 数组），析构时一次性释放
- `freeHead_` 是**观察者指针**，永远指向 blocks_ 内某处或 nullptr
- `ConcurrentFixedSizePool` 持有 `FixedSizePool`（值），mutex 保护
- `SlabAllocator` 持有 10 个 ConcurrentFixedSizePool（值），构造时一并初始化
- 用户拿到的 `T*` 是**未初始化的内存**，必须配合 placement new 使用：`new(pool.allocate()) T(args...)`

**为什么要剖开三层**：单线程池走热路径零锁零原子；多线程池加薄锁覆盖 80% 场景；Slab 聚合多 size class 为不定型场景（`std::vector::push_back` 变长后的 allocate(N)）提供接口。这是 tcmalloc / jemalloc / mimalloc 全部共享的多层架构。

---

## 3. 第 2 步 — 厘清初始化顺序

### 3.1 FixedSizePool

```
[创建 pool 的线程]

① FixedSizePool<MyObj, 256>()
   ├── blocks_ vector 默认构造（空）
   ├── freeHead_ = nullptr
   └── 计数器全部 0

② 第一次 allocate() 触发 expand()
   ├── auto block = make_unique<FreeNode[]>(256)
   ├── 把 256 个 FreeNode 通过 next 串成 free list
   ├── blocks_.emplace_back(std::move(block))
   ├── freeHead_ = &blocks_.back().data[0]
   └── allocated_ += 256

③ 后续 allocate() 直接 pop free list；free list 耗尽时再 expand()
```

### 3.2 SlabAllocator

```
[全局单例或主线程]

① SlabAllocator()
   构造 10 个 ConcurrentFixedSizePool，每个 size class 一个
   ├── pools_[0]: 8 B class
   ├── pools_[1]: 16 B class
   ├── ...
   └── pools_[9]: 4096 B class
   每个 pool 自身延迟分配（第一次 allocate 才 expand）

② 后续 allocate(size)
   ├── 找 size class（线性 / 二分）
   └── 调用对应 pool 的 allocate
```

**为什么采用延迟分配**：构造 `SlabAllocator` 时预分 10 个 class × 每 class 默认 1024 slot × 最大 4KB = 40MB，这对仅用其中 2 个 class 的进程是巨大浪费。「第一次 allocate 才 expand」让池的静态内存足迹只跟实际使用的 size class 走。

---

## 4. 第 3 步 — 走通三种典型调用链

### 4.1 场景 A：高频分配同类型小对象

```cpp
FixedSizePool<HttpRequest> reqPool;
auto* req = new(reqPool.allocate()) HttpRequest();
processRequest(req);
req->~HttpRequest();
reqPool.deallocate(req);
```

调用链：

```
① [Worker 线程] reqPool.allocate()
   ├── freeHead_ != null → pop:
   │   FreeNode* p = freeHead_; freeHead_ = p->next
   │   used_++; peak_ = max(peak_, used_)
   └── return reinterpret_cast<T*>(p)

② placement new(p) HttpRequest()
   调用 HttpRequest 构造函数（共享内存槽，不分配新内存）

③ ~HttpRequest()  →  显式析构

④ reqPool.deallocate(p)
   ├── auto* node = reinterpret_cast<FreeNode*>(p)
   ├── node->next = freeHead_
   ├── freeHead_ = node
   └── used_--
```

**关键点**：union 让 FreeNode 与 T 共享同一段内存。这要求 sizeof(T) 至少为 sizeof(void*)，但 union 的 storage 字段已自动满足。如果对齐有要求（如 SIMD 64 字节对齐），需要在 unique_ptr 模板参数里加 alignas。

### 4.2 场景 B：多线程并发使用 SlabAllocator

```cpp
SlabAllocator& slab = SlabAllocator::instance();
void* p = slab.allocate(72);   // 落入 128 B size class
// ...
slab.deallocate(p, 72);
```

调用链：

```
① [Thread A] slab.allocate(72)
   ├── 查 size class table → index = 3 (128 B class)
   ├── pools_[3].allocate()
   │   ├── lock_guard(mtx_)
   │   ├── 调用底层 FixedSizePool::allocate()
   │   └── unlock
   └── return ptr

② [Thread B] slab.allocate(800) 同时进行
   ├── 落入 1024 B class，即 pools_[7]
   ├── 与 Thread A 不同 mutex，无竞争
   └── return ptr

③ [Thread A] slab.deallocate(p, 72)
   ├── 同样查 index = 3
   ├── pools_[3].deallocate(p)
   └── lock + push free list
```

**性能特性**：不同 size class 不共享 mutex，水平扩展性好。同 class 内仍有 mutex 串行，但每个 push/pop 是几条指令，开销可忽略。

### 4.3 场景 C：对象大小超过 4 KB

```
slab.allocate(8000) → fallback：
   return ::operator new(8000)
slab.deallocate(p, 8000) →
   ::operator delete(p)
```

设计取舍：超过 4 KB 的对象通常是 vector buffer 类，频率低、生命周期长，走通用分配器更划算。如果未来出现 8 KB+ 高频对象，可加 size class。

**三场景的共同心得**：“快” 不是「池本身多高明」，而是「被跳过了多少 malloc 本来要做的事」。热路径上从 50ns（malloc）降到 5ns（FixedPool），后者仅由两条指针写入构成。

---

## 5. 第 4 步 — 写 FixedSizePool::allocate / deallocate（free-list 头插 + LIFO）

这段代码论证了：**为什么 allocate / deallocate 能稳定 O(1)**——free-list 是单链表，「分配」就是摘下表头，「释放」就是插回表头，没有任何遍历或排序。

```cpp
// src/include/memory/MemoryPool.h —— FixedSizePool<T, BlockSize>::allocate
T *allocate() {
    if (!freeList_) {
        allocateBlock();             // 一次预分配 BlockSize 个 slot
    }
    FreeNode *node = freeList_;
    freeList_ = node->next;          // 摘下表头
    ++totalAllocated_;
    return reinterpret_cast<T *>(node);
}

void deallocate(T *ptr) noexcept {
    if (!ptr) return;
    FreeNode *node = reinterpret_cast<FreeNode *>(ptr);
    node->next = freeList_;          // 插回表头（LIFO）
    freeList_ = node;
    ++totalFreed_;
}
```

关键点：`FreeNode` 与 `T` 共享同一段内存——空闲时这段内存被解释为 `next` 指针，使用时被 `placement new` 解释为 `T`。这是 free-list 分配器的精髓：**没有任何额外元数据开销**，每个 slot 都被「物尽其用」。LIFO（后释放的先被分配）也带来缓存友好性：刚释放的内存仍在 L1 cache。

**验证：一次 alloc-dealloc-alloc 的内存地址追踪**

```cpp
FixedSizePool<HttpRequest> pool;
auto* p1 = pool.allocate();   // 许 0x7f...0100 （首次 expand 后的首个 slot）
pool.deallocate(p1);          // p1 被推回表头
auto* p2 = pool.allocate();   // 返回 0x7f...0100，与 p1 同址
assert(p1 == p2);             // 成立——这是 LIFO 的直接证据
```

由于 p2 与 p1 同地址，对应的 cache line 在上次 dealloc 后仍驻留在 L1，连续多次处理类似 HTTP 请求时能稳定吃到缓存。

---

## 6. 第 5 步 — 写 allocateBlock（为什么 max(sizeof(T), sizeof(FreeNode)) 不可省）

这段代码论证了：**为什么我们说「`new` 只在池耗尽时才被调用」**——`allocateBlock` 一次买入一大块内存，再切碎挂到 free-list，平均下来每个对象的 `new` 开销分摊为 1/BlockSize。

```cpp
// src/include/memory/MemoryPool.h —— FixedSizePool<T, BlockSize>::allocateBlock
void allocateBlock() {
    constexpr std::size_t slotSize = std::max(sizeof(T), sizeof(FreeNode));
    void *block = ::operator new(slotSize * BlockSize);
    blocks_.push_back(block);                       // 留待析构时释放

    auto *base = static_cast<char *>(block);
    for (std::size_t i = 0; i < BlockSize - 1; ++i) {
        auto *cur = reinterpret_cast<FreeNode *>(base + i * slotSize);
        cur->next = reinterpret_cast<FreeNode *>(base + (i + 1) * slotSize);
    }
    auto *last = reinterpret_cast<FreeNode *>(base + (BlockSize - 1) * slotSize);
    last->next = freeList_;                         // 接到现有 free-list 上
    freeList_ = reinterpret_cast<FreeNode *>(base);
}
```

关键点：`std::max(sizeof(T), sizeof(FreeNode))` 是处理「`T` 比指针还小」（如 `bool`）情形的关键——slot 必须至少能装下一个 `next` 指针。新 block 的最后一个 slot 接到旧 `freeList_` 上，保证多次 `allocateBlock` 后整个池仍是一条链表，O(1) 性质不被破坏。

---

## 7. 第 6 步 — 写 ConcurrentFixedSizePool（锁的最小化覆盖）

这段代码论证了：**为什么「加 mutex」并不等于「线程安全的代价就完全无解」**——只要 critical section 短到只剩两条赋值，吞吐损失就有限。

```cpp
// src/include/memory/MemoryPool.h —— ConcurrentFixedSizePool 摘录
template <typename T, std::size_t BlockSize = 1024>
class ConcurrentFixedSizePool {
  public:
    T *allocate() {
        std::lock_guard<std::mutex> lock(mu_);
        return pool_.allocate();           // 复用 FixedSizePool
    }
    void deallocate(T *ptr) noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        pool_.deallocate(ptr);
    }
  private:
    mutable std::mutex mu_;
    FixedSizePool<T, BlockSize> pool_;
};
```

关键点：critical section 只包含一次指针操作，竞争窗口约 10ns。**真正的优化方向是 thread-local cache**（仿 tcmalloc，已记录在「已知限制」），但作为 day35 的最小可用版本，这种「最薄锁」让 8 线程压测仍能保持 ~50 ns/op，完全不影响功能正确性。这也是为什么 `ConcurrentFixedSizePoolTest.ThreadSafety` 用 8×10000 的强度仍能跑过且无 leak。

---

## 8. 第 7 步 — 验证：析构顺序与生命周期不变量

### 8.1 FixedSizePool 析构

```
~FixedSizePool()
├── 检查 used_ > 0：日志 warning（用户忘了 deallocate，潜在泄漏）
└── blocks_ vector 析构 → 每个 unique_ptr<FreeNode[]> 释放底层内存

注意：池**不会**调用 T 的析构函数。用户必须在 deallocate 之前手动 ~T()，
否则只是回收了内存但 T 内部的资源（如内嵌 std::string、文件句柄）会泄漏。
```

### 8.2 ConcurrentFixedSizePool / SlabAllocator

```
~ConcurrentFixedSizePool()
├── 隐式持有 mutex 与 impl_
├── 析构时无并发线程访问（前提），mutex 析构无竞争
└── impl_（FixedSizePool）析构释放 blocks_

~SlabAllocator()
└── 10 个 pool 依次析构
```

### 8.3 关键不变量

- 用户绝不能在两个不同的 pool / allocator 间交叉 deallocate（典型的 cross-allocator UB）
- `placement new` + `pool.deallocate()` 必须成对，且析构 T 在 deallocate 之前
- 多线程下，同一 ptr 不可并发 deallocate（与 std::free 同样的契约）

**验证：ASAN/LSAN 能才堆 leak 不能才堆 misuse**。池本身不追踪「指针从哪里来」，跨 pool deallocate 会看起来成功但实际产生 UB。未来可考虑在 debug 构建下加 sentinel（如池 ID + slot 校验）。

---

## 9. 第 8 步 — 写单元测试 + 看微基准

### 9.1 单元测试分布

| 套件 | 测试名 | 验证要点 |
|------|--------|---------|
| `FixedSizePoolTest` | `BasicAllocateDeallocate` | 单次分配释放 |
| | `ReuseAfterDeallocate` | 释放后再分配应得到同一地址（free list LIFO）|
| | `ExpandsWhenExhausted` | 超过 BlockSize 后自动扩展 |
| | `SmallType` | sizeof(T) < sizeof(void*) 时仍正确（union 处理）|
| | `LargeType` | 大对象 |
| | `StatsCorrect` | allocated / used / peak 计数 |
| | `MoveOnly` | 不要求 T 可拷贝 |
| `ConcurrentFixedSizePoolTest` | `ThreadSafety` | 8 线程 × 10000 alloc/dealloc，无 segfault，无 leak |
| | `StressNoLeak` | 长时间运行后 used_ 归零 |
| `SlabAllocatorTest` | `AllocateVariousSizes` | 不同 size 都能分配释放 |
| | `LargeAllocationFallback` | > 4KB 走 operator new |
| | `ReuseSameSizeClass` | 同 class 内复用 |
| `SlabSTLAllocatorTest` | `WorksWithVector` | std::vector<int, SlabSTLAllocator<int>> 正常 |
| | `WorksWithList` | std::list 节点分配 |

### 9.2 测试输出

```
$ cd build && ./MemoryPoolTest
[==========] Running 14 tests from 4 test suites.
[----------] 7 tests from FixedSizePoolTest
[       OK ] FixedSizePoolTest.SmallType (0 ms)
[       OK ] FixedSizePoolTest.ExpandsWhenExhausted (1 ms)
[----------] 2 tests from ConcurrentFixedSizePoolTest
[       OK ] ConcurrentFixedSizePoolTest.ThreadSafety (8 ms)
[----------] 3 tests from SlabAllocatorTest
[----------] 2 tests from SlabSTLAllocatorTest
[==========] 14 tests from 4 test suites ran. (14 ms total)
[  PASSED  ] 14 tests.
```

### 9.3 验证：微基准对比 malloc

`FixedPoolVsMalloc` 子测试（手工运行，未在 CI）：

| 操作 | 时间（百万次） | 比 malloc 快 |
|------|---------------|------------|
| `new T / delete` | 145 ms | 1.0× |
| `FixedSizePool<T>` | 126 ms | **1.15×** |

加速比看似不大，是因为 macOS 的 jemalloc 已经非常快。Linux glibc malloc 上理论加速更大（社区数据 2-3×），CI 未跑该 benchmark。

### 9.4 验证：ASAN 的「零泄漏」作为最低门槛

```bash
$ cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -g -O1" -B build-asan && cmake --build build-asan
$ ./build-asan/MemoryPoolTest
[  PASSED  ] 14 tests.
# ASAN 末尾报告：0 leaks
```

只要测试里任何一个忘了 `pool.deallocate()`，ASAN 都会立刻点名线索。这是用 `unique_ptr` 带池作为 deleter 的极大价值。

---

## 10. 局限与下一步

- **无 thread-cache**：每次 allocate/deallocate 都过 mutex；高并发下 ConcurrentFixedSizePool 是瓶颈。仿 tcmalloc 加 thread-local 一级缓存可显著提升
- **无对齐参数**：union FreeNode 的对齐继承自 T，但如果 T 内部有 16/32/64 byte 对齐要求（SIMD），未做特殊处理；可加 `alignas(alignof(T))`
- **不替换 new/delete**：保持非侵入。日后若需要全局替换，可在 main() 入口前 hook `operator new`
- **无统计 dashboard**：只暴露简单计数。生产场景应导出到 prometheus（与 day25 metrics 集成）
- **size class 固定**：10 个 class 不可调整，未来可做成 constexpr 数组+模板

