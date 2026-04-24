# Airi-Cpp-Server-Lib 面试演示手册

> 面向面试场景的"按图索骥"操作清单。手册末尾每节都给出可直接复制粘贴的命令。
> 适用版本：day30 收官快照及之后任意提交。
> 仓库根目录假定为 `~/Desktop/MyCppServerLib`，文中以 `$REPO` 代称。

---

## 0. 30 秒电梯介绍

> "MyCppServerLib 是参考 muduo 设计、从零实现的主从 Reactor 网络库，
> 跨平台 epoll / kqueue 多路复用，提供完整 HTTP/1.x 应用层（Keep-Alive、
> ETag/304、Range/206、sendfile 零拷贝），可组合的中间件管道（per-IP 令牌桶
> 限流、Bearer/API Key 鉴权、CORS、gzip），TLS 通过 OpenSSL 可选接入。
> 44 个 GoogleTest 单元测试 + GitHub Actions CI（ASan/TSan/UBSan/gcov/clang-tidy）。
> 8 核 macOS 上峰值 18.8 万 QPS，P99 2.15 ms。"

如果面试官追问，按下面分章节展开。

---

## 1. 仓库结构速览

```
$REPO/
├── src/
│   ├── include/                    # 公开头文件
│   │   ├── net/                    # Reactor 核心：EventLoop / TcpServer / Channel / Connection
│   │   ├── net/Poller/             # 跨平台 IO 多路复用：EpollPoller / KqueuePoller
│   │   ├── http/                   # HTTP 应用层
│   │   │   ├── HttpServer.h        # 路由表 + 中间件链
│   │   │   ├── HttpContext.h       # 解析状态机 + 请求大小限流
│   │   │   ├── StaticFileHandler.h # ETag / Range / 304 / 路径遍历防护
│   │   │   ├── CorsMiddleware.h    # CORS 预检 + 通用头注入
│   │   │   ├── GzipMiddleware.h    # zlib 响应体压缩
│   │   │   ├── RateLimiter.h       # per-IP 令牌桶 → 429
│   │   │   ├── AuthMiddleware.h    # Bearer Token / API Key 双模式 → 403
│   │   │   └── ServerMetrics.h     # 计数器（QPS / 限流 / 鉴权失败）
│   │   ├── log/                    # 异步双缓冲日志
│   │   └── timer/                  # TimerQueue + SteadyClock
│   ├── common/                     # 上述 .h 对应的实现 .cpp
│   └── test/                       # 44 个 GoogleTest 单元测试
├── examples/
│   └── src/http_server.cpp         # 全特性演示服务器
├── benchmark/
│   └── conn_scale_test.cpp         # 连接规模 / 内存占用基线
├── cmake/                          # FetchContent 配置 + 安装规则模板
├── .github/workflows/ci.yml        # 7 项并行 CI Job
├── dev-log/                        # day1 ~ day30 教学型实现日志
├── HISTORY/                        # 每日只读快照（day1 ~ day30）
└── INTERVIEW_GUIDE.md              # ← 当前文件
```

**口播一句话**：核心代码在 `src/`，演示在 `examples/`，测试与 CI 全自动。

---

## 2. 三分钟构建 & 运行

```bash
cd $REPO
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)

# 跑一次全部单元测试（应全部 PASS）
cd build && ctest --output-on-failure -j8

# 启动演示服务器（监听 8888）
./examples/http_server &
SERVER_PID=$!

# 浏览器访问 http://localhost:8888 看到首页
# 或用 curl 验证
curl -i http://localhost:8888/
curl -i http://localhost:8888/api/users          # 鉴权失败 → 403
curl -i -H "Authorization: Bearer demo-token-2024" http://localhost:8888/api/users  # → 200

# 关闭演示服务器
kill $SERVER_PID
```

**面试官常问**：`-DCMAKE_BUILD_TYPE=Release` 与 `Debug` 的区别？
> Release 默认 `-O3 -DNDEBUG`，关闭 assert，开启高级优化（向量化、循环展开）；
> Debug 是 `-O0 -g`，调试信息完整。Sanitizer 必须用 Debug 配置。

---

## 3. 单元测试演示（GoogleTest）

### 3.1 列出所有测试

```bash
cd $REPO/build
ctest -N        # -N = no-execute, 仅列出
```

期望输出（部分）：

```
  Test #1: BackpressureDecisionTest
  Test #2: HttpContextTest
  Test #3: HttpRequestLimitsTest
  Test #4: SocketPolicyTest
  Test #5: TcpServerPolicyTest
  ...
Total Tests: 14
```

