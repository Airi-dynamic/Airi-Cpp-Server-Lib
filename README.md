# Day 29 — 生产特性（请求限流 / TLS 预留 / sendFile / 路由表 / 中间件链）

将 HTTP 框架从"demo 可用"推向"生产就绪"：请求体积限流 + 413 响应、TLS 接口预留、零拷贝文件发送快路径、路由表 + 前缀路由、中间件管道、请求超时保护。

## 新增 / 变更模块

| 文件 | 说明 |
|------|------|
| `include/http/HttpContext.h` | 新增 `Limits` 结构体 + `payloadTooLarge()` + 限制检查 |
| `common/http/HttpContext.cpp` | `parse()` 中实时累加各部分字节数，超限停止解析 |
| `include/http/HttpRequest.h` | 新增 `kOptions` 方法枚举；`normalizeHeaderKey()` |
| `include/http/HttpResponse.h` | 新增 204/206/304/408/413/416 状态码；`sendFile` 快路径描述 |
| `include/http/HttpServer.h` | 路由表 + 前缀路由 + 中间件链 + `Options.limits` / `requestTimeoutSec` |
| `common/http/HttpServer.cpp` | 路由分发 + 中间件链执行 + 请求超时 + 413 响应 + sendFile |
| `include/Connection.h` | TLS 接口预留（`#ifdef MCPP_HAS_OPENSSL`）；`sendFile()` |
| `common/Connection.cpp` | sendFile 实现（sendfile 系统调用 + 降级读写） |
| `include/TcpServer.h` | `Options::TlsOptions` 结构体 |
| `test/HttpRequestLimitsTest.cpp` | **新增** — 请求行/头部/体限制测试 + 限制内正常解析 |

## 构建 & 运行

```bash
cd HISTORY/day29
cmake -S . -B build && cmake --build build -j4

# 请求限制测试
./build/HttpRequestLimitsTest

# 其他测试
./build/BackpressureDecisionTest
./build/HttpContextTest
./build/SocketPolicyTest
./build/TcpServerPolicyTest
./build/EpollPolicyTest        # macOS 自动跳过

# 启动 server + client
./build/server &
./build/client
```

## 可执行文件

| 名称 | 说明 |
|------|------|
| `server` | TCP Echo 服务器 (test/) |
| `client` | TCP 客户端 (test/) |
| `BackpressureDecisionTest` | 回压策略决策测试 |
| `EpollPolicyTest` | epoll 自愈重试策略测试 |
| `HttpContextTest` | HTTP 解析器测试 |
| `HttpRequestLimitsTest` | HTTP 请求限流测试 |
| `SocketPolicyTest` | Socket 返回值语义测试 |
| `TcpServerPolicyTest` | 连接上限 / 线程归一化测试 |
| `LogTest` | 日志系统测试 |
| `TimerTest` | 定时器测试 |
| `ThreadPoolTest` | 线程池测试 |
| `StressTest` | 压力测试客户端 |
| `BenchmarkTest` | HTTP 压测工具 |

## 关键概念

- **请求限流**：HttpContext 在逐字符解析过程中实时累加字节数，超限时设置 `payloadTooLarge` 并返回 413
- **路由表**：精确匹配（`unordered_map<string, handler>`）优先于前缀匹配（`vector<PrefixRoute>`）
- **中间件链**：`use()` 注册的中间件按顺序执行，不调用 `next()` 即可中断链路
- **sendFile**：`HttpResponse::setSendFile()` 标记文件路径，`Connection::sendFile()` 使用 sendfile 系统调用
- **TLS 预留**：`Connection` 和 `TcpServer` 中使用 `#ifdef MCPP_HAS_OPENSSL` 条件编译，实际功能待后续启用
