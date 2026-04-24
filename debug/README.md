# 高性能网络编程：现代调试工具全景实验手册

> 本手册配合 `buggy_server.cpp`（含 Bug）和 `debugged_server.cpp`（待修复骨架）使用。
> 通过 **8 个精心设计的实验**，系统掌握工业级调试与性能分析工具链。
>
> **实验流程**：运行 buggy_server → 使用工具观测/诊断 → 理解输出 → 将修复代码写入 debugged_server → 验证修复。

---

## 工具全景速查表

| 工具 | 检测/分析目标 | 对应实验 | Linux | macOS 替代 |
|------|-------------|---------|-------|-----------|
| **AddressSanitizer (ASan)** | UAF、越界、内存泄漏 | 1.1, 1.2 | GCC/Clang `-fsanitize=address` | 同左 (Apple Clang) |
| **ThreadSanitizer (TSan)** | 数据竞争、锁序反转 | 2.1, 2.2 | GCC/Clang `-fsanitize=thread` | 同左 (Apple Clang) |
| **UBSan** | 整数溢出、数组越界等 UB | 3 | GCC/Clang `-fsanitize=undefined` | 同左 |
| **GDB / LLDB** | 崩溃分析、Core Dump、断点 | 4, (2.2) | `gdb` | `lldb` (Xcode 自带) |
| **perf + FlameGraph** | CPU 热点、缓存失效 | 5 | `perf record/report` | Instruments / `sample` |
| **strace** | 系统调用追踪 | 6 | `strace -p <PID>` | `dtruss` (需 SIP 关闭) |
| **tcpdump** | 网络抓包 | 7 | `tcpdump -i lo` | `tcpdump -i lo0` |
| **lsof / ss** | 端口占用、FD 泄漏 | 1.2, 7 | `lsof` / `ss -tlnp` | `lsof` / `netstat -an` |
| **Valgrind** | 内存泄漏 (ASan 替代) | 附录 | `valgrind --leak-check=full` | 不支持 ARM Mac |
| **eBPF / bpftrace** | 内核级动态追踪 | 附录 | `bpftrace` | DTrace |

---

## 环境准备

### 工具安装 (Ubuntu/Debian)

```bash
# 编译器 (GCC 或 Clang, 二选一即可, 推荐 Clang)
sudo apt install build-essential cmake
# 或: sudo apt install clang

# 调试器
sudo apt install gdb

# 性能分析
sudo apt install linux-tools-common linux-tools-$(uname -r)

# FlameGraph 脚本
git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph

# 网络/系统工具 (通常已预装)
sudo apt install strace tcpdump lsof iproute2 netcat-openbsd

# (可选) Valgrind
sudo apt install valgrind

# (可选) bpftrace
sudo apt install bpftrace
```

### 工具安装 (macOS)

```bash
# Xcode Command Line Tools (含 Apple Clang + LLDB)
xcode-select --install

# CMake
brew install cmake

# tcpdump 已预装; netcat 已预装 (nc)
# lsof 已预装
```

### 构建命令速查

```bash
cd debug/

# ASan 构建 (实验 1.1, 1.2)
cmake -B build-asan -DUSE_ASAN=ON && cmake --build build-asan

# TSan 构建 (实验 2.1, 2.2)
cmake -B build-tsan -DUSE_TSAN=ON && cmake --build build-tsan

# UBSan 构建 (实验 3)
cmake -B build-ubsan -DUSE_UBSAN=ON && cmake --build build-ubsan

# 普通 Debug 构建 (实验 4, 5, 6, 7)
cmake -B build-debug && cmake --build build-debug
```

---

## 实验 1.1: AddressSanitizer — Use-After-Free