每个二进制内部包含若干 `TEST()` 宏：

```bash
./HttpContextTest --gtest_list_tests
# Output:
#   HttpContextTest.
#     ParseSimpleGet
#     ParseChunkedBody
#     PayloadTooLargeReturns413
#     ...
```

### 3.2 跑单个测试 / 单个用例

```bash
./HttpContextTest                              # 跑所有用例
./HttpContextTest --gtest_filter="*ChunkedBody" # 仅跑名字含 ChunkedBody 的
ctest -R HttpContextTest --output-on-failure   # 用 ctest 跑某一个
```

### 3.3 解读失败输出

GoogleTest 失败示例：

```
[ RUN      ] HttpContextTest.ParseChunkedBody
test/HttpContextTest.cpp:47: Failure
Expected equality of these values:
  ctx.body()
    Which is: "Hello"
  std::string("Hello, World!")
[  FAILED  ] HttpContextTest.ParseChunkedBody (3 ms)
```

**口播**：`EXPECT_EQ` 失败时打印两个值，`ASSERT_EQ` 失败会立即停止当前用例。

### 3.4 关键测试用例导览

| 测试文件 | 验证什么 | 何时引用 |
|---------|---------|---------|
| `HttpContextTest` | 状态机解析正确性、limits 边界 | 面试官问 HTTP 解析 |
| `HttpRequestLimitsTest` | 行/头/体三层限流均能触发 413 | 问"如何防 OOM 攻击" |
| `BackpressureDecisionTest` | 高水位回调触发条件 | 问回压机制 |
| `TcpServerPolicyTest` | 连接上限 + 线程池均匀分发 | 问连接管理 |
| `StaticFileHandlerTest` | ETag / If-None-Match → 304；Range → 206；路径遍历 → 400 | 问 HTTP 缓存协商 |
| `CorsMiddlewareTest` | OPTIONS 预检返回 204；通用头注入 | 问跨域 |
| `MetricsTest` | 请求计数原子递增正确 | 问性能监控 |

---

## 4. Sanitizer 三件套手动操作

### 4.1 AddressSanitizer（堆/栈越界、UAF、泄漏）

```bash
cd $REPO
rm -rf build-asan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_ASAN=ON
cmake --build build-asan -j8
cd build-asan && ctest --output-on-failure -j4
```

**面试官追问**：ASan 怎么找堆越界？
> ASan 在分配的内存周围插入"红区"（红色保护带），并把 free 后的内存放进延迟释放的"隔离区"（quarantine）。读写红区或隔离区的内存会立即报告。运行期开销约 2× 时间、3× 内存。

**示范一次主动触发**（如果时间允许）：

```cpp
// 临时改 src/test/LogTest.cpp，插入：
char buf[10];
buf[10] = 'x';   // 越界写
```

跑测试，ASan 会输出：

```
==12345==ERROR: AddressSanitizer: stack-buffer-overflow on address ...
WRITE of size 1 at 0x... thread T0
    #0 0x... in TestBody src/test/LogTest.cpp:NN
```

### 4.2 ThreadSanitizer（数据竞争）

```bash
cd $REPO && rm -rf build-tsan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_TSAN=ON
cmake --build build-tsan -j8
cd build-tsan && ctest --output-on-failure -j2   # TSan 不要并发太高
```

**面试官追问**：TSan 与 ASan 能否同时开？
> 不能。两者都使用 shadow memory 但布局冲突，必须分开构建。

**关键点**：`StressTest` / `TimerTest` 是触发竞争的高概率用例，TSan 跑过就证明加锁正确。

### 4.3 UndefinedBehaviorSanitizer（整数溢出、空指针解引用、左移负数）

```bash
cd $REPO && rm -rf build-ubsan
cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_UBSAN=ON
cmake --build build-ubsan -j8
cd build-ubsan && ctest --output-on-failure -j8
```

**口播**：UBSan 比 ASan 轻量，运行期开销可忽略，建议长期开发分支默认开启。

---

## 5. 代码覆盖率（gcov + lcov）

仅 Linux / GCC（macOS 上 lcov 与 gcov-13 兼容性差）。在 GitHub Actions 上自动跑。本地手动：

