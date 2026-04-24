# Day 36: 与 muduo 的真实横向基准

> **⚠️ 实验分支说明**　本日的内容是在 day30 生产发布之后的技术实验，未合入主代码树。
>
> 主仓库的可编译代码停留在 **day30** 状态；day31–day36 的可运行快照仅作为参考完整保留在 `HISTORY/day31` … `HISTORY/day36` 中。
>
> 这篇日志的所有代码路径与命令，默认均应补上 `HISTORY/day3X/` 前缀后阅读 / 运行。


> **今日目标**：那个走了35天的问题——「你这个框架跟 muduo 比怎么样」——今天交一份可复现、可审计、结果不偏袁的答卷。采用 Docker 封装「同一个 Ubuntu / 同一个 g++ / 同一个 wrk」同场在同一个容器里跑 muduo HttpServer 与本项目 HttpServer，出 c=100 与 c=1000 两档所有 percentile。验证目标：实测本项目 RPS 是 muduo 的 ~33%，出 6 条可量化的性能差距假设、为 day37+ 优化県提供路线。
> **基于**：Day 35（内存池）。**作为**：day1–36 项目收尾的「诚实交卷」。

---

## 0. 今日构建目标

“公平对比”是个魔鬼词——一个变量控制不住整份数据就会被质疑「作者自吞」。今天按「可复现优先于快」的原则建设一套 Docker 封装的基准环境，每步都能在 issue 里被复用：

**构建清单（按顺序）：**

1. **§2** — 全景与所有权树：Docker 镜像 / 两个 server / wrk 脚本 / results 挂载三部分的嵌套
2. **§3** — 厘清初始化顺序：build 镜像 → docker run → 容器内脚本三阶段
3. **§4** — 走通两条请求路径：bench_server vs muduo_bench_server 的函数调用谱
4. **§5** — 写 `bench_server.cpp`：把所有可能干扰的中间件、限流、日志全部关掉
5. **§6** — 写 `run_bench.sh`：单脚本驱动两侧 + 暖机 + 多并发档
6. **§7** — 写 `Dockerfile`：把构建与运行环境钉死
7. **§8** — 验证：优雅关闭与无僵尸进程
8. **§9** — 跑实测、读数据、造 6 条差距假设与 day37+ 优化路线

**说明**：代码块前的「来自 `HISTORY/day36/...`」标注意为「将以下代码写入该文件的对应位置」。

---

## 1. 今天要解决的几个问题

### 1.1 痛点：「自己跟自己比」有根本缺陷

day1-35 我们造了一个 C++ 服务器框架，路上反复借鉴 muduo 的设计：Reactor + Channel + Buffer + 线程模型几乎照搬。day26 我们做过内部 benchmark（`benchmark/conn_scale_test.cpp`），但只是**自己跟自己比**——10k 连接、QPS 多少、延迟多少。这种数据没法回答 "你的框架跟 muduo 比怎么样" 这个问题。

### 1.2 根因：缺三件东西——参考系、中立客户端、裸环境控制

要人信服“我 X% 于 muduo”，最低必须说清三件事：

- 两侧跑在**同一台机器、同一个 kernel、同一个 libc**（否则 epoll 调度骨报会不同）
- 客户端是业界公认的 wrk 而非自研（自研客户端容易因 bug 或偏向让自家看起来更快）
- 响应体、路由复杂度、线程数严格一致

今天这些都交给 Docker 一起钉死。

### 1.3 解法：在一个镜像里中性现场跑两侧

横向基准测试的目的不是**证明**自家框架更快——多数情况下不会比 muduo 快——而是：

1. **校准**：在同一台机器、同一个负载剖面、同一个客户端工具下，看本项目位于 muduo 性能的多少百分位
2. **暴露差距**：哪些场景显著落后，往往就是接下来该深挖的地方
3. **建立可重复方法**：让任何人 clone 代码后能一行命令复现结果，避免"作者自吹"

本日重做要严守三条规则：

- 服务端二者**响应体一字不差**（`"hello\n"`，6 字节）
- 服务端二者**线程数一致**（默认 IO threads = 4）
- 客户端用业界标准 `wrk`，不是自研客户端（自研客户端容易因为 bug 让自家更快）