**使用工具**: ASan (`-fsanitize=address`)
**运行模式**: `./buggy_server uaf`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — simulate_uaf()
void simulate_uaf() {
    Connection* conn = new Connection(5);

    std::thread worker([conn]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        conn->handleMessage();    // <-- 100ms 后访问 conn
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    delete conn;                  // <-- 50ms 后已经 delete

    worker.join();
}
```

**发生的异常**：主线程在 50ms 时 `delete conn`，但 worker 线程在 100ms 时仍然通过悬空指针调用 `conn->handleMessage()`。这是经典的 **Use-After-Free (UAF)**——在网络服务器中极为常见：一个连接被关闭（释放），而另一个线程/回调仍持有指向它的指针。

不使用 ASan 时，程序**可能**看起来正常运行（未定义行为的危险之处），也可能随机崩溃或产生数据损坏。

### 2. 实验步骤

```bash
# Step 1: 以 ASan 编译
cmake -B build-asan -DUSE_ASAN=ON && cmake --build build-asan

# Step 2: 运行
./build-asan/buggy_server uaf
```

### 3. 输出解读与诊断思路

ASan 会输出类似如下的报告（关键信息已标注）：

```
=================================================================
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010
WRITE of size 4 at 0x602000000010 thread T1              ← [A] 线程 T1 写入了已释放的地址
    #0 0x... in Connection::handleMessage() buggy_server.cpp:31  ← [B] 出问题的代码位置
    #1 0x... in simulate_uaf()::$_0::operator()() buggy_server.cpp:48

0x602000000010 is located 4 bytes inside of 24-byte region [0x60200000000c,0x602000000024)
freed by thread T0 here:                                  ← [C] 被谁释放的
    #0 0x... in operator delete(void*)
    #1 0x... in simulate_uaf() buggy_server.cpp:53        ← [D] 释放位置 (delete conn)

previously allocated by thread T0 here:                   ← [E] 最初在哪分配的
    #0 0x... in operator new(unsigned long)
    #1 0x... in simulate_uaf() buggy_server.cpp:44        ← [F] 分配位置 (new Connection)

SUMMARY: AddressSanitizer: heap-use-after-free buggy_server.cpp:31 in Connection::handleMessage()
```

**诊断思路（假设你不知道 Bug 在哪）**：

1. **看错误类型 [A]**：`heap-use-after-free` → 程序访问了已经释放的堆内存
2. **看访问位置 [B]**：`Connection::handleMessage()` 在 thread T1 → worker 线程在操作 Connection
3. **看释放位置 [C][D]**：`simulate_uaf()` 第 53 行的 `delete` 由 thread T0 执行 → 主线程释放了对象
4. **看分配位置 [E][F]**：同样在 `simulate_uaf()` 的 `new Connection` → 主线程创建的
5. **得出结论**：主线程创建了对象，又在 worker 还在使用时提前释放了它。需要保证对象活到所有使用者都结束 → 使用共享所有权（`shared_ptr`）

### 4. 修复方案 (写入 debugged_server.cpp 的 `simulate_uaf`)

```cpp
void simulate_uaf() {
    std::cout << "\n=== [FIXED] 实验 1.1: Use-After-Free ===\n";

    auto conn = std::make_shared<Connection>(5);

    std::thread worker([conn]() {   // shared_ptr 按值捕获, 引用计数 +1
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        conn->handleMessage();      // worker 持有自己的 shared_ptr 副本, 安全
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "[Main] Releasing main reference...\n";
    conn.reset();   // 主线程释放引用, 但 worker 的副本仍持有对象

    worker.join();
    std::cout << "[Main] Worker joined. No UAF.\n";
}
```

**核心原理**：`shared_ptr` 通过引用计数保证对象在最后一个持有者释放后才被销毁。Lambda 按值捕获 `shared_ptr` 意味着 worker 线程拥有独立的引用。

### 5. 验证修复

```bash
# 写入修复代码后, 重新编译并运行
cmake --build build-asan
./build-asan/debugged_server uaf
# 预期: 无 ASan 错误, 正常输出
```

---

## 实验 1.2: AddressSanitizer + lsof — 内存泄漏与 FD 泄漏

**使用工具**: ASan (LeakSanitizer) + `lsof`
**运行模式**: `./buggy_server leak`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — simulate_leak()
void simulate_leak() {
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        Connection* c = new Connection(fd);
        c->handleMessage();
        // BUG: 既不 delete c (内存泄漏), 也不 close(fd) (FD 泄漏)
    }
    // 循环结束, 100 个 Connection* 指针丢失, 100 个 socket fd 未关闭
}
```

**发生的异常**：
- **内存泄漏**：100 个 `Connection` 对象分配在堆上，指针在每次循环后丢失，永远无法释放
- **FD 泄漏**：100 个 socket 文件描述符从未被 `close()`，操作系统的 FD 资源被耗尽

在真实服务器中，FD 泄漏比内存泄漏更致命——达到 `ulimit -n` 上限后，服务器将无法 `accept` 新连接。

### 2. 实验步骤

```bash
# Step 1: ASan 编译 (开启泄漏检测)
cmake -B build-asan -DUSE_ASAN=ON && cmake --build build-asan

# Step 2: 运行 (程序会暂停等待 Enter)
ASAN_OPTIONS=detect_leaks=1 ./build-asan/buggy_server leak

# Step 3: 在程序暂停期间, 打开另一个终端观察 FD 泄漏
lsof -p $(pgrep buggy_server) | tail -20     # 查看进程打开的文件描述符
lsof -p $(pgrep buggy_server) | grep -c sock # 统计泄漏的 socket 数量

# Step 4: 回到第一个终端, 按 Enter 让程序退出
# ASan 在进程退出时输出泄漏报告
```

### 3. 输出解读与诊断思路

**Phase 1: lsof 输出 (程序运行中)**

```
COMMAND     PID USER   FD   TYPE  DEVICE SIZE/OFF NODE NAME
buggy_ser 12345 user    3u  sock   0,9      0t0  ... protocol: TCP
buggy_ser 12345 user    4u  sock   0,9      0t0  ... protocol: TCP
buggy_ser 12345 user    5u  sock   0,9      0t0  ... protocol: TCP
...                                                    (100 行 sock)
```

→ **诊断**：进程持有大量 `sock` 类型的 FD，且这些 socket 都没有绑定/连接任何地址。说明 `socket()` 被调用但从未 `close()`。

**Phase 2: ASan LeakSanitizer 输出 (程序退出后)**

```
=================================================================
==12345==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 2400 byte(s) in 100 object(s) allocated from:
    #0 0x... in operator new(unsigned long)
    #1 0x... in simulate_leak() buggy_server.cpp:72

SUMMARY: AddressSanitizer: 2400 byte(s) leaked in 100 allocation(s).
```

→ **诊断**：100 个对象 × 24 字节 = 2400 字节泄漏。分配在 `simulate_leak()` 的 `new Connection(fd)` 处。

**完整诊断思路**：
1. lsof 发现大量未使用的 socket FD → FD 资源泄漏
2. ASan 报告 100 个 `new Connection` 未释放 → 内存泄漏
3. 两者结合 → `Connection` 没有被正确管理（RAII 缺失）
4. 修复方向：智能指针自动管理生命周期 + 析构函数关闭 FD

### 4. 修复方案

**首先修改 Connection 的析构函数** (在 `debugged_server.cpp` 的 `struct Connection` 中):

```cpp
~Connection() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    std::cout << "[Connection fd=" << fd << "] Destroyed\n";
}
```

**然后修复 simulate_leak** (在 `debugged_server.cpp` 中):

```cpp
void simulate_leak() {
    std::cout << "\n=== [FIXED] 实验 1.2: Memory Leak + FD Leak ===\n";
    std::cout << "[Main] PID = " << getpid() << "\n";

    std::vector<std::unique_ptr<Connection>> pool;
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); break; }
        pool.push_back(std::make_unique<Connection>(fd));
        pool.back()->handleMessage();
    }

    std::cout << "\n[Main] " << pool.size() << " connections created.\n";
    std::cout << "  lsof -p " << getpid() << " | grep -c sock\n";
    std::cout << "Press Enter to exit...\n";
    std::cin.get();
    // pool 离开作用域 → unique_ptr 自动 delete → ~Connection() 调用 close(fd)
}
```

**核心原理**：RAII (Resource Acquisition Is Initialization)——将资源获取（`socket()`）与对象绑定，在析构中释放（`close(fd)`）。`unique_ptr` 保证对象一定被销毁。

### 5. 验证修复

```bash
ASAN_OPTIONS=detect_leaks=1 ./build-asan/debugged_server leak
# 暂停时 lsof 检查: socket 数量应为 100 (仍在使用)
# 按 Enter 后: ASan 无泄漏报告, 析构函数输出 close 信息
```

---

## 实验 2.1: ThreadSanitizer — 数据竞争

**使用工具**: TSan (`-fsanitize=thread`)
**运行模式**: `./buggy_server race`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — Connection 结构体 + simulate_race()
struct Connection {
    int bytes_processed = 0;   // 普通 int, 无同步保护
    // ...
};

void simulate_race() {
    Connection conn(5);

    std::thread t1([&conn]() {
        for (int i = 0; i < 100000; i++)
            conn.bytes_processed++;    // 线程 1 写入
    });
    std::thread t2([&conn]() {
        for (int i = 0; i < 100000; i++)
            conn.bytes_processed++;    // 线程 2 同时写入
    });

    t1.join();
    t2.join();
    // 期望 200000, 实际结果不确定
}
```

**发生的异常**：两个线程同时对同一个非原子 `int` 执行 `++` 操作。`++` 并非原子操作——它包含「读→加→写」三步。两个线程可能同时读到相同的值，各自加 1 后写回，导致增量丢失。最终结果 < 200000。

这是网络服务器中最隐蔽的 Bug 类型：统计计数器、连接数、字节数等共享状态如果不做同步，在高并发下会产生数据不一致。

### 2. 实验步骤

```bash
# Step 1: TSan 编译
cmake -B build-tsan -DUSE_TSAN=ON && cmake --build build-tsan

# Step 2: 运行
./build-tsan/buggy_server race
```

### 3. 输出解读与诊断思路

```
==================
WARNING: ThreadSanitizer: data race (pid=12345)
  Write of size 4 at 0x7ffd... by thread T2:           ← [A] 线程 T2 在写
    #0 simulate_race()::$_1::operator()() const buggy_server.cpp:93

  Previous write of size 4 at 0x7ffd... by thread T1:  ← [B] 线程 T1 也在写同一地址
    #0 simulate_race()::$_0::operator()() const buggy_server.cpp:89

  Location is stack of main thread.                     ← [C] 变量在主线程栈上

SUMMARY: ThreadSanitizer: data race buggy_server.cpp:93
==================
```

**诊断思路**：
1. **[A][B]**：两个线程对同一地址（同一变量）执行写操作 → 经典数据竞争
2. **看行号**：89 行和 93 行都是 `conn.bytes_processed++` → 共享变量 `bytes_processed` 未受保护
3. **[C]**：变量在主线程栈上（`conn` 是局部变量）
4. **结论**：需要让 `bytes_processed` 的读写成为原子操作 → `std::atomic<int>` 或 `std::mutex`

### 4. 修复方案

**首先修改 Connection 结构体** (在 `debugged_server.cpp` 顶部):

```cpp
struct Connection {
    int fd;
    std::atomic<int> bytes_processed{0};   // 原子变量, 线程安全
    // ... (其余不变)
};
```

**然后修复 simulate_race** (在 `debugged_server.cpp` 中):

```cpp
void simulate_race() {
    std::cout << "\n=== [FIXED] 实验 2.1: Data Race ===\n";
    Connection conn(5);

    std::thread t1([&conn]() {
        for (int i = 0; i < 100000; i++)
            conn.bytes_processed.fetch_add(1, std::memory_order_relaxed);
    });
    std::thread t2([&conn]() {
        for (int i = 0; i < 100000; i++)
            conn.bytes_processed.fetch_add(1, std::memory_order_relaxed);
    });

    t1.join();
    t2.join();
    std::cout << "[Main] bytes_processed = " << conn.bytes_processed.load()
              << " (expected 200000)\n";
}
```

**核心原理**：`std::atomic<int>` 保证读写操作的原子性。`memory_order_relaxed` 在此场景足够——我们只需要计数准确，不需要与其他变量建立 happens-before 关系。相比 `mutex`，原子操作的开销小得多，适合高频计数器。

### 5. 验证修复

```bash
./build-tsan/debugged_server race
# 预期: 无 TSan 警告, bytes_processed = 200000
```

---

## 实验 2.2: ThreadSanitizer + GDB — 死锁

**使用工具**: TSan (预防性检测) + GDB/LLDB (现场分析)
**运行模式**: `./buggy_server deadlock`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — simulate_deadlock()
std::mutex mtx_connMgr;   // 锁 A: 连接管理器
std::mutex mtx_logger;    // 锁 B: 日志系统

void simulate_deadlock() {
    std::thread t1([]() {
        std::lock_guard<std::mutex> lk1(mtx_connMgr);   // T1: 先锁 A
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk2(mtx_logger);    // T1: 再锁 B
    });

    std::thread t2([]() {
        std::lock_guard<std::mutex> lk1(mtx_logger);    // T2: 先锁 B
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk2(mtx_connMgr);   // T2: 再锁 A → 死锁!
    });

    t1.join();
    t2.join();
}
```

**发生的异常**：经典的 **AB-BA 死锁**。T1 持有 A 等待 B，T2 持有 B 等待 A，双方互相等待，永远无法推进。程序会 **卡住不动**（hang）。

在真实服务器中，这种死锁可能在高并发场景下偶发触发，且没有任何错误日志——服务器只是停止响应请求。

### 2. 实验步骤

#### 方法 A: TSan 预防性检测 (推荐先做)

TSan 能检测 **潜在的** 锁序反转，即使本次运行没有真正死锁：

```bash
# TSan 编译并运行
./build-tsan/buggy_server deadlock
```

#### 方法 B: GDB/LLDB 分析实际死锁

```bash
# 普通编译并运行 (程序会卡住)
./build-debug/buggy_server deadlock &
DEADLOCK_PID=$!

# 等待 2 秒让死锁发生
sleep 2

# 用 GDB 附加到挂起的进程 (Linux)
sudo gdb -p $DEADLOCK_PID

# 或用 LLDB 附加 (macOS)
# lldb -p $DEADLOCK_PID
```

在 GDB 中执行：

```
(gdb) info threads                    # 查看所有线程状态
(gdb) thread apply all bt             # 打印所有线程的调用栈
```

完成后：

```
(gdb) kill                            # 杀死目标进程
(gdb) quit
```

### 3. 输出解读与诊断思路

#### TSan 输出

```
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=12345)
  Cycle in lock order graph: M0 (0x...) => M1 (0x...) => M0  ← [A] 锁环

  Mutex M1 acquired here while holding mutex M0 in thread T1:  ← [B]
    #0 pthread_mutex_lock
    #1 std::mutex::lock()
    #2 simulate_deadlock()::$_0::operator()() buggy_server.cpp:114

  Mutex M0 previously acquired by the same thread here:        ← [C]
    #0 pthread_mutex_lock
    #1 std::mutex::lock()
    #2 simulate_deadlock()::$_0::operator()() buggy_server.cpp:112

  Mutex M0 acquired here while holding mutex M1 in thread T2:  ← [D] 反序!
    ...simulate_deadlock()::$_1::operator()() buggy_server.cpp:120
```

**诊断思路**：
1. **[A]** `Cycle: M0 => M1 => M0` → 存在锁序环路
2. **[B][C]** T1: 先获取 M0 (connMgr)，再获取 M1 (logger)
3. **[D]** T2: 先获取 M1 (logger)，再获取 M0 (connMgr) → 顺序相反
4. **结论**：两线程以不同顺序获取同一组锁 → 需要统一锁序或一次性获取

#### GDB `thread apply all bt` 输出

```
Thread 3 (LWP 12347):
#0  __lll_lock_wait () ...
#1  pthread_mutex_lock ()
#2  std::mutex::lock() ...
#3  simulate_deadlock()::$_1::operator()() at buggy_server.cpp:120  ← T2 卡在获取 connMgr

Thread 2 (LWP 12346):
#0  __lll_lock_wait () ...
#1  pthread_mutex_lock ()
#2  std::mutex::lock() ...
#3  simulate_deadlock()::$_0::operator()() at buggy_server.cpp:114  ← T1 卡在获取 logger
```

→ 两个线程都阻塞在 `pthread_mutex_lock`，互相等待对方持有的锁 → 死锁确认。

### 4. 修复方案 (写入 debugged_server.cpp 的 `simulate_deadlock`)

```cpp
void simulate_deadlock() {
    std::cout << "\n=== [FIXED] 实验 2.2: Deadlock ===\n";

    std::thread t1([]() {
        std::scoped_lock lock(mtx_connMgr, mtx_logger);  // 一次性获取两把锁
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::cout << "[T1] Got both locks.\n";
    });

    std::thread t2([]() {
        std::scoped_lock lock(mtx_connMgr, mtx_logger);  // 顺序无所谓, scoped_lock 内部避免死锁
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::cout << "[T2] Got both locks.\n";
    });

    t1.join();
    t2.join();
    std::cout << "[Main] Both threads finished. No deadlock.\n";
}
```

**核心原理**：`std::scoped_lock` (C++17) 内部使用 `std::lock()` 算法，以不会死锁的方式同时获取多把锁（本质是 try-and-back-off 策略）。无论参数顺序如何，都不会产生 AB-BA 死锁。

### 5. 验证修复

```bash
./build-tsan/debugged_server deadlock
# 预期: 无 TSan 警告, 无挂起, 正常退出
```

---

## 实验 3: UndefinedBehaviorSanitizer — 未定义行为

**使用工具**: UBSan (`-fsanitize=undefined`)
**运行模式**: `./buggy_server ubsan`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — simulate_ubsan()
void simulate_ubsan() {
    // BUG 1: 有符号整数溢出 (C++ 标准明确为 UB)
    int packet_size = 0x7FFFFFFF;   // INT_MAX = 2147483647
    packet_size += 1;               // 溢出! UB!

    // BUG 2: C 数组越界访问
    int buffer[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int idx = 4;                    // 合法下标: 0~3
    std::cout << buffer[idx] << "\n";  // 越界! UB!
}
```

**发生的异常**：
- **有符号整数溢出**：不同于无符号整数的 wraparound，有符号溢出在 C++ 标准中是 **未定义行为**。编译器可以假设它不会发生，从而做出激进优化（如删除某些条件判断），导致程序行为完全不可预期。
- **数组越界**：访问 `buffer[4]` 读取了栈上其他变量或返回地址的内存，可能看到垃圾值，也可能触发安全漏洞。

在网络编程中，整数溢出常见于数据包长度计算 (`content_length + header_size`)，越界常见于缓冲区解析。

### 2. 实验步骤

```bash
# Step 1: UBSan 编译
cmake -B build-ubsan -DUSE_UBSAN=ON && cmake --build build-ubsan

# Step 2: 运行
./build-ubsan/buggy_server ubsan
```

### 3. 输出解读与诊断思路

```
buggy_server.cpp:133:17: runtime error: signed integer overflow:
  2147483647 + 1 cannot be represented in type 'int'      ← [A]

buggy_server.cpp:137:26: runtime error: index 4 out of bounds
  for type 'int [4]'                                       ← [B]
```

**诊断思路**：
1. **[A]** 精确定位到行号和溢出表达式 `2147483647 + 1` → 检查该变量的上游来源
2. **[B]** 数组 `int[4]` 被下标 4 访问 → 检查下标计算逻辑
3. **这类问题在没有 UBSan 的情况下往往无任何症状**，但可能在不同编译器/优化级别下突然爆发

### 4. 修复方案 (写入 debugged_server.cpp 的 `simulate_ubsan`)

```cpp
void simulate_ubsan() {
    std::cout << "\n=== [FIXED] 实验 3: Undefined Behavior ===\n";

    // Fix 1: 溢出前检查边界
    int packet_size = std::numeric_limits<int>::max();
    if (packet_size < std::numeric_limits<int>::max()) {
        packet_size += 1;
    } else {
        std::cout << "[Safe] packet_size is at INT_MAX, skip increment.\n";
    }
    std::cout << "[Safe] packet_size = " << packet_size << "\n";

    // Fix 2: 使用 std::vector + at() 获得边界检查
    std::vector<int> buffer = {0xAA, 0xBB, 0xCC, 0xDD};
    size_t idx = 4;
    try {
        std::cout << "[Safe] buffer.at(" << idx << ") = " << buffer.at(idx) << "\n";
    } catch (const std::out_of_range& e) {
        std::cout << "[Safe] Caught out-of-range: " << e.what() << "\n";
    }
}
```

**核心原理**：
- 整数溢出：在算术运算前检查是否会越界。真实项目中可使用 `__builtin_add_overflow()` (GCC/Clang 内建) 进行安全加法。
- 数组越界：使用 `std::vector::at()` 代替裸数组下标访问，提供运行时边界检查。

### 5. 验证修复

```bash
./build-ubsan/debugged_server ubsan
# 预期: 无 UBSan 错误, 输出 "Caught out-of-range"
```

---

## 实验 4: GDB/LLDB — 崩溃分析与 Core Dump

**使用工具**: GDB (Linux) / LLDB (macOS)
**运行模式**: `./buggy_server crash`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — 多层调用栈中的空指针
void parse_header(char* buf, int len) {
    std::memcpy(buf, "HTTP/1.1 200 OK\r\n", 17);   // buf 是 null → SIGSEGV
}

void process_request(char* buf, int len) {
    parse_header(buf, len);
}

void handle_client(int fd) {
    char* response_buf = nullptr;      // BUG: 忘记分配内存!
    process_request(response_buf, 256);
}

void simulate_crash() {
    handle_client(42);
}
```

**发生的异常**：`handle_client` 声明了指针但未分配内存，传递 `nullptr` 经过两层函数后在 `parse_header` 中的 `memcpy` 对空地址写入，触发 **Segmentation Fault (SIGSEGV)**。

在真实项目中，空指针的来源可能在很远的上游（配置错误、条件分支遗漏等），调用栈分析是定位它的关键。

### 2. 实验步骤

#### 方法 A: 交互式调试（推荐）

**Linux (GDB):**

```bash
gdb ./build-debug/buggy_server
```

在 GDB 提示符中：

```
(gdb) run crash
# 程序崩溃, GDB 自动停在崩溃点

(gdb) bt
# 查看完整调用栈 (backtrace)

(gdb) frame 0
# 切换到最内层栈帧 (parse_header)

(gdb) print buf
# 查看 buf 的值 → 0x0 (null)

(gdb) frame 2
# 向上两层到 handle_client

(gdb) info locals
# 查看局部变量 → response_buf = 0x0

(gdb) quit
```

**macOS (LLDB):**

```bash
lldb ./build-debug/buggy_server
```

在 LLDB 提示符中：

```
(lldb) run crash
# 程序崩溃, LLDB 自动停在 EXC_BAD_ACCESS

(lldb) bt
# 查看调用栈

(lldb) frame select 0
# 切换到 parse_header 帧

(lldb) p buf
# 查看 buf → 0x0000000000000000

(lldb) frame select 2
# 切换到 handle_client

(lldb) frame variable
# 查看所有局部变量

(lldb) quit
```

#### 方法 B: Core Dump 分析（Linux，模拟线上环境）

```bash
# 启用 core dump
ulimit -c unlimited
echo "core.%e.%p" | sudo tee /proc/sys/kernel/core_pattern

# 运行崩溃程序
./build-debug/buggy_server crash
# 产生 core 文件: core.buggy_server.<PID>

# 事后分析 (验尸)
gdb ./build-debug/buggy_server core.buggy_server.*
(gdb) bt
(gdb) frame 0
(gdb) print buf
(gdb) quit
```

### 3. 输出解读与诊断思路

**`bt` (backtrace) 输出**:

```
#0  parse_header (buf=0x0, len=256) at buggy_server.cpp:145
#1  process_request (buf=0x0, len=256) at buggy_server.cpp:149
#2  handle_client (fd=42) at buggy_server.cpp:153
#3  simulate_crash () at buggy_server.cpp:159
#4  main (argc=2, argv=0x...) at buggy_server.cpp:187
```

**诊断思路（假设你不知道 Bug 在哪）**：
1. **看崩溃点 #0**：`parse_header` 中 `buf=0x0` → 空指针导致 memcpy 崩溃
2. **向上追溯 #1**：`process_request` 也是 `buf=0x0` → 它只是透传，不是根因
3. **继续追溯 #2**：`handle_client` → 这里创建了 `response_buf`
4. **看 `info locals`**：`response_buf = 0x0` → 指针声明了但从未赋值
5. **根因定位**：`handle_client` 忘记分配内存。`char* response_buf = nullptr;` 后应该 `new` 或使用栈/vector 分配

### 4. 修复方案 (写入 debugged_server.cpp 的 `simulate_crash` 及相关函数)

```cpp
void parse_header(char* buf, int len) {
    if (!buf || len < 17) {
        std::cerr << "[Error] parse_header: invalid buffer\n";
        return;
    }
    std::memcpy(buf, "HTTP/1.1 200 OK\r\n", 17);
}

void process_request(char* buf, int len) {
    parse_header(buf, len);
}

void handle_client(int fd) {
    std::vector<char> response_buf(256, 0);          // 使用 vector 自动管理内存
    process_request(response_buf.data(), static_cast<int>(response_buf.size()));
    std::cout << "[Client fd=" << fd << "] Response: "
              << std::string(response_buf.data(), 17) << "\n";
}

void simulate_crash() {
    std::cout << "\n=== [FIXED] 实验 4: Segfault ===\n";
    handle_client(42);
}
```

**核心原理**：
- 使用 `std::vector<char>` 代替裸指针，自动分配和释放内存
- 在底层函数添加防御性空指针检查，实践**"不信任上游输入"**原则
- 真实项目中，宁可提前返回错误也不要解引用可疑指针

### 5. 验证修复

```bash
./build-debug/debugged_server crash
# 预期: 正常输出 "Response: HTTP/1.1 200 OK", 无崩溃
```

---

## 实验 5: perf + FlameGraph — CPU 性能热点分析

**使用工具**: `perf` (Linux) / Instruments (macOS)
**运行模式**: `./buggy_server cpu`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — 性能问题
std::string slow_find_header(std::string request, std::string header_name) {
    //                        ^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^
    //                        按值传参! 每次调用都拷贝整个 HTTP 请求字符串
    size_t pos = request.find(header_name);
    if (pos == std::string::npos) return "";
    size_t end = request.find("\r\n", pos);
    return request.substr(pos, end - pos);
}

void simulate_cpu_hog() {
    std::string http_request = "GET /api/v1/data HTTP/1.1\r\n..."; // ~250 字节

    for (int i = 0; i < 2000000; i++) {
        slow_find_header(http_request, "Host");
        // 每次迭代: 拷贝 http_request(~250B) + 拷贝 "Host"(5B) + substr 分配
        // 200万次 = ~500MB 的无效拷贝和内存分配/释放
    }
}
```

**发生的异常**：不是功能错误，而是**性能问题**。函数参数按值传递导致每次调用都产生字符串拷贝。在热路径（每秒调用百万次的 HTTP 解析函数）上，这会导致：
- CPU 时间大量浪费在 `std::string` 的构造/析构上
- 内存分配器（`malloc/free`）成为瓶颈
- 缓存效率低下

### 2. 实验步骤

#### Linux (perf + FlameGraph)

```bash
# Step 1: 使用 perf 记录 CPU 采样
perf record -g --call-graph dwarf ./build-debug/buggy_server cpu

# Step 2: 查看交互式报告
perf report
# 使用方向键浏览, Enter 展开, q 退出
# 观察哪个函数占用 CPU 最多

# Step 3: 生成火焰图 (需要先 clone FlameGraph 仓库)
perf script | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl > flamegraph.svg

# Step 4: 用浏览器打开火焰图
# firefox flamegraph.svg   或   xdg-open flamegraph.svg
```

#### macOS (Instruments 或 sample)

```bash
# 方法 1: sample 命令 (CLI, 快速)
./build-debug/buggy_server cpu &
CPU_PID=$!
sample $CPU_PID 3 -file cpu_profile.txt   # 采样 3 秒
cat cpu_profile.txt | head -100
kill $CPU_PID

# 方法 2: Instruments (GUI, 更详细)
# 1. 打开 Instruments.app (Cmd+Space 搜索 "Instruments")
# 2. 选择 "Time Profiler" 模板
# 3. 点左上角 Target → Choose Target → 选择 build-debug/buggy_server
# 4. 在 Arguments 添加 "cpu"
# 5. 点击录制按钮, 等程序运行完毕
# 6. 展开调用树, 查看各函数 CPU 占比
```

### 3. 输出解读与诊断思路

**`perf report` 典型输出**:

```
  Overhead  Command        Symbol
+  62.15%  buggy_server   std::__cxx11::basic_string<...>::basic_string(...)  ← [A]
+  18.42%  buggy_server   std::__cxx11::basic_string<...>::~basic_string()    ← [B]
+   8.67%  buggy_server   slow_find_header(...)                                ← [C]
+   5.12%  buggy_server   malloc
+   3.21%  buggy_server   free
```

**火焰图解读**：
- 火焰图的 X 轴是采样比例（宽度 = CPU 时间占比），Y 轴是调用栈深度
- 你会看到一个很宽的 `slow_find_header` 柱子，上面堆满了 `string::basic_string` (拷贝构造) 和 `~basic_string` (析构)

**诊断思路（假设你不知道为什么慢）**：
1. **[A]** 62% 时间在 string 拷贝构造 → 大量字符串被创建
2. **[B]** 18% 时间在 string 析构 → 与 [A] 对应，创建后又销毁
3. **[C]** 这些都发生在 `slow_find_header` 内 → 这个函数是热点
4. **追问**：为什么 `slow_find_header` 需要创建这么多字符串？ → 参数按值传递
5. **结论**：将参数改为引用或 `string_view`，避免拷贝

### 4. 修复方案 (写入 debugged_server.cpp 的 `simulate_cpu_hog`)

```cpp
std::string_view fast_find_header(std::string_view request, std::string_view header_name) {
    size_t pos = request.find(header_name);
    if (pos == std::string_view::npos) return {};
    size_t end = request.find("\r\n", pos);
    return request.substr(pos, end - pos);   // string_view::substr 是 O(1), 不分配内存
}

void simulate_cpu_hog() {
    std::cout << "\n=== [FIXED] 实验 5: CPU Hotspot ===\n";
    std::string http_request =
        "GET /api/v1/data HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: Mozilla/5.0\r\n"
        "Accept: text/html,application/json\r\n"
        "Accept-Encoding: gzip, deflate, br\r\n"
        "Connection: keep-alive\r\n"
        "X-Request-ID: abcdef-123456-ghijkl-789012\r\n"
        "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJ1c2VyIjoiYWRtaW4ifQ\r\n"
        "\r\n";

    auto start = std::chrono::steady_clock::now();
    int found = 0;

    for (int i = 0; i < 2000000; i++) {
        if (!fast_find_header(http_request, "Host").empty())
            found++;
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "[CPU] " << found << " matches in " << ms << " ms\n";
}
```

**核心原理**：`std::string_view` 是一个不拥有数据的轻量级视图，仅包含指针和长度。传参零拷贝，`substr()` 也是 O(1)。在 HTTP 解析这类只读场景中，用 `string_view` 替代 `string` 可以获得数量级的性能提升。

### 5. 验证修复

```bash
# 分别运行 buggy 和 debugged 版本, 对比执行时间
./build-debug/buggy_server cpu
./build-debug/debugged_server cpu
# 预期: debugged 版本快 5~20 倍
```

---

## 实验 6: strace — 系统调用追踪

**使用工具**: `strace` (Linux) / `dtruss` (macOS)
**运行模式**: `./buggy_server network`

### 1. Bug 代码分析

```cpp
// buggy_server.cpp — simulate_network() (echo server 片段)
while (true) {
    int cfd = accept(server_fd, (struct sockaddr*)&cli, &cli_len);
    // ... read + write (echo) ...

    // BUG: 从不 close(cfd)! 每次连接后 fd 泄漏
    std::cout << "[Server] fd=" << cfd << " NOT closed (fd leak!)\n";
}
```

**发生的异常**：每次 `accept` 返回的客户端 fd 在处理完后从未被 `close()`。随着客户端不断连接，进程的 fd 数量持续增长，最终达到 `ulimit -n` 上限（通常 1024），之后 `accept` 和 `socket` 都会返回 `EMFILE` 错误。

strace 的作用：追踪进程的所有系统调用，让你看到程序在内核层面做了什么。

### 2. 实验步骤

```bash
# 终端 1: 启动服务器
./build-debug/buggy_server network

# 终端 2: 用 strace 附加到服务器进程 (Linux)
strace -p $(pgrep buggy_server) -e trace=network,read,write -f

# 终端 3: 用 nc 发送数据触发 accept
echo "Hello strace" | nc localhost 9527
echo "Hello again"  | nc localhost 9527
echo "Third time"   | nc localhost 9527
```

**macOS 替代** (需要关闭 SIP 或使用 sudo):

```bash
# macOS 使用 dtruss (dtrace 封装)
sudo dtruss -p $(pgrep buggy_server) -f
```

### 3. 输出解读与诊断思路

**strace 典型输出 (每次客户端连接)**:

```
accept4(3, {sa_family=AF_INET, sin_port=htons(54321), sin_addr=inet_addr("127.0.0.1")}, ...) = 4
read(4, "Hello strace\n", 1023) = 13
write(4, "Hello strace\n", 13) = 13
                                          ← 注意: 没有 close(4) !

accept4(3, {sa_family=AF_INET, sin_port=htons(54322), ...}, ...) = 5
read(5, "Hello again\n", 1023) = 12
write(5, "Hello again\n", 12) = 12
                                          ← 没有 close(5) !

accept4(3, ...) = 6
read(6, "Third time\n", 1023) = 11
write(6, "Third time\n", 11) = 11
                                          ← 没有 close(6) !
```

**诊断思路（假设你不知道 Bug 在哪）**：
1. 观察每次 `accept` 返回的 fd 数字持续递增：4 → 5 → 6 → ...
2. 正常的服务器应该呈现 `accept → read → write → close` 的循环模式
3. 但这里只有 `accept → read → write`，**缺少 `close`**
4. fd 号只增不减 → FD 泄漏
5. 验证：`lsof -p <PID> | wc -l` 会随连接数线性增长
6. 根因：代码中 `accept` 后的处理流程缺少 `close(cfd)`

**strace 其他常见分析场景**：
- `connect` 返回 `ECONNREFUSED` → 目标服务未启动
- 大量 `epoll_wait` 返回 0 → 空转，可能超时设置不合理
- `write` 返回 `EPIPE` → 对方已关闭连接
- `open` 返回 `EMFILE` → fd 耗尽

### 4. 修复方案

FD 泄漏的修复在实验 7 中与网络观测一并给出（同一个 `simulate_network` 函数）。

---

## 实验 7: tcpdump + lsof + ss — 网络层观测

**使用工具**: `tcpdump`, `lsof`, `ss`/`netstat`
**运行模式**: `./buggy_server network` (与实验 6 共用)

### 1. 观测目标

本实验不引入新的 Bug，而是学习使用网络层观测工具来理解服务器行为。包括：
- **lsof**: 查看进程打开的文件/socket，检测 FD 泄漏
- **ss / netstat**: 查看 socket 连接状态（LISTEN, ESTABLISHED, CLOSE_WAIT...）
- **tcpdump**: 抓取网络包，分析 TCP 握手/数据传输/连接关闭

### 2. 实验步骤

#### Phase 1: lsof — 查看端口监听与 FD 泄漏

```bash
# 终端 1: 启动服务器
./build-debug/buggy_server network

# 终端 2: 查看谁在监听 9527 端口
lsof -i :9527
# 输出:
# COMMAND      PID USER   FD  TYPE DEVICE SIZE/OFF NODE NAME
# buggy_ser  12345 user   3u  IPv4 ...    0t0      TCP *:9527 (LISTEN)

# 发送几个连接
for i in $(seq 1 5); do echo "msg $i" | nc localhost 9527; done

# 再次检查 fd 数量 (观察泄漏)
lsof -p $(pgrep buggy_server) | grep -c "IPv4"
# 结果: 6 (1 个 listen fd + 5 个泄漏的 client fd)

# 再发 5 个连接
for i in $(seq 1 5); do echo "msg $i" | nc localhost 9527; done

lsof -p $(pgrep buggy_server) | grep -c "IPv4"
# 结果: 11 (1 + 10) → 持续增长, 确认 FD 泄漏
```

#### Phase 2: ss / netstat — 查看连接状态

```bash
# Linux: 使用 ss
ss -tlnp | grep 9527
# 输出: LISTEN  0  128  0.0.0.0:9527  0.0.0.0:*  users:(("buggy_server",pid=12345,fd=3))

# ss -tnp | grep 9527     # 查看所有 TCP 连接 (含 ESTABLISHED, CLOSE_WAIT 等)

# macOS: 使用 netstat
netstat -an | grep 9527
# 输出: tcp4  0  0  *.9527  *.*  LISTEN
```

#### Phase 3: tcpdump — 抓包分析 TCP 交互

```bash
# 终端 2: 启动抓包 (在发送数据之前)
# Linux:
sudo tcpdump -i lo port 9527 -nn -X

# macOS:
sudo tcpdump -i lo0 port 9527 -nn -X

# 终端 3: 发送数据
echo "Hello tcpdump" | nc localhost 9527
```

### 3. 输出解读与诊断思路

**tcpdump 典型输出**:

```
# TCP 三次握手
12:00:00.001 IP 127.0.0.1.54321 > 127.0.0.1.9527: Flags [S], seq 1000    ← SYN
12:00:00.001 IP 127.0.0.1.9527 > 127.0.0.1.54321: Flags [S.], seq 2000   ← SYN-ACK
12:00:00.001 IP 127.0.0.1.54321 > 127.0.0.1.9527: Flags [.], ack 2001    ← ACK

# 数据传输
12:00:00.002 IP 127.0.0.1.54321 > 127.0.0.1.9527: Flags [P.], length 15  ← 客户端发送 "Hello tcpdump\n"
12:00:00.002 IP 127.0.0.1.9527 > 127.0.0.1.54321: Flags [.], ack ...     ← 服务端 ACK
12:00:00.003 IP 127.0.0.1.9527 > 127.0.0.1.54321: Flags [P.], length 15  ← 服务端 echo 回复

# 连接关闭 (客户端发起, 因为 nc 结束了)
12:00:00.003 IP 127.0.0.1.54321 > 127.0.0.1.9527: Flags [F.], ...        ← 客户端 FIN
12:00:00.003 IP 127.0.0.1.9527 > 127.0.0.1.54321: Flags [.], ack ...     ← 服务端 ACK
# 注意: 服务端没有发 FIN! 因为服务端从未 close(cfd)
```

**诊断思路**：
1. **三次握手正常** → 连接建立没问题
2. **数据收发正常** → echo 功能正确
3. **只有客户端发了 FIN，服务端没有发 FIN** → 服务端没有 `close()` 客户端 socket
4. 客户端连接进入 `FIN_WAIT_2` 状态（等待服务端 FIN），服务端 socket 进入 `CLOSE_WAIT` 状态
5. 用 `ss -tnp | grep 9527` 确认：大量 `CLOSE_WAIT` 状态连接 → 经典 FD 泄漏特征

**tcpdump Flags 速查**:
- `[S]` = SYN (发起连接)
- `[S.]` = SYN-ACK
- `[.]` = ACK
- `[P.]` = PSH-ACK (推送数据)
- `[F.]` = FIN-ACK (关闭连接)
- `[R.]` = RST-ACK (重置连接)

### 4. 修复方案 (写入 debugged_server.cpp 的 `simulate_network`)

```cpp
void simulate_network() {
    std::cout << "\n=== [FIXED] 实验 6 & 7: Network Echo Server ===\n";

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9527);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return;
    }
    if (listen(server_fd, 128) < 0) {
        perror("listen"); close(server_fd); return;
    }

    std::cout << "[Server] Listening on 0.0.0.0:9527 (PID: " << getpid() << ")\n";
    std::cout << "Press Ctrl+C to stop.\n\n";

    while (true) {
        struct sockaddr_in cli{};
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(server_fd, (struct sockaddr*)&cli, &cli_len);
        if (cfd < 0) { perror("accept"); continue; }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        std::cout << "[Server] Client " << ip << ":" << ntohs(cli.sin_port)
                  << " (fd=" << cfd << ")\n";

        char buf[1024];
        ssize_t n = read(cfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::cout << "[Server] Received: " << buf;
            write(cfd, buf, n);
        }

        close(cfd);   // FIX: 正确关闭客户端 fd
        std::cout << "[Server] fd=" << cfd << " closed.\n\n";
    }
}
```

**核心原理**：每个 `accept` 返回的 fd 在使用完毕后必须 `close`。在真实服务器中，通常配合 RAII wrapper（如自定义的 `Socket` 类，析构时自动 close）来确保不遗漏。

### 5. 验证修复

```bash
# 终端 1: 运行修复版服务器
./build-debug/debugged_server network

# 终端 2: 发送多个连接后检查 FD 数
for i in $(seq 1 10); do echo "msg $i" | nc localhost 9527; done
lsof -p $(pgrep debugged_server) | grep -c "IPv4"
# 预期: 始终为 1 (只有 listen fd)

# tcpdump 中应该能看到服务端也发送了 FIN
```

---

## 附录 A: Valgrind 补充

Valgrind 是 ASan 出现之前最常用的内存错误检测工具。虽然 ASan 更快（约 2x 减速 vs Valgrind 的 20-50x），但 Valgrind 不需要重新编译，且能检测未初始化内存读取。

```bash
# 无需特殊编译, 只需 -g
cmake -B build-debug && cmake --build build-debug

# 检测内存泄漏
valgrind --leak-check=full --show-leak-kinds=all ./build-debug/buggy_server leak
# (按 Enter 退出程序后查看报告)

# 检测 UAF
valgrind ./build-debug/buggy_server uaf
```

Valgrind 输出格式与 ASan 类似，但更详细。适合以下场景：
- 无法重新编译的第三方库
- 需要检测未初始化内存读取 (ASan 无法检测，需 MSan，而 MSan 仅 Clang/Linux 支持)
- 生产环境二进制的离线分析

> **注意**：Valgrind 不支持 Apple Silicon (ARM) Mac。如在 macOS 上，可使用 `leaks --atExit -- ./program` 作为替代。

---

## 附录 B: eBPF / bpftrace 简介

eBPF 是 Linux 内核的可编程追踪框架，可以在**不修改代码、不重启进程**的情况下动态注入观测逻辑。在高性能网络编程领域被广泛使用。

### 典型应用场景

```bash
# 追踪所有 accept 系统调用 (谁在接受连接?)
sudo bpftrace -e 'tracepoint:syscalls:sys_enter_accept4 { printf("pid=%d comm=%s\n", pid, comm); }'

# 统计每秒的 TCP 连接数
sudo bpftrace -e 'tracepoint:syscalls:sys_exit_accept4 /retval >= 0/ { @connections = count(); } interval:s:1 { print(@connections); clear(@connections); }'

# 追踪 TCP 重传
sudo bpftrace -e 'tracepoint:tcp:tcp_retransmit_skb { printf("retransmit to %s\n", ntop(args->daddr)); }'

# 对指定进程的 malloc 调用计数 (定位内存分配热点)
sudo bpftrace -e 'uprobe:/lib/x86_64-linux-gnu/libc.so.6:malloc /pid == 12345/ { @sizes = hist(arg0); }'
```

### macOS 替代: DTrace

macOS 使用 DTrace 实现类似功能 (需关闭 SIP):

```bash
# 追踪进程的系统调用
sudo dtrace -n 'syscall:::entry /pid == 12345/ { @[probefunc] = count(); }'
```

> eBPF/DTrace 是高级主题，面试中展示对其原理和应用场景的了解即可。具体深入可参考 Brendan Gregg 的 *"BPF Performance Tools"* 一书。

---

## 附录 C: macOS 下完成实验的替代指导

### Sanitizers (实验 1-3)

**完全兼容**。Apple Clang 支持 ASan、TSan、UBSan，使用同样的 CMake 选项即可。

唯一区别：**Apple Clang 不支持 LeakSanitizer** (`ASAN_OPTIONS=detect_leaks=1` 无效)。

替代方案：
```bash
# 使用 macOS 内置的 leaks 工具
leaks --atExit -- ./build-asan/buggy_server leak
# 按 Enter 退出后, leaks 工具报告泄漏
```

或安装 Homebrew LLVM (完整版 Clang, 支持 LeakSanitizer):
```bash
brew install llvm
export CC=/opt/homebrew/opt/llvm/bin/clang
export CXX=/opt/homebrew/opt/llvm/bin/clang++
cmake -B build-asan -DUSE_ASAN=ON && cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1 ./build-asan/buggy_server leak
```

### GDB → LLDB (实验 4)

macOS 默认使用 LLDB。命令对照表：

| GDB | LLDB | 功能 |
|-----|------|------|
| `run <args>` | `run <args>` | 启动程序 |
| `bt` | `bt` | 调用栈回溯 |
| `frame N` | `frame select N` | 切换栈帧 |
| `print var` | `p var` | 打印变量 |
| `info locals` | `frame variable` | 查看局部变量 |
| `info threads` | `thread list` | 查看线程 |
| `thread apply all bt` | `thread backtrace all` | 所有线程调用栈 |
| `attach PID` | `process attach --pid PID` | 附加到进程 |
| `break func` | `b func` | 设置断点 |

Core Dump 在 macOS 上的替代：
```bash
# macOS 崩溃报告自动生成在:
ls ~/Library/Logs/DiagnosticReports/
# 文件名类似: buggy_server_2026-04-04-xxx.ips
# 内容包含调用栈, 但不如 GDB 交互式分析方便
# 推荐直接使用 LLDB 交互式调试
```

### perf → Instruments / sample (实验 5)

macOS 没有 `perf`。替代方案：

```bash
# 方法 1: sample (快速命令行方式)
./build-debug/buggy_server cpu &
PID=$!
sample $PID 3 -file cpu_profile.txt
kill $PID 2>/dev/null
# 查看 cpu_profile.txt 中的调用树

# 方法 2: Instruments (GUI, 可生成火焰图)
# 打开 Instruments.app → Time Profiler → 选择可执行文件 → 录制
# Call Tree 视图即为 perf report 的图形化等价物
```

如果想在 macOS 上生成类似 Linux 的火焰图:
```bash
# 使用 dtrace 采样 (需关闭 SIP)
sudo dtrace -x ustackframes=100 -n \
  'profile-997 /pid == '$PID'/ { @[ustack()] = count(); }' \
  -o dtrace_out.txt
~/FlameGraph/stackcollapse.pl dtrace_out.txt | ~/FlameGraph/flamegraph.pl > flamegraph.svg
```

### strace → dtruss (实验 6)

macOS 没有 `strace`。替代方案：

```bash
# dtruss (dtrace 封装, 需 sudo, 可能需关闭 SIP)
sudo dtruss -p $(pgrep buggy_server) -f

# 或只追踪网络相关调用
sudo dtruss -p $(pgrep buggy_server) -f -t accept,read,write,close,socket
```

> **SIP (System Integrity Protection) 说明**：macOS 的 dtruss/dtrace 需要关闭 SIP 才能追踪非系统进程。
> 关闭方法：重启 → 开机时按住 Cmd+R 进入恢复模式 → 终端执行 `csrutil disable` → 重启。
> **安全提示**：实验完成后建议重新开启 SIP (`csrutil enable`)。

### tcpdump (实验 7)

macOS 自带 tcpdump，但 loopback 接口名不同：

```bash
# macOS: lo0 (不是 lo)
sudo tcpdump -i lo0 port 9527 -nn -X
```

### lsof (实验 1.2, 7)

macOS 的 `lsof` 命令与 Linux **完全相同**。无需替代。

### ss → netstat (实验 7)

macOS 没有 `ss`。使用 `netstat`:

```bash
netstat -an | grep 9527
# 或
netstat -anp tcp | grep 9527
```
---

## 附录 D: 面试高频问题速查

### Q: 线上服务 CPU 100%，怎么排查？
1. `top -Hp <PID>` 找到 CPU 最高的线程 TID
2. `perf top -p <PID>` 或 `perf record -p <PID> -g` 找到热点函数
3. 生成火焰图，定位调用链
4. 检查是否死循环、低效算法、锁争用 (`perf lock`)

### Q: 服务突然无法接受新连接？
1. `lsof -p <PID> | wc -l` 检查 fd 数量是否逼近 `ulimit -n`
2. `ss -s` 查看 socket 统计
3. `ss -tnp | grep CLOSE_WAIT` 检查未关闭的连接
4. `strace -e trace=accept4 -p <PID>` 看 accept 是否返回 EMFILE

### Q: 客户端说请求超时，怎么证明是客户端还是服务端的问题？
1. 在服务端 `tcpdump -i eth0 port <PORT> -w capture.pcap`
2. 用 Wireshark 打开 pcap，分析：
   - 是否收到客户端的 SYN？ (没收到 → 网络问题)
   - 是否回了 SYN-ACK？ (没回 → 服务端 backlog 满)
   - 是否收到请求数据？ (没收到 → 客户端未发送)
   - 是否回了响应？ (没回 → 服务端处理超时)

### Q: 程序偶尔崩溃，Core Dump 怎么分析？
1. 确保 `ulimit -c unlimited` 且 core pattern 正确配置
2. `gdb ./program core.<PID>`
3. `bt` 查看调用栈
4. 如果是多线程：`thread apply all bt` 查看所有线程
5. 如果怀疑内存问题：上 ASan 重新编译复现

### Q: 什么时候用 Sanitizer，什么时候用 Valgrind？
- **Sanitizer**：开发/CI 阶段常开。速度快（2x 减速），需重新编译。ASan 和 TSan 互斥。
- **Valgrind**：不需要重新编译，但速度慢（20-50x 减速）。适合分析第三方二进制。
- **建议**：CI 中同时配置 ASan 构建和 TSan 构建两条流水线。

### Q: shared_ptr vs unique_ptr 怎么选？
- `unique_ptr`：独占所有权，零开销，优先使用
- `shared_ptr`：共享所有权（如连接对象被多个线程引用），有引用计数开销
- 网络服务器典型模式：连接由 `shared_ptr` 管理，注册到 EventLoop 和用户回调中

---

## 实验清单与进度追踪

完成每个实验后打勾：

- [ ] 实验 1.1: ASan — Use-After-Free
- [ ] 实验 1.2: ASan + lsof — 内存泄漏 + FD 泄漏
- [ ] 实验 2.1: TSan — 数据竞争
- [ ] 实验 2.2: TSan + GDB — 死锁
- [ ] 实验 3: UBSan — 未定义行为
- [ ] 实验 4: GDB/LLDB — 崩溃分析
- [ ] 实验 5: perf + FlameGraph — CPU 性能热点
- [ ] 实验 6: strace — 系统调用追踪
- [ ] 实验 7: tcpdump + lsof + ss — 网络层观测