```bash
# 需要 gcc-11 + lcov 1.16
brew install lcov   # macOS
# 或 apt install gcc-11 lcov   # Ubuntu

cd $REPO && rm -rf build-cov
CC=gcc-11 CXX=g++-11 cmake -S . -B build-cov \
    -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_COVERAGE=ON
cmake --build build-cov -j8
cd build-cov && ctest -j8 || true   # 容忍个别失败，仍能产出报告

lcov --capture --directory . --output-file coverage.info \
     --gcov-tool gcov-11 --rc lcov_branch_coverage=0 \
     --ignore-errors mismatch,unused,empty
lcov --remove coverage.info '/usr/*' '*/_deps/*' '*/test/*' \
     --output-file coverage.filtered.info
genhtml coverage.filtered.info --output-directory cov-report
open cov-report/index.html       # macOS；Linux 用 xdg-open
```

**口播**：
- `--coverage` 编译选项让 gcc 在每个分支插入计数器，运行后产出 `.gcda` 文件。
- `lcov --capture` 把 `.gcda` 聚合成纯文本 `.info`，`genhtml` 渲染成 HTML。
- 项目当前覆盖率约 **78%**（核心 net/ + http/ 模块 > 90%）。

---

## 6. 静态分析（clang-tidy）

```bash
cd $REPO && cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j8                 # 先编译生成头文件 / PCH
cmake --build build --target clang-tidy # 触发自定义 target
# 或直接：
clang-tidy --quiet -p build src/common/net/Connection.cpp
```

**面试官追问**：`.clang-tidy` 配置了哪些 check？
> 主要是 `cppcoreguidelines-*`、`bugprone-*`、`performance-*`、`modernize-use-nullptr / use-override / use-equals-default`。
> 项目级别禁用了 `cppcoreguidelines-pro-bounds-pointer-arithmetic`（与 socket buffer 操作不兼容）。

**口播**：clang-tidy 是 LLVM 的"编译期 lint"，比 cppcheck 慢但能跨翻译单元，依赖 `compile_commands.json`。

---

## 7. 性能基准（在面试机上现场跑）

### 7.1 内置 BenchmarkTest（4 线程 5 秒）

```bash
cd $REPO/build
./examples/http_server &
sleep 1
./BenchmarkTest 127.0.0.1 8888 / 4 5
kill %1
```

期望输出：

```
[BenchmarkTest] threads=4 duration=5s target=http://127.0.0.1:8888/
[BenchmarkTest] total requests: 940000
[BenchmarkTest] QPS: 188000.0
[BenchmarkTest] avg latency: 21.3 us
[BenchmarkTest] p99 latency: 2.15 ms
```

### 7.2 wrk 复现简历数据（本机推荐）

```bash
brew install wrk
./examples/http_server &
sleep 1
wrk -t4 -c100 -d10s --latency http://127.0.0.1:8888/
```

简历数字（8 核 macOS, M-series）：
- 峰值 QPS：**18.8 万 req/s**
- P99 延迟：**2.15 ms**
- 单连接内存：**3.72 KB**（10000 并发，RSS 线性增长无泄漏）
- 峰值 CPU：**22%**（仅 2 个 IO 线程）

### 7.3 连接规模基线

```bash
./build/conn_scale_test 10000 127.0.0.1 8888
```

逐步爬升 1000 → 5000 → 10000 并发，对比 RSS。

---

## 8. CI 工作流速查

`.github/workflows/ci.yml` 共 **7 个并行 Job**：

| Job | 作用 | 失败常见原因 |
|------|------|-------------|
| `build-and-test (ubuntu-latest)` | Linux Release 构建 + ctest | 平台特定头文件遗漏 |
| `build-and-test (macos-latest)` | macOS Release 构建 + ctest | kqueue 接口差异 |
| `sanitizer-asan` | Debug + ASan 全测试 | 内存泄漏 / UAF |
| `sanitizer-tsan` | Debug + TSan 全测试 | 数据竞争 / 死锁 |
| `sanitizer-ubsan` | Debug + UBSan 全测试 | 整数溢出 / 空指针 |
| `coverage` | gcov + lcov 上传 artifact | lcov 版本不兼容 |
| `static-analysis` | clang-tidy 全文件 | 任何新引入的 warning |
| `benchmark-regression` | QPS < 10000 阈值告警 | demo_server 启动失败 |

**口播**：每次 push 到 main / PR 都触发，平均 8 分钟跑完。

---

## 9. 高频追问 & 标准回答

### Q1：为什么自己写 Reactor 而不直接用 boost::asio？
> 学习目标驱动。从零实现能让我准确理解：non-blocking + epoll + 状态机这条主线是怎么衔接的；
> asio 的 executor / strand / awaitable 抽象层次太高，看不到底层。生产环境我会用 asio 或 muduo。