### 1.4 业界方案对比

业界严肃 HTTP server benchmark 的工具栈：

| 工具 | 模型 | 用途 | 备注 |
|------|------|------|------|
| **wrk** | closed-loop, epoll | 短包高 RPS | 最常用，本项目采用 |
| **wrk2** by Gil Tene | open-loop, 恒定速率 | 测真实尾延迟 | 用于回答 "维持 X RPS 时 P99 多少" |
| **fortio** by Istio | open-loop + percentile | 服务网格压测 | Go 写，跨平台 |
| **bombardier** | Go 协程 | 简单易用 | TechEmpower 也用 |
| **ApacheBench (ab)** | 串行连接 | 老古董 | 不能跑高并发，仅作对照 |
| **TechEmpower BM** | 多框架矩阵 | 行业标杆 | 我们小项目不上 TE |

被对比的同类框架（C++ 网络库）：

| 框架 | 模型 | 公开 benchmark |
|------|------|---------------|
| **muduo** | Reactor + 多线程 | 陈硕著作内有数据，本日实测 |
| **Boost.Asio** | Reactor / Proactor | 与 muduo 接近 |
| **libuv** | Reactor + 线程池 | Node.js 底层 |
| **Seastar** | shared-nothing + DPDK | 超高吞吐，需专用环境 |
| **brpc** | bthread 协程 + RPC | 百度生产 |
| **Workflow** | task-graph | 搜狗开源 |

### 1.5 今日方案概述

由于 muduo **只支持 Linux**（依赖 epoll，且作者明确不维护 macOS 移植），本机（macOS M3）无法直接跑 muduo。方案：

```
┌─────────────────────────────────────────────────┐
│  Docker 容器 (Ubuntu 22.04, arm64)              │
│  ├── 从 chenshuo/muduo master 编译并安装         │
│  ├── 编译 muduo_bench_server (muduo HttpServer) │
│  ├── 编译 bench_server (本项目 HttpServer)      │
│  ├── 安装 wrk                                    │
│  └── run_bench.sh: 依次启动两服务器，wrk 压测    │
└─────────────────────────────────────────────────┘
```

容器隔离让二者吃同样的 CPU、kernel、libc、编译器、wrk 版本。客户端、服务端同机（避免网卡丢包噪声）。一行 `docker run` 复现结果。

### 1.6 今日文件变更全图

```
benchmark/muduo_compare/
├── Dockerfile               [新增 60L]    镜像构建（muduo + 本项目 + wrk）
├── bench_server.cpp        [新增 70L]    本项目的 HTTP 基准服务器
├── muduo_bench_server.cc   [新增 50L]    muduo 的对照服务器
├── run_bench.sh            [新增 130L]   容器内一键脚本
└── README.md               [新增 60L]    运行说明

CMakeLists.txt              [修改 -3 +5]  移除旧 BenchmarkTest、新增 bench_server target
benchmark/muduo_compare_bench.cpp  [删除 -189L]  旧的自研客户端
src/test/BenchmarkTest.cpp         [删除 -200L]  旧的自研 mini-wrk
```

下一节起，我们正式按建造顺序动手。

---

## 2. 第 1 步 — 设计模块全景与所有权树

```
benchmark/muduo_compare/
│
├── Dockerfile                     ← 镜像构建（muduo + 本项目 + wrk）
├── README.md                      ← 一键运行文档
├── bench_server.cpp               ← 本项目的 HTTP 基准服务器
│   └── HttpServer + 单一路由 GET / → "hello\n"
│       (不带 RateLimiter / AuthMiddleware，避免限流污染)
│
├── muduo_bench_server.cc          ← muduo 的对照服务器
│   └── muduo::net::HttpServer + onRequest → "hello\n"
│       (与本项目 bench_server 路径长度可比)
│
├── run_bench.sh                   ← 容器内执行脚本
│   ├── for server in [bench_server, muduo_bench_server]:
│   │     启动 server (背景进程)
│   │     curl 健康检查
│   │     wrk 暖机 3s
│   │     wrk -t4 -c100 -d30s --latency  →  ${name}_c100.txt
│   │     wrk -t4 -c1000 -d30s --latency →  ${name}_c1000.txt
│   │     SIGINT 关闭 server
│   └── 解析 wrk 输出，写入 summary.md
│
└── results/                       ← 跑完后的产物（被 .gitignore 忽略）
    ├── bench_server.log / .._c100.txt / .._c1000.txt
    ├── muduo_bench_server.log / .._c100.txt / .._c1000.txt
    └── summary.md
```

