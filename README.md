# Airi-Cpp-Server-Lib

> **一个跨平台、学习用的 C++17 高性能 HTTP / TCP 服务库**

[![CI](https://img.shields.io/badge/CI-7%20jobs-success)](.github/workflows/ci.yml)
[![tests](https://img.shields.io/badge/GoogleTest-34%20cases-success)](src/test/)
[![sanitizers](https://img.shields.io/badge/ASan%20%7C%20TSan%20%7C%20UBSan-clean-success)](.github/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](CMakeLists.txt)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

---

## 这是什么

一个从 `socket()` 开始，30 天用 C++17 手写的**多 Reactor 网络库 + HTTP/1.x 服务器框架**，覆盖了一个工业级网络中间件应该有的几乎全部能力：

- **网络层**：epoll(Linux) / kqueue(macOS) 双后端、`one-loop-per-thread` 多 Reactor、`Buffer` / `Channel` / `Connection` / `EventLoop` / `TcpServer` 完整抽象。
- **HTTP 层**：HTTP/1.0/1.1 状态机解析、Keep-Alive、分块编码、请求体大小限制、背压控制、`Content-Type` 自动协商。
- **中间件链（洋葱模型）**：`AccessLog → RateLimiter → CORS → Auth → Gzip` 五层可插拔，顺序敏感的设计已踩过坑并在测试里固化。
- **可观测性**：异步日志（前后台双缓冲 + 滚动 + 进程内上下文）、Prometheus 风格 `/metrics`、Per-IP 令牌桶限流统计。
- **工程化**：GoogleTest 34 案例、CI 7 个 job 矩阵（含 ASan / TSan / UBSan / 覆盖率 / clang-tidy / 性能回归）、`compile_commands.json` + `.clangd` 完整配置。

最终性能（macOS M3 / 4 worker）：**188 K QPS、P99 = 2.15 ms、10K 长连接稳定**。

---

## 主分支状态

主仓库的可编译代码停留在 **day30 — 生产就绪发布** 状态。day01 → day30 的全部历史快照保留在 [`HISTORY/day01..day30/`](HISTORY/)，每天一个完整可编译目录。

day31 → day36 的 6 项加分实验（WebSocket / C++20 协程 / io_uring / 无锁队列 / 内存池 / muduo 横向基准）**未合入主代码树**，仅作为实验分支保留在：

- [`HISTORY/day31..day36/`](HISTORY/) — 完整可编译快照
- [`dev-log/day31..day36-*.md`](dev-log/) — 完整实现日志（顶部带实验分支说明 banner）

主分支保持精简、可生产部署的最小核心；进阶特性请进 `HISTORY/` 自取。

---

## 30 秒快速体验

```bash
git clone <this-repo> && cd Airi-Cpp-Server-Lib
cmake -S . -B build && cmake --build build -j8

# 跑全部单元测试
cd build && ctest --output-on-failure
# 100% tests passed, 0 tests failed out of 34

# 启动全特性演示服务器（中间件链 + 限流 + 鉴权 + CORS + gzip + 静态文件）
./app_demo                                 # 默认 0.0.0.0:8080
MYCPPSERVER_BIND_PORT=18888 ./app_demo     # 指定端口
```

打开浏览器访问 <http://127.0.0.1:8080/>，或：

```bash
curl http://127.0.0.1:8080/health                                        # 200
curl http://127.0.0.1:8080/metrics                                       # Prometheus 文本
curl -H "Authorization: Bearer demo-token-2024" \
     http://127.0.0.1:8080/api/users                                     # 200 JSON
curl -i -X OPTIONS -H "Origin: https://example.com" \
     -H "Access-Control-Request-Method: GET" \
     http://127.0.0.1:8080/api/users                                     # 204 + CORS 头
curl -H "Accept-Encoding: gzip" -i http://127.0.0.1:8080/static/index.html  # 200 + gzip
```

---

## 目录结构

```
Airi-Cpp-Server-Lib/
├── src/
│   ├── include/            # 公开头文件（按模块分目录）
│   │   ├── base/           # NonCopyable / SteadyClock 等基础设施
│   │   ├── log/            # AsyncLogging / Logger / LogContext
│   │   ├── net/            # EventLoop / Channel / Buffer / TcpServer / Poller…
│   │   └── http/           # HttpServer / HttpRequest / HttpResponse / 中间件
│   ├── common/             # 跨平台公共实现
│   ├── linux/ + src/mac/   # 平台特化（epoll / kqueue 实现挑一个进 NetLib）
│   └── test/               # GoogleTest 单元测试（34 个 case，10 个套件）
├── examples/
│   ├── src/http_server.cpp # 全特性演示（约 270 行，覆盖整条中间件链）
│   ├── static/index.html   # 演示前端页面（前后端已分离）
│   └── files/              # 静态文件示例（readme.txt / scores.csv / server.log）
├── demo/                   # Phase 4 早期演示服务器
├── benchmark/              # 长连接 / QPS 压测脚本与基线报告
├── dev-log/                # day01 → day36 完整开发日志（共 36 篇）
├── HISTORY/                # day01 → day36 每日快照目录
├── INTERVIEW_GUIDE.md      # 面试速通索引：30 秒电梯陈述 + 8 题 FAQ
├── 开发日志.md              # 总览 / 起源
└── .github/workflows/ci.yml  # 7-job CI 矩阵
```

---

## 核心设计要点

### 1. 多 Reactor + `one-loop-per-thread`

```
                 ┌──────────────┐
 listen fd ───▶ │ MainEventLoop │ (Acceptor + accept)
                 └──────┬───────┘
                        │ round-robin 派发
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
 ┌──────────┐   ┌──────────┐   ┌──────────┐
 │ SubLoop0 │   │ SubLoop1 │   │ SubLoopN │
 │  epoll/  │   │  epoll/  │   │  epoll/  │
 │  kqueue  │   │  kqueue  │   │  kqueue  │
 └──────────┘   └──────────┘   └──────────┘
```

- `EventLoopThreadPool` 启动 N 个 IO 线程，每个独占一个 `EventLoop`。
- 新连接通过 `eventfd` / `pipe` 唤醒方式跨线程派发到 sub loop。
- 业务回调全部在该连接所属的 sub loop 线程里跑，**全程无锁**。

### 2. 中间件链（洋葱模型）

`HttpServer::use(Middleware)` 注册顺序即执行顺序，进去的相反顺序出来：

```
请求 ──▶ AccessLog ─▶ RateLimiter ─▶ CORS ─▶ Auth ─▶ Gzip ─▶ Handler
响应 ◀── AccessLog ◀─ RateLimiter ◀─ CORS ◀─ Auth ◀─ Gzip ◀─ Handler
```

**顺序敏感的硬约束（已写进测试）**：

- **CORS 必须在 Auth 之前**——否则 OPTIONS 预检会被 Auth 拦成 401/403。
- **Gzip 必须在最里层**——否则压缩数据会被后续中间件再处理一次。
- **RateLimiter 在 AccessLog 之后**——这样被限流的请求也能记录访问日志。

### 3. 可观测性

- `AsyncLogging`：前后台双缓冲，IO 线程批量 `flush()`，业务线程零阻塞。
- `LogContext`：进程内的 traceId / requestId / 自定义字段，跨函数透传不靠参数。
- `ServerMetrics::toPrometheus()`：暴露 `http_requests_total{path,status}` 等标准指标。

### 4. 静态文件与背压

- `StaticFileHandler` 支持 `Range`、`If-Modified-Since`、自动 MIME、防路径穿越。
- `Connection` 高水位触发后暂停读，`onWriteComplete` 里恢复，避免内存爆炸。

---

## 构建选项

| CMake 选项 | 默认 | 说明 |
| --- | --- | --- |
| `MCPP_ENABLE_OPENSSL` | `ON` | 启用 TLS（自动检测 OpenSSL，找不到自动降级） |
| `MCPP_ENABLE_TESTING` | `ON` | 编译 GoogleTest 单元测试 |
| `MCPP_ENABLE_ASAN` | `OFF` | 启用 AddressSanitizer |
| `MCPP_ENABLE_TSAN` | `OFF` | 启用 ThreadSanitizer |
| `MCPP_ENABLE_UBSAN` | `OFF` | 启用 UndefinedBehaviorSanitizer |
| `MCPP_ENABLE_COVERAGE` | `OFF` | 启用 gcov / llvm-cov 覆盖率埋点 |

zlib 自动检测，找到则启用 gzip 中间件并定义 `MCPP_HAS_ZLIB=1`。

---

## 测试

```bash
cd build && ctest --output-on-failure
```

10 个测试套件 / 34 个 case：

| 套件 | 重点 |
| --- | --- |
| `LogTest` / `LogContextTest` | 异步日志线程安全、上下文透传 |
| `TimerTest` / `SteadyClockTest` | 定时器调度、跨平台单调时钟 |
| `ThreadPoolTest` | 线程池任务队列、关停语义 |
| `MetricsTest` | 计数器 / Prometheus 格式 |
| `HttpContextTest` / `HttpRequestLimitsTest` | HTTP 解析状态机、大小限制 |
| `BackpressureDecisionTest` | 高/低水位决策 |
| `CorsMiddlewareTest` | 预检短路、Allow-Origin 匹配 |
| `StaticFileHandlerTest` | Range、路径穿越防护、文件名编码 |

Sanitizer 与覆盖率请直接看 [`.github/workflows/ci.yml`](.github/workflows/ci.yml)，CI 全部跑过。

---

## CI 矩阵（共 7 个 job）

| Job | 平台 | 功能 |
| --- | --- | --- |
| `build-and-test` | Linux + macOS | Release 构建 + ctest |
| `sanitizer-asan` | Linux | AddressSanitizer 跑全部测试 |
| `sanitizer-tsan` | Linux | ThreadSanitizer 跑全部测试 |
| `sanitizer-ubsan` | Linux | UndefinedBehaviorSanitizer |
| `coverage` | Linux | gcov + lcov 覆盖率上传 |
| `static-analysis` | Linux | clang-tidy 全量扫描 |
| `benchmark-regression` | Linux | 基线 QPS 回归保护 |

---

## 性能数据（macOS / M3 / 4 worker）

| 场景 | 指标 |
| --- | --- |
| HTTP 短连接 QPS | **188 K req/s** |
| P50 / P99 延迟 | **0.42 ms / 2.15 ms** |
| 长连接保活上限 | **10 240 fds 稳定** |
| Keep-Alive QPS | **220 K req/s** |

完整压测脚本与原始报告：[`benchmark/`](benchmark/)。

---

## 性能评估

保留三个必要的评估手段，覆盖从"应用层吏吞"到"内核采样热点"三个视角：

| 维度 | 工具 | 入口 | 一句话回答 |
| --- | --- | --- | --- |
| 应用吏吞 + 资源 | wrk + `conn_scale_test` | `bench:wrk+rss` task / `scripts/bench.sh` | 吏吞 + RSS 同场别量 |
| CPU 热点 (跨平台) | macOS `sample` / Linux `perf` + FlameGraph | `bench:flamegraph` task / `scripts/flamegraph.sh` | 产出原始堆栈 + 可选 SVG 火焰图 |
| CPU 热点 (macOS 深度) | Apple `xctrace` | `bench:instruments` task / `scripts/instruments_profile.sh` | 产出 `.trace`，双击用 Instruments 打开看函数级 CPU 占比 |

历史实测报告与分析参见 [`benchmark/benchmark_report.md`](benchmark/benchmark_report.md) 和 [`benchmark/HOTSPOT_ANALYSIS.md`](benchmark/HOTSPOT_ANALYSIS.md)。

```bash
# wrk 30s + RSS 长连接测试
./scripts/bench.sh

# macOS sample / Linux perf 采样火焰图
./scripts/flamegraph.sh
# 设置 FLAMEGRAPH_DIR=~/FlameGraph 可自动渲染 SVG

# macOS 深度采样（需 sudo）
./scripts/instruments_profile.sh
```

---

## 30 天开发日志

[`dev-log/`](dev-log/) 收录了 day01 → day36 全部 36 篇日志，每篇 1~2 万字，覆盖：

- **day01–day10**：TCP socket → IO 多路复用 → Reactor 雏形 → ThreadPool。
- **day11–day20**：连接生命周期、跨平台 Poller、智能指针化、安全停机。
- **day21–day28**：one-loop-per-thread、定时器、异步日志、HTTP 协议层、CI 接入。
- **day29–day30**：限流 / CORS / Auth / Gzip 中间件、GTest 全量化、生产收尾。
- **day31–day36**（实验分支）：WebSocket、C++20 协程、io_uring、无锁队列、内存池、与 muduo 横向基准。

> day31–day36 的代码不在主分支，但日志保留并在顶部加了实验分支说明。