### Q2：epoll 的 LT 和 ET 有什么区别？项目用哪个？
> LT (Level Triggered) 默认模式：只要 fd 可读 / 可写就持续通知，编程容易，不易丢事件。
> ET (Edge Triggered) 仅状态变化时通知一次，必须循环读到 EAGAIN 才能确保读完，吞吐更高但难写。
> 项目 **EpollPoller 用 LT**（兼容性好），**Connection::handleRead 用 while-loop + EAGAIN 退出**模拟 ET 语义，
> 两者结合：行为正确 + 吞吐接近 ET。Day 22 的代码里能看到这个写法。

### Q3：Connection 的生命周期怎么管？
> `std::shared_ptr<Connection>` + `enable_shared_from_this`。
> Channel 注册的回调通过 `weak_ptr` 弱持有 Connection，每次回调里 `lock()` 提升为 shared_ptr。
> 这避免了"Connection 已析构但 Channel 还在 epoll 中"的悬挂问题。
> 关闭时通过 `EventLoop::queueInLoop` 把销毁延后到下一轮事件循环，确保正在执行的回调安全完成。

### Q4：HTTP 解析为什么用状态机？
> HTTP/1.x 报文边界由分隔符（`\r\n`）和长度字段决定，**TCP 是流式的，会发生分包/粘包**。
> 状态机模型保证：每收到一个字节就推进一次状态，不要求一次拿到完整报文。
> 状态：`kExpectRequestLine` → `kExpectHeaders` → `kExpectBody` → `kGotAll` / `kInvalid`。
> 同时在每个状态入口累加字节计数 → 三层限流（行 / 头 / 体）一站式实现。

### Q5：令牌桶 vs 漏桶 vs 滑动窗口？
> **令牌桶**：允许突发（桶可瞬时满），长期速率不超 refillRate。本项目使用。
> **漏桶**：请求强制以恒定速率流出，平滑但不允许突发。
> **滑动窗口**：精确，但需要 O(N) 时间或环形缓冲。
> API 网关场景令牌桶最常见——既保护后端又允许合理突发。

### Q6：Bearer Token vs API Key？为什么三档查找？
> Bearer 通常承载 JWT，含到期时间和签名，**有状态**（短期）。
> API Key 是固定字符串，**无状态**（长期），适合机器间调用。
> 优先级：Authorization Header（最规范）> X-API-Key Header（不进 URL log）> ?api_key= 查询参数（兼容旧客户端）。
> 失败返回 **403 Forbidden** 而非 401 —— 401 必须带 `WWW-Authenticate` 头，会触发浏览器原生登录框。

### Q7：sendfile 零拷贝省了几次拷贝？
> 传统 read+write：4 次上下文切换 + 2 次内核↔用户态拷贝。
> sendfile：2 次上下文切换 + 1 次拷贝（从 page cache 直接到 socket buffer）。
> 100 MB 文件下载，CPU 利用率从 100% 降到约 30%。

### Q8：你们的 P99 2.15ms 是怎么测出来的？
> wrk + `--latency` 选项。10 秒持续压测，统计延迟直方图，取第 99 百分位。
> P99 反映"99% 的请求都比这个快"，比平均延迟更能反映长尾。
> 我们的 P99 主要来源：(1) 偶发的 macOS 系统中断；(2) 内核 socket buffer 满触发的 EAGAIN 回退。

---

## 10. 万一现场出错的兜底应对

| 现象 | 可能原因 | 应对 |
|------|---------|------|
| `cmake` 找不到 OpenSSL | macOS 默认无 OpenSSL | `brew install openssl@3 && cmake -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) ..` |
| `ctest` 全挂 | 没先 `cmake --build` | 先 build 再 test |
| ASan / TSan 编译报 `__asan_init` 未定义 | 编译器与运行时不匹配 | 切到 GCC 而非 Apple clang，或装 LLVM |
| `port already in use` | 上次的 server 没杀 | `lsof -i :8888` + `kill <pid>` |
| clang-tidy 找不到 `compile_commands.json` | 没开 `CMAKE_EXPORT_COMPILE_COMMANDS` | 配置时加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` |

---

## 11. 一句话总结

> "这个项目的设计目标是让面试官看到：我能从 epoll 系统调用一直串到 HTTP 中间件链，
> 中间任何一层（线程模型、状态机、回压、限流、鉴权、零拷贝）都能展开讲；
> 同时它的工程化（CI、Sanitizer、覆盖率、静态分析）证明我不是只写'能跑'的代码。"

祝面试顺利。