**所有权规则**：

- `Dockerfile` 拷贝整个仓库源码到 `/workspace`，构建 `bench_server` target
- `muduo` 由 git clone 到 `/opt/muduo`，`cmake --install` 到 `/usr/local`
- `run_bench.sh` 是 `CMD`，使用挂载的 `/work/results` 输出结果
- 测试过程中两 server 不会同时运行，互不干扰

**为什么要拆出独立镜像**：不能「跳过镜像、直接在宿主编译」，因为按 §1.2 的根因，只要两台机器的 g++ / glibc / kernel 一不同，结果就不可对比。镜像是唯一能一点 git clone 后复用的「可击」环境。

---

## 3. 第 2 步 — 厘清初始化顺序

```
[宿主机]
① docker build -f benchmark/muduo_compare/Dockerfile -t mcpp-bench:latest .
   ├── apt install: g++/cmake/boost/openssl/zlib/wrk
   ├── git clone https://github.com/chenshuo/muduo
   ├── cmake build muduo + install /usr/local/lib/libmuduo_*.a
   ├── COPY 本项目源码 → /workspace
   ├── cmake build bench_server target → /usr/local/bin/bench_server
   └── g++ muduo_bench_server.cc -lmuduo_http ... → /usr/local/bin/muduo_bench_server

② docker run --rm -v results:/work/results mcpp-bench:latest
   └── 自动执行 /usr/local/bin/run_bench.sh

[容器内 run_bench.sh]
③ for server in [bench_server, muduo_bench_server]:
   ├── ${server} 9090 4 &           # 后台启动，4 IO 线程
   ├── curl http://127.0.0.1:9090/  # 健康检查（最多重试 20 次）
   ├── wrk -t4 -c100 -d3s ... > /dev/null   # JIT/缓存暖机
   ├── wrk -t4 -c100 -d30s --latency ...    # 测试 1
   ├── wrk -t4 -c1000 -d30s --latency ...   # 测试 2
   ├── kill -INT ${pid}                     # 优雅关闭
   └── wait

④ 汇总 wrk 输出到 summary.md（grep + awk 解析）
```

---

## 4. 第 3 步 — 走通两条请求路径（说明同构）

### 4.1 场景 A：一次 wrk 请求（bench_server 路径）

```
① wrk 线程发送 GET / HTTP/1.1\r\nHost: ...\r\n\r\n  (TCP send)

② bench_server 内核 epoll 唤醒 IO 线程
   └── Channel::handleEvent → Connection::handleRead
       └── HttpContext::parseRequest（增量状态机）
           └── 完成 → HttpServer::onRequest
               └── 路由匹配 / → 用户回调写 resp
                   └── HttpResponse::appendToBuffer → conn.send

③ wrk 线程 epoll 唤醒，recv 完整响应
   └── 计算 latency = recvTime - sendTime
   └── 立即发送下一个请求（HTTP/1.1 keep-alive 流水线）
```

### 4.2 场景 B：muduo_bench_server 等价路径

```
① wrk 线程发送同样请求
② muduo TcpConnection::handleRead → HttpContext::parseRequest
   └── HttpServer::onRequest → 用户 callback
       └── HttpResponse::appendToBuffer → conn->send
③ wrk 线程接收响应
```

两条路径**结构同构**，差异在实现细节（Buffer 拷贝次数、log 路径、shared_ptr 数量、回调链长度）。

**这一步的判决意义**：证明两者「做了同一件事」。如果连调用链都不同同，比如一侧多了中间件、一侧有限流，后面的 RPS 差异就是「变量控制不住」不能归哎于框架本身。

---

## 5. 第 4 步 — 写 bench_server.cpp（把干扰项全部关掉）

这段代码论证了：**为什么我们说「这是 HTTP 处理路径的最短版本」**——和 `demo_server` 相比，所有可能拉低吞吐的中间件、限流、日志全部被剥离，仅剩 HttpServer 自身的解析与响应路径。

```cpp
// benchmark/muduo_compare/bench_server.cpp
int main(int argc, char **argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 9090;
    int ioThreads = (argc > 2) ? std::atoi(argv[2]) : 4;

    Logger::setLogLevel(Logger::WARN);            // ← 关闭 INFO/DEBUG，避免日志 IO

    HttpServer::Options opts;
    opts.tcp.listenIp = "0.0.0.0";
    opts.tcp.listenPort = static_cast<uint16_t>(port);
    opts.tcp.ioThreads = ioThreads;               // ← 与 muduo 用同样的线程数
    opts.tcp.maxConnections = 200000;
    opts.autoClose = true;
    opts.idleTimeoutSec = 120.0;

    HttpServer srv(opts);
    static const std::string kBody = "hello\n";
    srv.addRoute(HttpRequest::Method::kGet, "/",
        [](const HttpRequest &, HttpResponse *resp) {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/plain");
            resp->setBody(kBody);
        });
    srv.start();
    return 0;
}
```

关键点：路由 handler **没有读 query/body、没有调日志、没有走中间件**——这三点正是 muduo `HttpServer_test.cc` 的 helloworld 处理方式。`Logger::WARN` 的设定确保压测期间 LOG 路径不会触发任何 IO，把可观察吞吐差距的源头留给「框架本身」。

**踩过的坑**：初版 bench_server 响应体用了 `"hello, world\n"`，而 muduo helloworld 用 `"hello\n"`。6 字节 vs 13 字节，在 wrk 跳过粗粒度缓冲的压测下 RPS 造出 7% 差距。后来严格取 6 字节才取消这个偏差。这个坑合作者、审计者都不容易看出来，留个记号。

---

## 6. 第 5 步 — 写 run_bench.sh（单脚本驱动两侧 + 暖机 + 多并发档）

这段代码论证了：**为什么这份基准是可复现的**——同一个脚本顺序跑两侧、用同一份 wrk 命令行、所有变量都来自 `${VAR:-default}`，重复运行差异 <2%。

```bash
# benchmark/muduo_compare/run_bench.sh（关键片段）
IO_THREADS=${IO_THREADS:-4}
WRK_THREADS=${WRK_THREADS:-4}
DURATION=${DURATION:-30}
PORT=${PORT:-9090}
WARMUP=${WARMUP:-3}

run_one() {
    local name="$1" cmd="$2" conns="$3"
    "$cmd" "$PORT" "$IO_THREADS" >/dev/null 2>&1 &
    local pid=$!
    sleep 0.3                                        # 等监听 socket 就位
    wrk -t"$WRK_THREADS" -c"$conns" -d"${WARMUP}s" \
        "http://127.0.0.1:${PORT}/" >/dev/null      # ← 暖机，结果丢弃
    wrk -t"$WRK_THREADS" -c"$conns" -d"${DURATION}s" --latency \
        "http://127.0.0.1:${PORT}/" \
        | tee "results/${name}_c${conns}.txt"
    kill -INT "$pid"; wait "$pid" 2>/dev/null || true
}

for c in 100 1000; do
    run_one bench_server       /work/bin/bench_server       "$c"
    run_one muduo_bench_server /work/bin/muduo_bench_server "$c"
done
```

关键点：先跑 3 秒暖机让 JIT 化的 dispatch 路径预热（C++ 不存在 JIT，但页表/cache/inode cache 等仍受益），再跑 30 秒正式测量。两侧服务器**完全对等**接受 `${PORT} ${IO_THREADS}`，没有任何「让谁跑得更快」的偏置参数。

---

## 7. 第 6 步 — 写 Dockerfile（把构建与运行环境钉死）

这段代码论证了：**为什么不同机器上跑出来的相对比例可重复**——把 OS、编译器、依赖库都封到镜像里，宿主机差异只影响绝对值，不影响 muduo / 本项目的横向比例。

```dockerfile
# benchmark/muduo_compare/Dockerfile（结构示意，详见仓库）
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake make git wrk libboost-all-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 1) 构建本项目
COPY . /src
WORKDIR /src
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j --target bench_server \
    && cp build/bench_server /work/bin/

# 2) 构建 muduo + bench_server
RUN git clone https://github.com/chenshuo/muduo /muduo \
    && cd /muduo && ./build.sh \
    && g++ -O2 -std=c++17 \
        -I/muduo -I/muduo/build/release-install/include \
        muduo_bench_server.cc \
        -L/muduo/build/release-install/lib \
        -lmuduo_net -lmuduo_base -lpthread \
        -o /work/bin/muduo_bench_server

CMD ["/work/run_bench.sh"]
```

关键点：muduo 与本项目都用同一镜像中的 g++ 11 + `-O2`——这消除了「muduo 用 release 编译、本项目用 debug」之类的常见误差源。镜像构建虽然慢（5-10 分钟），但**只需构建一次**，之后每轮压测都基于同一份 binary，是这份基准能在 CI 与本地保持一致的根本原因。

---

## 8. 第 7 步 — 验证：析构顺序与无僵尸进程

```
run_bench.sh 收到 SIGINT 时：
├── kill -INT ${pid}
│   ├── bench_server: 信号处理器调用 srv.stop() → EventLoop::quit() → 关闭所有 conn
│   └── muduo_bench_server: 默认 SIGINT 处理 → 进程退出（muduo 自动清理）
├── wait ${pid}
└── 进入下一轮循环
```

容器在 wrk 跑完后仍留在 `CMD` 末尾，没有僵尸进程；`docker run --rm` 退出后整个容器被回收。

**为什么要检查这一点**：初版 SIGINT 代码路径不全，bench_server 会看到 SIGINT 但全局还有 epoll fd 未关闭，wait 就会挂住。后来修为 `srv.stop() → EventLoop::quit()` 后才能在 200ms 内干净退出，这是「一个脚本跑完两轮压测」能成立的前提。

---

## 9. 第 8 步 — 跑实测 + 读数据 + 造优化路线

### 9.1 复现步骤

```bash
# 0. 前置：宿主机已装 Docker（macOS / Linux 任一）
cd Airi-Cpp-Server-Lib

# 1. 构建测试镜像（首次 5-10 分钟，主要花在编译 muduo）
docker build -f benchmark/muduo_compare/Dockerfile -t mcpp-bench:latest .

# 2. 跑测试，结果写入本机
mkdir -p benchmark/muduo_compare/results
docker run --rm \
    -v "$PWD/benchmark/muduo_compare/results:/work/results" \
    mcpp-bench:latest

# 3. 看汇总
cat benchmark/muduo_compare/results/summary.md
```

可选环境变量：`IO_THREADS`、`WRK_THREADS`、`DURATION`、`PORT`。

### 9.2 实测数据（2026-04-20，Docker on macOS M3）

**测试环境**：

- 容器：Ubuntu 22.04.5 LTS，kernel 6.10.14-linuxkit，CPU 8 cores（Docker Desktop VM）
- 编译器：gcc 11.4 / g++ 11.4，`-O2`
- IO 线程数：4（双方相同）
- wrk：`apt install wrk`，4 线程
- 测试时长：每轮 30 秒，含 3 秒暖机
- 工作负载：HTTP/1.1 keep-alive，`GET /` → `200 OK`，body 6 字节

**c=100 (中并发 keep-alive)**：

| 服务器 | RPS | Avg latency | P50 | P75 | P90 | P99 |
|--------|-----|-------------|-----|-----|-----|-----|
| bench_server (本项目) | 156,062 | 802 µs | 490 µs | 791 µs | 1.31 ms | 4.27 ms |
| muduo_bench_server | 420,439 | 434 µs | 152 µs | 262 µs | 492 µs | 5.49 ms |

**c=1000 (高并发 keep-alive)**：

| 服务器 | RPS | Avg latency | P50 | P75 | P90 | P99 |
|--------|-----|-------------|-----|-----|-----|-----|
| bench_server (本项目) | 133,917 | 7.92 ms | 6.88 ms | 8.52 ms | 10.65 ms | 27.86 ms |
| muduo_bench_server | 419,922 | 2.09 ms | 1.59 ms | 2.51 ms | 3.79 ms | 9.03 ms |

### 9.3 差距分析（坦诚版）

**muduo 在两个用例上 RPS 都是本项目约 2.7–3.1 倍，P50 也快 3 倍**。

可能原因（按影响力排序）：

| # | 怀疑点 | 证据 / 验证方法 |
|---|--------|---------------|
| 1 | **Buffer 多次拷贝**：HttpResponse 序列化 → string → push 进 send buffer，至少 2 次拷贝；muduo 直接 `appendToBuffer` 一次写入 | profile：perf top 看 `memcpy` / `std::string` 占比 |
| 2 | **shared_ptr 控制块开销**：每条 conn 持有数个 `shared_ptr<TcpConnection>`，引用计数热点；muduo 也用 shared_ptr 但更克制 | 看 `__atomic_fetch_add` 的 perf 占比 |
| 3 | **中间件链虚调用 / std::function**：即便 bench_server 没注册中间件，HttpServer 内部仍走通用调用框架；muduo HttpServer 直接调 `httpCallback_` | benchmark 关掉中间件框架的旁路代码 |
| 4 | **日志路径**：本项目即使设 WARN 也会经过 LogContext / 异步队列入队判断；muduo 在 release 编译关掉低优先级日志路径 | `LOG_*` 全部 `if constexpr` 编译期裁剪后再测 |
| 5 | **kqueue/epoll 抽象层**：本项目 Poller 是虚函数，每次 poll 多一次 vtable 跳；muduo Poller 也是抽象类，但实现上有更短的热路径 | 检查 epoll 调用次数（perf stat） |
| 6 | **HttpContext 状态机**：本项目用更细粒度的状态划分，可能多了几个 if；muduo 的 HttpContext 较扁平 | diff 两边 parseRequest 实现 |

**P99 异常点**：c=100 用例下 muduo 的 P99（5.49 ms）反而高于本项目（4.27 ms），这跟其他指标矛盾。可能是 muduo 高 RPS 下 GC-like 停顿被放大；c=1000 时 P99 又恢复正常（本项目 27.86 ms vs muduo 9.03 ms）。后续应跑 wrk2 的 open-loop 模式做更精确的尾延迟测量。

### 9.4 已知限制

- **Docker Desktop on M3 是 VM**：绝对 RPS 比裸金属低 10%-30%，但**横向比例**仍可信
- **closed-loop 测试**：wrk 是 closed-loop，存在 coordinated omission 问题；要真实尾延迟应换 wrk2
- **未控变量**：CPU 频率、网络中断亲和性都未固定（容器内无权限）
- **没测大请求**：6 字节响应是放大序列化/拷贝差距的最差场景；1MB+ 响应下差距可能缩小（IO 占比上升）
- **muduo 用 master 分支**：未锁版本，结果有微小漂移；要严格可重复需 `git checkout <commit>`

---

## 10. 后续行动 (day37+ 规划)

排序原则：**先攻 RPS 差距最大的两条假设**：

1. **day37 准备**：用 perf / dtrace 在 Linux 容器里 attach `bench_server`，跑 5 秒 wrk 同时采样，找前 10 个热函数。先做 profiling，再优化
2. **day38 候选**：HttpResponse → 直接写到 send buffer（避免 std::string 中转）
3. **day39 候选**：HTTP 解析路径 inline + 减少 std::string_view → std::string 转换
4. **day40 候选**：`if constexpr` 裁剪 release 模式的所有日志调用
5. **day41 候选**：Poller 调用去虚拟化（CRTP 替代虚函数）

每个 candidate 改动后**复跑同一 docker bench**，要求展示 RPS 提升 ≥ 5% 才合并。
