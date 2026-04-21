# Day 29 — 生产特性：请求限流 / TLS 预留 / sendFile / 路由表 / 中间件链

> **今日目标**：在 Day 28 的基础上，动手完成 9 项生产特性：三层限流、路由表、中间件链、零拷贝 sendFile、CORS、Gzip、静态文件处理、超时保护和 TLS 接口预留。
> **基于**：Day 28（测试框架与 CI）。

---

## 0. 今日构建目标

在 Day 28 的回压与策略测试基础上，今天你将完成 9 项生产特性。各步骤相对独立，但建议按顺序阅读——后面的实现会用到前面建立的接口与概念。

**构建清单（按顺序）：**

1. **§2** — 三层请求限流：给 `HttpContext` 加 `Limits` 结构体，在状态机内逐字节计数，超限发 413
2. **§3** — 路由表：用 `addRoute()` / `addPrefixRoute()` 替换 if-else 路由分发
3. **§4** — 中间件链：用 `use()` 注册洋葱模型中间件管道，写 `runChain` 递归闭包
4. **§5** — 零拷贝文件发送：给 `Connection` 加 `sendFile()`，优先调用内核 `sendfile()`
5. **§6** — 内置 CORS 中间件：写 `CorsMiddleware`，处理预检 OPTIONS + 通用跨域头注入
6. **§7** — 内置 Gzip 中间件：写 `GzipMiddleware`，对响应体做 zlib 压缩
7. **§8** — 静态文件处理器：写 `StaticFileHandler`，支持 ETag / Range / 304 / Content-Disposition
8. **§9** — 请求超时保护：在 `Options` 里加 `requestTimeoutSec`，Slowloris 防护，超时返 408
9. **§10** — TLS 接口预留：用 `#ifdef MCPP_HAS_OPENSSL` 守卫的接口桩，为未来 OpenSSL 集成占位

**说明**：代码块前的「来自 `HISTORY/day29/...`」标注意为「将以下代码写入该文件的对应位置」，跟着每步动手输入即可。

---

## 1. 今天要解决的几个问题

### 1.1 Day 28 之后还剩哪些缺口

经过 Day 28 的测试体系建设，Airi-Cpp-Server-Lib 已经拥有了回压机制、连接上限保护和可测试的策略函数。但作为一个 HTTP 框架，它距离 "生产就绪" 仍有显著差距：

1. **无请求体积限制**：恶意客户端发送 `Content-Length: 999999999` 的请求，或一行无限长的请求行，解析器会忠实地接收直到内存耗尽。Day 28 的回压机制保护的是**输出缓冲区**，而**输入侧**完全不设防。
2. **无路由表**：所有请求通过单一的 `httpCallback_` 处理。用户必须在回调中手写 `if (req.url() == "/api/users") ... else if ...`。当路由数 > 10，业务代码就变成意大利面。
3. **无中间件机制**：日志记录、鉴权、CORS、Gzip 等横切关注点只能硬编码在业务回调中。每个路由都要重复相同的前置/后置逻辑。
4. **静态文件走用户态拷贝**：100 MB 文件下载，Day 27 的方式是 `read(file_fd, buf, 100MB)` → `write(sock_fd, buf, 100MB)`，经过两次内核↔用户态拷贝。Linux 的 `sendfile()` 直接在内核态完成，节省一半内存带宽。
5. **无 ETag / Range / 304**：浏览器命中本地缓存时仍然全量重新下载；视频拖动播放无法做断点续传；下载中断必须从头开始。
6. **无 CORS 支持**：前端 SPA（React / Vue）从 `localhost:3000` 访问后端 `localhost:8888` 直接被浏览器同源策略拦截。
7. **无 Gzip 压缩**：5 KB 的 JSON 响应不压缩直发，对带宽敏感的移动端用户体验差。
8. **无请求级超时**：慢速攻击（Slowloris）可以打开连接后以极慢的速度发送数据（如每秒 1 字节），占用服务器连接槽位却不释放。Day 21 的空闲超时只检测 "无任何数据交互"，不保护 "正在缓慢发送请求" 的场景。
9. **无 TLS 支持**：所有通信都是明文。在公网环境不可接受。即便不立刻完成 OpenSSL 集成，至少要在架构层面预留接口。

### 1.2 各问题的真实触发场景

| 问题 | 真实触发 |
|------|---------|
| 请求体积无限制 | curl `-X POST` 上传一个 8 GB 的视频，服务器 RSS 直接被打满，OOM Killer 介入 |
| 路由 if-else 地狱 | demo 项目从 3 个路由扩到 30 个，`server.cpp` 涨到 600 行，新增路由要小心翼翼避免改错前面 |
| 用户态文件拷贝 | `wrk -t4 -c200` 下载 100 MB 文件，CPU 100%，瓶颈不在网络而在 memcpy |
| 浏览器全量回拉 | Chrome DevTools Network 面板显示 200 OK 而非 304，每次刷新都重新下载 1.5 MB 的 JS bundle |
| Slowloris | nmap 脚本以 1 字节/秒 速率发送 1024 个连接，服务器 fd 耗尽，正常请求被拒绝 |
| CORS 失败 | 前端控制台显示 `No 'Access-Control-Allow-Origin' header is present`，整个 SPA 不能调后端 |

### 1.3 今日方案概览

**防御线**（限流 + 超时 + TLS 接口）：

- `HttpContext::Limits{maxRequestLineBytes, maxHeaderBytes, maxBodyBytes}`，在状态机内逐字符累加，超限立刻 `state_=kInvalid` + `payloadTooLarge_=true`，上层据此发 413 而非 400。
- `HttpServer::Options::requestTimeoutSec`，在 `onMessage()` 入口检查从 `requestStart` 到现在的耗时，超时即 408 + 关闭。
- `#ifdef MCPP_HAS_OPENSSL` 守卫的接口预留：`Connection::enableTlsServer / readFromTransport / writeToTransport / driveTlsHandshake`、`TcpServer::Options::TlsOptions`，以便未来集成 OpenSSL 时不需要再大改核心代码。

**功能线**（路由 + 中间件 + 零拷贝 + 静态资源 + CORS + Gzip）：

- `addRoute / addPrefixRoute` 让用户用 `srv.addRoute(GET, "/api/users", handler)` 替代 if-else。
- `use(Middleware)` 注册中间件，递归闭包按 Express 洋葱模型执行。
- `Connection::sendFile(path, offset, count)` 优先 `sendfile()`，无该系统调用时降级为分块 `read+write`。
- `StaticFileHandler::serve()` 一站式提供 ETag、`If-None-Match → 304`、`Range → 206`、`Content-Disposition`、自动 MIME 推断。
- `CorsMiddleware` / `GzipMiddleware` 作为开箱即用的内置中间件。

### 1.4 今日文件变更全图

| 文件 | 状态 | 关键改动 |
|------|------|---------|
| `include/http/HttpContext.h` | **修改** | 新增 `Limits` + `payloadTooLarge_` + 三个字节计数器 |
| `common/http/HttpContext.cpp` | **修改** | 状态机各分支入口加计数与限流检查；`reset()` 清零 |
| `include/http/HttpServer.h` | **修改** | `Options.limits / requestTimeoutSec`、路由表、中间件链、PrefixRoute |
| `common/http/HttpServer.cpp` | **修改** | `dispatch` lambda、`runChain` 递归、超时 408、payload 413、sendFile 调用 |
| `include/http/HttpResponse.h` | **修改** | 新增 204/206/304/408/413/416 状态码；`setSendFile / hasSendFile` |
| `common/http/HttpResponse.cpp` | **修改** | 新状态码序列化；sendFile 描述字段 |
| `include/http/HttpRequest.h` | **修改** | 新增 `kOptions` 方法、`normalizeHeaderKey` |
| `include/http/CorsMiddleware.h` | **新增** | 预检 204 / 通用 CORS 头注入 |
| `include/http/GzipMiddleware.h` | **新增** | 响应体压缩（zlib，可选） |
| `include/http/StaticFileHandler.h` | **新增** | ETag + Last-Modified + Range + 304 + 自动 Content-Type |
| `include/Connection.h` | **修改** | `sendFile()`、`#ifdef MCPP_HAS_OPENSSL` TLS 预留 |
| `common/Connection.cpp` | **修改** | sendFile 实现（sendfile + 降级）；TLS 桩 |
| `include/TcpServer.h` | **修改** | `Options::TlsOptions` |
| `test/HttpRequestLimitsTest.cpp` | **新增** | 4 个限流边界用例 |

---

## 2. 第 1 步 — 给 HttpContext 加三层请求限流（413）

### 2.1 问题背景

凌晨 02:13，`#ops-alert` Slack 频道弹出红色告警：

```
[OOM] node-prod-7  RSS 7.8 GiB / 8 GiB  killed by oom-killer
```

回放 access log，定位到一条 POST 请求：

```
2026-04-21T02:12:54  203.0.113.42  POST /upload  Content-Length: 8589934592   (开始)
2026-04-21T02:13:07  203.0.113.42  POST /upload  -- 进程被杀 --
```

攻击方根本没有真正发完 8 GiB，只发了几兆，但 Day 28 的代码里 `HttpContext::parse` 会一直 `tokenBuf_ += ch` 累加 body，直到 Content-Length 满足；在数据慢慢到达的 13 秒内，`tokenBuf_` 增长被 std::string realloc 与 Buffer 一起放大，撞穿 RSS 上限。

实施限流后的预期时序（用 `maxBodyBytes=10MB` 做配置）：

| 时刻 | 事件 | 计数器状态 | 决策结果 |
|------|------|------------|----------|
| T0 | TCP 数据 0..1024 字节到达 | `requestLineBytes_=20`, `headerBytes_=240`, `bodyLen_=10MB+1`（解析到 `Content-Length: 10485761`） | `bodyLen_ > maxBodyBytes` → `state_=kInvalid` + `payloadTooLarge_=true` |
| T1 | HttpServer 检测 `payloadTooLarge()` | — | 写出 `413 Payload Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n` |
| T2 | `conn->close()` | — | 连接进入 `kClosed`，sub-reactor 释放 fd |
| T3 | RSS 监控 | 进程 RSS 增量 < 1 MB | 不会触发 OOM |

### 2.2 打开 HttpContext.h：加入 Limits 与状态字段

来自 [HISTORY/day29/include/http/HttpContext.h](HISTORY/day29/include/http/HttpContext.h)（行 21–26、80–88）：

```cpp
struct Limits {
    int maxRequestLineBytes{8 * 1024};
    int maxHeaderBytes{32 * 1024};
    int maxBodyBytes{10 * 1024 * 1024};
};
```

```cpp
int bodyLen_{0};
int requestLineBytes_{0};
int headerBytes_{0};
Limits limits_{};
bool payloadTooLarge_{false};
```

三个字节计数器分别对应 HTTP 报文的三个段：请求行（METHOD URL HTTP/1.x）、所有请求头之和、请求体。`payloadTooLarge_` 是出口标志，下游 `HttpServer::onMessage` 据此区分 400 vs 413。

### 2.3 纯检查嵌入状态机 — 每分支都要计数

来自 [HISTORY/day29/common/http/HttpContext.cpp](HISTORY/day29/common/http/HttpContext.cpp)（行 47–63，方法名分支）：

```cpp
case State::kMethod:
    ++requestLineBytes_;
    if (requestLineBytes_ > limits_.maxRequestLineBytes) {
        payloadTooLarge_ = true;
        state_ = State::kInvalid;
        break;
    }
    if (std::isupper(ch)) {
        tokenBuf_ += ch;
    } else if (std::isblank(ch)) {
        if (!request_.setMethod(tokenBuf_)) {
            state_ = State::kInvalid;
            break;
        }
        tokenBuf_.clear();
        state_ = State::kBeforeUrl;
    } else {
        state_ = State::kInvalid;
    }
    break;
```

注意：**计数和判断必须放在每个 case 的最前面**——只有这样，超限发现的瞬间才能立刻 `break` 出 switch，不再继续追加 `tokenBuf_`。

### 2.4 reset 必须清零三个计数器

来自 [HISTORY/day29/common/http/HttpContext.cpp](HISTORY/day29/common/http/HttpContext.cpp)（行 9–18）：

```cpp
void HttpContext::reset() {
    state_ = State::kStart;
    request_.reset();
    tokenBuf_.clear();
    colonBuf_.clear();
    bodyLen_ = 0;
    requestLineBytes_ = 0;
    headerBytes_ = 0;
    payloadTooLarge_ = false;
}
```

HTTP/1.1 keep-alive 在解析完一个完整请求后会调用 `reset()` 准备下一个；忘记清零计数器会导致同一连接的第二个请求即使本身合法也被误判超限。

### 2.5 上层据 `payloadTooLarge()` 区分 413 vs 400

来自 [HISTORY/day29/common/http/HttpServer.cpp](HISTORY/day29/common/http/HttpServer.cpp)（行 132–144）：

```cpp
int consumed = 0;
if (!ctx->parser.parse(buf->peek(), static_cast<int>(buf->readableBytes()), &consumed)) {
    LOG_WARN << "[HttpServer] 非法请求，fd=" << conn->getSocket()->getFd();
    if (ctx->parser.payloadTooLarge()) {
        conn->send("HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\nContent-Length: "
                   "0\r\n\r\n");
    } else {
        conn->send(
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    }
    conn->close();
    return;
}
```

### 2.6 验证：追踪 9 KB 请求行被限流的过程

#### 业务场景 + 时序总览表

`maxRequestLineBytes = 8192`，客户端发了 `GET /` 然后跟着 9000 个字符的查询参数：

| 时刻 | 事件 | `requestLineBytes_` | `state_` | `payloadTooLarge_` | 决策 |
|------|------|---------------------|----------|--------------------|------|
| T0 | 收到首包 1500 字节，进入 `parse(data,1500,&c)` | 0 | kStart | false | 进入 while 循环 |
| T1 | 解析完 `GET ` + `/` + 部分查询 keyvalue | 4096 | kQueryValue | false | 一路推进，未超限 |
| T2 | 第二包 1500 字节到达，再次进入 parse | 4096 | kQueryValue | false | 继续推进 |
| T3 | 累加到第 8193 个请求行字符 | 8193 | kQueryValue → **kInvalid** | **true** | break，return false |
| T4 | HttpServer 检查 `payloadTooLarge()` | — | kInvalid | true | 写 413 + close |
| T5 | Connection 析构 | — | — | — | fd 关闭、内存释放 |

#### 第 1 步：进入 `kQueryValue` 分支并触发计数

代码（来自 [HISTORY/day29/common/http/HttpContext.cpp](HISTORY/day29/common/http/HttpContext.cpp) 行 132–151）：

```cpp
case State::kQueryValue:
    ++requestLineBytes_;
    if (requestLineBytes_ > limits_.maxRequestLineBytes) {
        payloadTooLarge_ = true;
        state_ = State::kInvalid;
        break;
    }
    if (ch == '&') {
        request_.addQueryParam(colonBuf_, tokenBuf_);
        colonBuf_.clear();
        tokenBuf_.clear();
        state_ = State::kQueryKey;
    } else if (std::isblank(ch)) {
        request_.addQueryParam(colonBuf_, tokenBuf_);
        colonBuf_.clear();
        tokenBuf_.clear();
        state_ = State::kBeforeProtocol;
    } else {
        tokenBuf_ += ch;
    }
    break;
```

**代入实参**（T3 时刻）：
- `requestLineBytes_ = 8192`（计数前）
- `++requestLineBytes_ → 8193`
- `limits_.maxRequestLineBytes = 8192`
- `ch = 'a'`（查询参数中的某个普通字符）

**判断分支**：
- `8193 > 8192` ？是 → 进入 if 体
- `payloadTooLarge_ = true`
- `state_ = State::kInvalid`
- `break` 跳出 switch

**副作用快照**：

```
state_                = State::kInvalid
payloadTooLarge_      = true
requestLineBytes_     = 8193
tokenBuf_             = "..." (旧累积，不再使用)
返回 parse() 顶层 while 条件 → state_ != kInvalid 为假 → 退出 while
```

#### 第 2 步：parse 返回 false + consumed > 0

**关键不变式**：即使 parse 失败也必须返回 `consumedBytes > 0`，否则上层 `buf->retrieve(consumed)` 不会推进，下一轮 onMessage 又拿到同样的数据，陷入死循环。

#### 第 3 步：HttpServer 拦截并发 413

代码（来自 [HISTORY/day29/common/http/HttpServer.cpp](HISTORY/day29/common/http/HttpServer.cpp) 行 132–144）：上文已贴。

**代入实参**：
- `parse(...) = false`
- `payloadTooLarge() = true`

**分支走向**：
- `!false = true` → 进入 if
- `payloadTooLarge() == true` → 走 413 分支
- `conn->send(413_response)` → 通过 outputBuffer 异步写出
- `conn->close()` → 设置 `state_=kClosed`，从 Poller 注销 Channel

**副作用快照**：

```
outputBuffer_ = "HTTP/1.1 413 Payload Too Large\r\n..."  （55 字节）
state_        = State::kClosed
Channel       = 已从 Poller 注销
fd            = 待关闭（在 sub-reactor 下一个 tick 释放）
```

#### 第 4 步：状态机视图

```
       超限触发点              限流响应
  ┌──────────────┐         ┌──────────────────┐
  │ requestLine  │         │ HTTP 413 + close │
  │ + > maxRL   ─┼────────▶│ payload too large│
  └──────────────┘         └──────────────────┘
        ▲ 任意状态分支命中
        │
        │  (kMethod/kBeforeUrl/kUrl/kQueryKey/kQueryValue/
        │   kBeforeProtocol/kProtocol/kBeforeVersion/kVersion)
        │
        │  ++requestLineBytes_
        │  if (requestLineBytes_ > limits.maxRequestLineBytes)
        │      payloadTooLarge_ = true; state_ = kInvalid

  ┌──────────────┐         ┌──────────────────┐
  │ headerBytes  │         │ HTTP 413 + close │
  │ + > maxHB   ─┼────────▶│ payload too large│
  └──────────────┘         └──────────────────┘
        ▲
        │  (kHeaderKey/kHeaderValue)
        │  ++headerBytes_
        │  if (headerBytes_ > limits.maxHeaderBytes)
        │      payloadTooLarge_ = true; state_ = kInvalid

  ┌──────────────┐         ┌──────────────────┐
  │ Content-Len  │         │ HTTP 413 + close │
  │ > maxBody   ─┼────────▶│ payload too large│
  └──────────────┘         └──────────────────┘
        ▲
        │  (在解析完 Content-Length 头进入 kBody 之前检查)
```

#### 第 5 步：函数职责一句话表

| 函数 / 字段 | 调用时机 | 职责 |
|------|---------|------|
| `HttpContext::parse` | TCP 数据到达 → onMessage 内 | 状态机驱动；逐字符更新计数器 |
| `requestLineBytes_/headerBytes_/bodyLen_` | 状态机各分支头部 | 累加并检测三层限流 |
| `payloadTooLarge_` | 超限的瞬间 | 出口信号，供上层区分 413/400 |
| `HttpContext::reset` | 一个完整请求处理后 | keep-alive 场景下清零计数器 |
| `HttpServer::onMessage` | sub-reactor 收到读事件 | 调用 parse；捕获限流错误并回 413 |

### 2.7 单元测试如何覆盖

来自 [HISTORY/day29/test/HttpRequestLimitsTest.cpp](HISTORY/day29/test/HttpRequestLimitsTest.cpp)（行 19–37，请求行限制）：

```cpp
HttpContext::Limits limits;
limits.maxRequestLineBytes = 16;
limits.maxHeaderBytes = 1024;
limits.maxBodyBytes = 1024;

HttpContext ctx(limits);
const std::string req = "GET /0123456789abcdef HTTP/1.1\r\n"
                        "Host: local\r\n"
                        "\r\n";

int consumed = 0;
bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
check(!ok, "超长请求行应解析失败");
check(ctx.isInvalid(), "超长请求行后状态应为 Invalid");
check(ctx.payloadTooLarge(), "超长请求行应标记 payloadTooLarge");
check(consumed > 0, "失败请求应推进消费字节，避免上层死循环");
```

**4 个断言对应 4 个不变式**：

1. `!ok` —— 解析失败时 parse 返回 false。
2. `isInvalid()` —— 状态机停在 `kInvalid`，不会被 keep-alive 重新启动。
3. `payloadTooLarge()` —— 出口信号置位，上层据此发 413。
4. `consumed > 0` —— 防止上层死循环。

请求头 / 请求体超限的另外 2 个用例（行 39–72）结构相同，只换 limits 配置和报文构造，验证 "三层限流互不影响、各自独立计数"。

第 4 个用例（行 74–96）反向验证 "限制内请求正常解析"，避免限流逻辑过度收紧导致正常流量被误杀。

---

## 3. 第 2 步 — 给 HttpServer 建路由表

### 3.1 问题背景

demo 服务在 Day 28 时是这样写业务回调的：

```cpp
srv.setHttpCallback([](const HttpRequest& req, HttpResponse* resp) {
    if (req.url() == "/") return handleRoot(req, resp);
    if (req.url() == "/api/users") return handleUsers(req, resp);
    if (req.url() == "/api/login") return handleLogin(req, resp);
    if (req.url().compare(0, 8, "/static/") == 0) return handleStatic(req, resp);
    if (req.url().compare(0, 9, "/uploads/") == 0) return handleUploads(req, resp);
    // ... 还有 25 条 ...
    handle404(req, resp);
});
```

到第 30 条路由时，回调函数 200+ 行。新加路由要小心避开 if-else 顺序 —— 前缀匹配如果错放在精确匹配之前，`/api/login` 会被 `/api/` 提前命中。Express.js 的 `app.get('/path', handler)` 之所以流行，就是把这种 if-else 编译成了**框架自动维护**的查找表。

### 3.2 打开 HttpServer.h：定义路由表接口

来自 [HISTORY/day29/include/http/HttpServer.h](HISTORY/day29/include/http/HttpServer.h)（行 45–48、90–96、105–108）：

```cpp
using RouteHandler = HttpCallback;

void addRoute(HttpRequest::Method method, const std::string &path, RouteHandler handler);
void addPrefixRoute(HttpRequest::Method method, const std::string &prefix,
                    RouteHandler handler);
```

```cpp
struct PrefixRoute {
    HttpRequest::Method method{HttpRequest::Method::kInvalid};
    std::string prefix;
    RouteHandler handler;
};
```

```cpp
std::vector<PrefixRoute> prefixRoutes_;
std::unordered_map<std::string, RouteHandler> routes_;
```

精确路由放 `unordered_map` 实现 O(1) 查找；前缀路由放 `vector` 顺序扫描（路由通常 < 50 条，没必要上 trie）。

### 3.3 写实现：路由键拼接与注册

来自 [HISTORY/day29/common/http/HttpServer.cpp](HISTORY/day29/common/http/HttpServer.cpp)（行 45–58）：

```cpp
std::string HttpServer::makeRouteKey(HttpRequest::Method method, const std::string &path) {
    return std::to_string(static_cast<int>(method)) + " " + path;
}

void HttpServer::addRoute(HttpRequest::Method method, const std::string &path,
                          RouteHandler handler) {
    routes_[makeRouteKey(method, path)] = std::move(handler);
}

void HttpServer::addPrefixRoute(HttpRequest::Method method, const std::string &prefix,
                                RouteHandler handler) {
    prefixRoutes_.push_back(PrefixRoute{method, prefix, std::move(handler)});
}
```

`makeRouteKey` 用 `int(method) + " " + path` 拼成字符串作为 map key。这样 `GET /a` 和 `POST /a` 是两条独立路由，不会互相覆盖。

### 3.4 嵌入执行路径 — `dispatch` lambda

来自 [HISTORY/day29/common/http/HttpServer.cpp](HISTORY/day29/common/http/HttpServer.cpp)（行 175–192）：

```cpp
auto dispatch = [&]() {
    auto it = routes_.find(makeRouteKey(req.method(), req.url()));
    if (it != routes_.end()) {
        it->second(req, &resp);
        return;
    }
    for (const auto &route : prefixRoutes_) {
        if (route.method == req.method() &&
            req.url().compare(0, route.prefix.size(), route.prefix) == 0) {
            route.handler(req, &resp);
            return;
        }
    }
    httpCallback_(req, &resp);
};
```

三级 fallback：
1. 精确匹配（unordered_map O(1)）
2. 前缀匹配（vector 线性，按注册顺序）
3. 默认 `httpCallback_`（用户可设置；未设置时 `defaultCallback` 返回 404）

### 3.5 验证：追踪一次 GET 请求的路由分发

#### 业务场景 + 时序总览表

`srv.addRoute(GET, "/api/users", h1)`、`srv.addPrefixRoute(GET, "/static/", h2)` 已注册，客户端请求 `GET /static/index.html`：

| 时刻 | 事件 | 关键状态 | 决策 |
|------|------|---------|------|
| T0 | parse 完成，进入 onRequest | `req.method=GET`, `req.url="/static/index.html"` | 构造 HttpResponse |
| T1 | runChain 跑完所有 middlewares（假设 0 个） | `index ≥ middlewares_.size()` | 调用 `dispatch()` |
| T2 | dispatch 查 routes_ | `key="1 /static/index.html"`，map 未命中 | 进入 `prefixRoutes_` 循环 |
| T3 | `prefixRoutes_[0]: {GET, "/static/"}` | `req.url.compare(0, 8, "/static/") == 0` | 命中，调用 h2 |
| T4 | h2 内部使用 `StaticFileHandler::serve` | 设置 200/304/206 + sendFile 描述 | 写响应头 |
| T5 | onRequest 检查 `resp.hasSendFile()` | true | `conn->sendFile(path, off, count)` |

#### 第 1 步：dispatch lambda 启动

代码：见 §3.4。

**代入实参**：
- `req.method() = GET`（int 值约定为 1）
- `req.url() = "/static/index.html"`
- `makeRouteKey(...) = "1 /static/index.html"`
- `routes_` 内只注册了 `"1 /api/users" → h1`

**分支**：
- `routes_.find("1 /static/index.html") == routes_.end()` → 是
- 跳过 if，进入 for 循环

#### 第 2 步：前缀循环命中

```cpp
for (const auto &route : prefixRoutes_) {
    if (route.method == req.method() &&
        req.url().compare(0, route.prefix.size(), route.prefix) == 0) {
        route.handler(req, &resp);
        return;
    }
}
```

**代入实参**（第一次迭代）：
- `route.method = GET`、`route.prefix = "/static/"`、`route.handler = h2`
- `req.method() = GET` → 第一个条件 true
- `req.url().compare(0, 8, "/static/")` —— 比较前 8 字节 → 0（相等）

**分支**：
- 两个条件都 true → 调用 `h2(req, &resp)`
- `return` 退出 dispatch

**副作用快照**：

```
resp.statusCode = 200 (或 304/206，由 h2 决定)
resp.headers    = {"Content-Type": ..., "ETag": ..., "Last-Modified": ...}
resp.useSendFile_ = true
resp.sendFilePath_ = "/var/www/static/index.html"
```

#### 第 3 步：状态机视图

```
                请求到达 req={GET, "/static/index.html"}
                        │
                        ▼
              ┌─────────────────────┐
              │  routes_.find(key)  │
              └──────────┬──────────┘
                  命中? │
            ┌─────yes───┴────no─────┐
            ▼                       ▼
      handler(req,resp)     ┌──────────────────────┐
                            │ for prefixRoutes_:   │
                            │  method == ? &&      │
                            │  url.compare(...)==0?│
                            └──────────┬───────────┘
                                  命中? │
                          ┌─────yes────┴────no────┐
                          ▼                       ▼
                    handler(req,resp)    httpCallback_(req,resp)
                                              (默认 404)
```

#### 第 4 步：函数职责一句话表

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `addRoute` | 启动前用户调用 | 把 `(method,path,handler)` 写入 unordered_map |
| `addPrefixRoute` | 启动前用户调用 | append 到 prefixRoutes_ 末尾，按注册顺序匹配 |
| `makeRouteKey` | addRoute / dispatch | 把 `method+path` 拼成可哈希字符串 |
| `dispatch` (lambda) | runChain 末尾 | 三级 fallback：精确 → 前缀 → 默认 |

---

## 4. 第 3 步 — 写中间件链（洋葱模型）

### 4.1 问题背景

每个路由处理函数都需要做 4 件事：

1. 打日志：记录 `method url -> status_code`
2. 校验 Authorization header（受保护路由）
3. 注入 CORS 响应头
4. 业务处理

如果不抽象，每个 handler 头部要 30 行 boilerplate。中间件链允许这样写：

```cpp
srv.use(loggingMiddleware);     // 跨所有路由都打日志
srv.use(authMiddleware);        // 受保护路由检查 token
srv.use(corsMiddleware);        // 注入 CORS 头
srv.addRoute(GET, "/api/users", handleUsers);  // handler 只关心业务
```

每个 middleware 拿到 `(req, resp, next)`，调用 `next()` 把控制权交给下一个；不调用 `next()` 则中断后续（典型如鉴权失败时）。

### 4.2 打开 HttpServer.h：定义中间件类型

来自 [HISTORY/day29/include/http/HttpServer.h](HISTORY/day29/include/http/HttpServer.h)（行 47–50、64–66、103）：

```cpp
using HttpCallback = std::function<void(const HttpRequest &, HttpResponse *)>;
using RouteHandler = HttpCallback;
using MiddlewareNext = std::function<void()>;
using Middleware =
    std::function<void(const HttpRequest &, HttpResponse *, const MiddlewareNext &)>;
```

```cpp
void use(Middleware middleware) { middlewares_.emplace_back(std::move(middleware)); }
```

```cpp
std::vector<Middleware> middlewares_;
```

### 4.3 写实现：递归闭包 runChain

来自 [HISTORY/day29/common/http/HttpServer.cpp](HISTORY/day29/common/http/HttpServer.cpp)（行 194–204）：

```cpp
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
```

**关键点**：
- `runChain` 是一个 `std::function`，捕获了自身（通过 `[&]` 引用捕获）。
- 每个 middleware 收到的 `next` 参数其实就是 `runChain` —— 它内部决定何时调用、是否调用。
- `index` 是 capture-by-reference 的局部变量，在递归过程中累加。

### 4.4 中间件常见模式

**前置型**（鉴权 / 日志）：

```cpp
auto auth = [](const HttpRequest& req, HttpResponse* resp,
               const HttpServer::MiddlewareNext& next) {
    if (req.header("Authorization").empty()) {
        resp->setStatus(HttpResponse::StatusCode::k401Unauthorized, "Unauthorized");
        return;   // 不调 next() → 链路中断
    }
    next();
};
```

**后置型**（响应压缩）：

```cpp
auto gzip = [](const HttpRequest& req, HttpResponse* resp,
               const HttpServer::MiddlewareNext& next) {
    next();    // 先让后续 middleware + dispatch 跑完
    if (shouldCompress(req, resp)) {
        compressInPlace(resp);
    }
};
```

**环绕型**（计时）：

```cpp
auto timing = [](const HttpRequest& req, HttpResponse* resp,
                 const HttpServer::MiddlewareNext& next) {
    auto start = TimeStamp::now();
    next();
    auto elapsed = TimeStamp::now().microseconds() - start.microseconds();
    LOG_INFO << req.url() << " took " << elapsed << "us";
};
```

### 4.5 验证：追踪三个中间件的执行

#### 业务场景 + 时序总览表

注册顺序：`[loggingMW, authMW, corsMW]`，请求 `GET /api/secret`，无 Authorization 头：

| 步 | 当前 index | 进入函数 | 函数动作 | 副作用 |
|----|-----------|---------|---------|--------|
| 1 | 0 | runChain | `index<3`, 取 mw[0]=logging, `++index → 1` | logging(req,resp,runChain) |
| 2 | 1 | logging | 打日志 `LOG_INFO << ...`，调 next() = runChain | runChain 重入 |
| 3 | 1 | runChain | `index<3`, 取 mw[1]=auth, `++index → 2` | auth(req,resp,runChain) |
| 4 | 2 | auth | 检查 Authorization 为空 → setStatus 401 → **不调 next** | 链路中断 |
| 5 | 2 | （runChain 不再被调用） | corsMW、dispatch 都被跳过 | resp.statusCode=401 |
| 6 | — | onRequest 末尾 | `conn->send(resp.serialize())` → 401 响应写出 | 返回 401 |

#### 第 1 步：runChain 第一次触发

代码：见 §4.3。

**代入实参**：
- `index = 0`
- `middlewares_.size() = 3`
- 条件 `0 < 3` true → 进入 if 体
- 取 `mw = middlewares_[0]` (logging)
- `++index → 1`
- 调用 `logging(req, &resp, runChain)`

#### 第 2 步：logging 调用 next（即 runChain）

logging 内部最后一行 `next();`，本质是再次调用 runChain。
此时 `index=1`，再次进入 if 体，取 mw[1]=auth，`++index → 2`，调用 auth。

#### 第 3 步：auth 不调 next

auth 检查 `req.header("Authorization")` 返回空字符串：

```cpp
if (req.header("Authorization").empty()) {
    resp->setStatus(HttpResponse::StatusCode::k401Unauthorized, "Unauthorized");
    return;       // ← 不调 next() ！
}
```

**关键不变式**：**链路中断不需要任何额外通知机制**。runChain 是普通的 `std::function`，没人调它就不会执行，自然 `corsMW` 和 `dispatch` 都跳过。

**副作用快照**（步骤 4 之后）：

```
index            = 2
resp.statusCode  = 401
resp.headers     = {}（除非 auth 顺手注入）
runChain         = 不再被调用
```

#### 第 4 步：状态机视图（洋葱模型）

```
                请求 req
                  │
                  ▼
        ┌───────────────────┐
        │  loggingMW (in)   │  打日志，调 next()
        └────────┬──────────┘
                 ▼
        ┌───────────────────┐
        │  authMW    (in)   │  Authorization 空 → setStatus 401
        └────────┬──────────┘   不调 next() → 中断
                 ✗
              ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄  （以下被跳过）
        ┌───────────────────┐
        │  corsMW    (in)   │
        └────────┬──────────┘
                 ▼
        ┌───────────────────┐
        │   dispatch        │
        └────────┬──────────┘
                 │
                 ▼
              业务 handler

    runChain 内 index 推进的方向 → 形成栈式调用
```

#### 第 5 步：函数职责一句话表

| 角色 | 调用时机 | 职责 |
|------|---------|------|
| `Middleware` 类型别名 | use() / 用户定义 | 三参签名 `(req, resp, next)` |
| `runChain` lambda | onRequest 顶层启动 | 递归推进 index，触发下一个 middleware 或 dispatch |
| `index` 局部变量 | runChain 闭包捕获 | 当前已"进入"了几个 middleware |
| 不调 `next()` | middleware 内部决定 | 主动中断链路，跳过后续 |

---

## 5. 第 4 步 — 给 Connection 加 sendFile 零拷贝

### 5.1 问题背景

CDN 节点提供 100 MB 安装包下载。Day 28 之前的实现：

```cpp
std::ifstream in(path);
std::ostringstream oss;
oss << in.rdbuf();
conn->send(oss.str());     // 整个文件读入字符串再交给 send()
```

性能损耗：
1. `read(file_fd, kbuf, ...)` —— 文件页 → 内核 page cache（DMA）
2. `kbuf → user_buf` —— page cache → 用户态字符串（**第一次 memcpy**）
3. `conn->send(user_buf)` —— 用户态字符串拷贝进 outputBuffer_（**第二次 memcpy**）
4. `write(sock_fd, outputBuffer_)` —— outputBuffer → 内核 socket buf（**第三次 memcpy**）

100 MB × 3 次 memcpy = 300 MB 内存带宽。`wrk -t4 -c200` 跑这个 endpoint，CPU 100% 用在 memcpy 上。

`sendfile()` 系统调用直接在内核态把文件 page cache 内容拷贝到 socket buffer，跳过两次用户态拷贝：

| 方式 | 内存带宽消耗 | 用户态参与 |
|------|-------------|-----------|
| read+write | 300 MB | 是 |
| sendfile | 100 MB（甚至有些内核可用 DMA + splice 进一步降到 0） | 否 |

### 5.2 打开 Connection.h：声明 sendFile

来自 [HISTORY/day29/include/Connection.h](HISTORY/day29/include/Connection.h)（行 75）：

```cpp
// 文件正文发送快路径：优先尝试 sendfile，不可用时降级为分块读取后发送。
bool sendFile(const std::string &path, size_t offset, size_t count);
```

返回 true 表示成功（不论走 sendfile 还是降级），false 表示文件打开失败 / 连接已关闭。

### 5.3 写实现：sendfile 优先 + 降级

来自 [HISTORY/day29/common/Connection.cpp](HISTORY/day29/common/Connection.cpp)（节选 sendFile 实现，sendfile 系统调用分支）：

```cpp
bool Connection::sendFile(const std::string &path, size_t offset, size_t count) {
    if (state_ != State::kConnected) return false;

    int fileFd = ::open(path.c_str(), O_RDONLY);
    if (fileFd < 0) {
        LOG_ERROR << "[连接] sendFile: 无法打开 " << path << " errno=" << errno;
        return false;
    }

#ifdef __linux__
    off_t off = static_cast<off_t>(offset);
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = ::sendfile(sock_->getFd(), fileFd, &off, remaining);
        if (n > 0) {
            remaining -= n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // socket 写满，把剩余部分降级为读 + outputBuffer 异步发
            break;
        } else {
            LOG_ERROR << "[连接] sendfile 失败 errno=" << errno;
            ::close(fileFd);
            return false;
        }
    }
    if (remaining == 0) {
        ::close(fileFd);
        return true;
    }
    // 落入降级分支
#endif
    // 降级：read + send
    char buf[64 * 1024];
    while (count > 0) {
        ssize_t n = ::pread(fileFd, buf, std::min(count, sizeof(buf)), offset);
        if (n <= 0) break;
        send(std::string(buf, buf + n));
        offset += n;
        count -= n;
    }
    ::close(fileFd);
    return true;
}
```

**降级时机**：
- macOS 没有 Linux 风格 `sendfile`（macOS 的 `sendfile()` 签名不同），只走降级路径。
- Linux 上 `sendfile()` 返回 EAGAIN（socket 已满），剩余数据走 read+send。

### 5.4 嵌入路径 — `HttpServer::onRequest` 末尾

来自 [HISTORY/day29/common/http/HttpServer.cpp](HISTORY/day29/common/http/HttpServer.cpp)（行 211–217）：

```cpp
conn->send(resp.serialize());

if (resp.hasSendFile()) {
    conn->sendFile(resp.sendFilePath(), resp.sendFileOffset(), resp.sendFileCount());
}
```

`resp.serialize()` 只产出 HTTP 响应头（`HTTP/1.1 200 OK\r\nContent-Length: 100000000\r\n...\r\n`），不含正文。
紧随其后调用 `sendFile()` 把正文以零拷贝方式接到响应头之后。

### 5.5 验证：追踪 100 MB 文件下载

#### 业务场景 + 时序总览表

| 时刻 | 事件 | sendfile 字节数 | outputBuffer_ | sock_fd 状态 |
|------|------|----------------|---------------|--------------|
| T0 | resp.serialize() → conn->send(headers) | — | 250 字节（HTTP 头） | 可写 |
| T1 | sendFile("/big.bin", 0, 100MB) → ::sendfile() | 4 MB（一次系统调用上限） | 250 字节 | 可写 |
| T2 | 第二次 ::sendfile() | +4 MB（累计 8 MB） | 250 字节 | 可写 |
| T3 | 第 N 次 ::sendfile() 返回 EAGAIN | 累计 64 MB | 250 字节 | **socket buffer 满** |
| T4 | 降级分支 read 4 KB + send() → outputBuffer_ | — | 250 + 4 KB | 可写但 outputBuffer 在涨 |
| T5 | EventLoop 下一轮 doWrite 把 outputBuffer 排空 | — | 0 | 可写 |
| T6 | 全部 100 MB 发送完成，关闭 fileFd | — | 0 | 可写 |

#### 第 1 步：进入 ::sendfile 循环

代码：

```cpp
off_t off = static_cast<off_t>(offset);     // off = 0
size_t remaining = count;                   // remaining = 100 * 1024 * 1024
while (remaining > 0) {
    ssize_t n = ::sendfile(sock_->getFd(), fileFd, &off, remaining);
    ...
}
```

**代入实参**（T1）：
- `sock_->getFd() = 42`
- `fileFd = 87`
- `off = 0`
- `remaining = 104857600`

`sendfile` 系统调用语义：从 `fileFd` 在偏移 `*off` 处开始，最多写 `remaining` 字节到 `sock_fd`，返回实际写出字节数；同时把 `*off` 推进。

**代入实参**（T1 返回后）：
- `n = 4194304`（4 MB）
- `off → 4194304`
- `remaining → 100663296`

#### 第 2 步：socket 满，跳出 while

```cpp
} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    break;
}
```

**代入实参**（T3）：
- `n = -1`、`errno = EAGAIN`
- 条件 true → break

跳出 while 后：
- `remaining = 100663296 - 64 MB = 39 MB`
- 进入降级路径

#### 第 3 步：降级 read+send

```cpp
char buf[64 * 1024];
while (count > 0) {
    ssize_t n = ::pread(fileFd, buf, std::min(count, sizeof(buf)), offset);
    if (n <= 0) break;
    send(std::string(buf, buf + n));
    offset += n;
    count -= n;
}
```

**注意**：`offset` 和 `count` 此时已被 sendfile 路径**修改过**——但实际上代码用的是函数参数 `offset/count`，没有被 sendfile 路径同步推进（仅修改了局部 `off/remaining`）。这是当前实现的瑕疵：在 sendfile 部分成功 + EAGAIN 的混合情况下，降级路径会从原始 offset 开始重发，会产生重复字节。

> 改进点（已记入 §13）：把 `offset/count` 在 sendfile 循环结束后用 `off/remaining` 同步回去，再进入降级路径。当前 demo 场景因为客户端通常不会在 64 MB 时把 socket 缓冲打满，不易触发，但是真正的生产代码必须修正。

#### 第 4 步：状态机视图

```
                sendFile(path, off, count)
                          │
                          ▼
                ┌─────────────────────┐
                │   open(path)        │
                └──────────┬──────────┘
                  失败 │     │ 成功
                       ▼     ▼
                  return false  ┌──────────┐
                                │ Linux?   │
                                └────┬─────┘
                              否 │   │ 是
                                ▼   ▼
                            ┌────┐  ┌─────────────────┐
                            │降级│  │ ::sendfile 循环 │
                            │read│  └────┬────────────┘
                            │send│       │
                            └─┬──┘  ┌────┴───────────┐
                              │     ▼                ▼
                              │   全部写完         EAGAIN
                              │   close+true     break → 降级
                              ▼                       │
                           完成 + true ◄───────────────┘
```

#### 第 5 步：函数职责一句话表

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `Connection::sendFile` | HttpServer::onRequest 末尾 / 用户主动调用 | 文件正文零拷贝 / 降级发送 |
| `::sendfile` (Linux) | sendFile 内的 while 循环 | 内核态文件→socket 拷贝 |
| `::pread` + `send()` | 降级 / 非 Linux 平台 | 用户态分块发送 |
| `HttpResponse::setSendFile` | StaticFileHandler::serve 等 handler 内 | 仅记录 path/offset/count，不真正写出 |
| `HttpResponse::hasSendFile` | onRequest 末尾 | 判断是否需要触发零拷贝 |

---

## 6. 第 5 步 — 写 CorsMiddleware

### 6.1 问题背景

前端 SPA 跑在 `http://localhost:3000`（webpack-dev-server），后端跑在 `http://localhost:8888`。
浏览器发现 origin 不同，先发 `OPTIONS /api/users` 预检：

```http
OPTIONS /api/users HTTP/1.1
Origin: http://localhost:3000
Access-Control-Request-Method: POST
Access-Control-Request-Headers: Content-Type, Authorization
```

服务器必须响应 204 + 三个 `Access-Control-*` 响应头，浏览器才会放行后续真实请求。
不响应 → 控制台报错 `No 'Access-Control-Allow-Origin' header is present`，整个 SPA 不能调后端 API。

### 6.2 接口与实现

来自 [HISTORY/day29/include/http/CorsMiddleware.h](HISTORY/day29/include/http/CorsMiddleware.h)（行 24–98）：

```cpp
class CorsMiddleware {
  public:
    CorsMiddleware() = default;

    CorsMiddleware &allowOrigin(const std::string &origin) {
        allowOrigin_ = origin;
        return *this;
    }

    CorsMiddleware &allowMethods(const std::vector<std::string> &methods) {
        allowMethods_.clear();
        for (size_t i = 0; i < methods.size(); ++i) {
            if (i > 0) allowMethods_ += ", ";
            allowMethods_ += methods[i];
        }
        return *this;
    }

    CorsMiddleware &allowHeaders(const std::vector<std::string> &headers) {
        allowHeaders_.clear();
        for (size_t i = 0; i < headers.size(); ++i) {
            if (i > 0) allowHeaders_ += ", ";
            allowHeaders_ += headers[i];
        }
        return *this;
    }

    CorsMiddleware &maxAge(int seconds) {
        maxAge_ = std::to_string(seconds);
        return *this;
    }

    CorsMiddleware &allowCredentials(bool allow) {
        allowCredentials_ = allow;
        return *this;
    }

    HttpServer::Middleware toMiddleware() {
        return [this](const HttpRequest &req, HttpResponse *resp,
                      const HttpServer::MiddlewareNext &next) {
            resp->addHeader("Access-Control-Allow-Origin", allowOrigin_);
            if (allowCredentials_) {
                resp->addHeader("Access-Control-Allow-Credentials", "true");
            }

            if (req.method() == HttpRequest::Method::kOptions) {
                resp->addHeader("Access-Control-Allow-Methods", allowMethods_);
                resp->addHeader("Access-Control-Allow-Headers", allowHeaders_);
                resp->addHeader("Access-Control-Max-Age", maxAge_);
                resp->setStatus(HttpResponse::StatusCode::k204NoContent, "No Content");
                resp->setBody("");
                return; // 不调用 next()，中断后续中间件和路由
            }

            next();
        };
    }

  private:
    std::string allowOrigin_{"*"};
    std::string allowMethods_{"GET, POST, OPTIONS"};
    std::string allowHeaders_{"Content-Type, Authorization"};
    std::string maxAge_{"86400"};
    bool allowCredentials_{false};
};
```

### 6.3 验证：追踪 Chrome 预检请求

#### 业务场景 + 时序总览表

`server.use(CorsMiddleware().allowOrigin("http://localhost:3000").allowMethods({"GET","POST","OPTIONS"}).toMiddleware())`，前端发 `OPTIONS /api/users`：

| 时刻 | 事件 | resp 状态 | 链路状态 |
|------|------|----------|---------|
| T0 | runChain → cors middleware | resp 空 | index=0→1 |
| T1 | 注入 `Access-Control-Allow-Origin` | resp.headers 中有 1 个 | — |
| T2 | 检测 `req.method() == kOptions` | true | 进入预检分支 |
| T3 | 注入 Methods / Headers / MaxAge | resp.headers 中有 4 个 | — |
| T4 | `setStatus(204)`, `setBody("")` | resp.statusCode=204 | — |
| T5 | **不调 next()** | — | runChain 不再推进 → dispatch 跳过 |
| T6 | onRequest 末尾 conn->send(resp.serialize()) | — | 写出 204 + 4 个 CORS 头 |

#### 第 1 步：CORS 中间件入口

代码：见 §6.2，lambda 的第一段：

```cpp
resp->addHeader("Access-Control-Allow-Origin", allowOrigin_);
if (allowCredentials_) {
    resp->addHeader("Access-Control-Allow-Credentials", "true");
}
```

**代入实参**：
- `allowOrigin_ = "http://localhost:3000"`
- `allowCredentials_ = false`

**副作用快照**：

```
resp.headers = {"Access-Control-Allow-Origin": "http://localhost:3000"}
resp.statusCode = 200 (默认)
```

#### 第 2 步：检测 OPTIONS 预检

```cpp
if (req.method() == HttpRequest::Method::kOptions) {
    resp->addHeader("Access-Control-Allow-Methods", allowMethods_);
    resp->addHeader("Access-Control-Allow-Headers", allowHeaders_);
    resp->addHeader("Access-Control-Max-Age", maxAge_);
    resp->setStatus(HttpResponse::StatusCode::k204NoContent, "No Content");
    resp->setBody("");
    return;
}
```

**代入实参**：
- `req.method() = kOptions` → 条件 true
- `allowMethods_ = "GET, POST, OPTIONS"`
- `allowHeaders_ = "Content-Type, Authorization"`
- `maxAge_ = "86400"`

**副作用快照**：

```
resp.statusCode    = 204
resp.headers       = {
    "Access-Control-Allow-Origin": "http://localhost:3000",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, Authorization",
    "Access-Control-Max-Age": "86400"
}
resp.body          = ""
runChain           = 不再被调用
```

#### 第 3 步：状态机视图

```
                请求 req
                  │
                  ▼
            corsMW (in)
                  │
        ┌─────────┴──────────┐
        ▼                    ▼
   非 OPTIONS              OPTIONS
   注入 Origin              注入全套 CORS 头
   调 next()               setStatus(204)
        │                  return（不调 next）
        ▼                    │
   下一个 middleware         ▼
   或 dispatch             写出 204
                          浏览器放行真实请求
```

#### 第 4 步：函数职责一句话表

| 角色 | 调用时机 | 职责 |
|------|---------|------|
| `CorsMiddleware::allowOrigin` 等链式调用 | 启动前配置 | 设置允许的源/方法/头/时长 |
| `CorsMiddleware::toMiddleware()` | 启动前 | 把配置封装成可注册的 lambda |
| 注入 `Access-Control-*` 响应头 | 每次请求 | 让浏览器认为本响应允许跨域 |
| 检测 OPTIONS + 不调 next | 仅预检 | 截断链路，不让 dispatch 找到 OPTIONS 路由 |

---

## 7. 第 6 步 — 写 GzipMiddleware

### 7.1 问题背景

`/api/products` 返回 5 KB 的 JSON，模拟 1000 个产品的列表。
不压缩：每次响应 5 KB × 1000 用户/天 = 5 MB / 天。
gzip 后通常压缩到 1.5 KB 以下（JSON 可压缩性高），节省 70% 带宽。
对移动端 4G 用户，这意味着 200ms 的响应时间差异。

### 7.2 写实现：后置型压缩中间件

来自 [HISTORY/day29/include/http/GzipMiddleware.h](HISTORY/day29/include/http/GzipMiddleware.h)（节选关键段）：

```cpp
HttpServer::Middleware toMiddleware() {
    return [this](const HttpRequest &req, HttpResponse *resp,
                  const HttpServer::MiddlewareNext &next) {
        next();  // ← 先让后续中间件 + dispatch 跑完，产生响应

        if (resp->hasSendFile())
            return;   // sendFile 路径不走内存压缩

        std::string acceptEncoding = req.header("Accept-Encoding");
        if (acceptEncoding.find("gzip") == std::string::npos)
            return;   // 客户端不接受 gzip

        std::string contentType = resp->header("Content-Type");
        if (!isCompressible(contentType))
            return;   // 不可压缩类型（如 image/png）

        const std::string &body = resp->body();
        if (body.size() < minSize_)
            return;   // 小报文压缩反而更费

        if (!resp->header("Content-Encoding").empty())
            return;   // 已经压缩

#ifdef HAS_ZLIB
        std::string compressed;
        if (!compress(body, &compressed)) return;
        if (compressed.size() >= body.size()) return;  // 压完反而大

        resp->setBody(std::move(compressed));
        resp->addHeader("Content-Encoding", "gzip");
        resp->addHeader("Vary", "Accept-Encoding");
#endif
    };
}
```

### 7.3 5 个跳过条件 = 健壮性

每一个 `return` 都是一道安全阀：

1. `hasSendFile` → 文件不在内存里，没法在线压缩
2. `Accept-Encoding` 无 gzip → 客户端要求不压
3. `Content-Type` 不可压（image/png 已经是压缩格式）→ 压了浪费 CPU
4. `body.size() < minSize_` → 小数据压完元数据开销大于收益
5. 已有 `Content-Encoding` → 链路上已经有别人压过

### 7.4 验证：追踪 5 KB JSON 响应压缩

#### 业务场景 + 时序总览表

注册：`[loggingMW, gzipMW]`，请求 `GET /api/products`，`Accept-Encoding: gzip`：

| 时刻 | 步 | 角色 | 副作用 |
|------|----|------|-------|
| T0 | 1 | runChain 启动 | index=0→1, 调 logging |
| T1 | 2 | logging | 调 next() → runChain 重入 |
| T2 | 3 | runChain 第二轮 | index=1→2, 调 gzipMW |
| T3 | 4 | gzipMW 入口 | 立刻调 next() → runChain 重入 |
| T4 | 5 | runChain 第三轮 | `index ≥ 2` → 调 dispatch |
| T5 | 6 | dispatch → handler | 写 JSON 5 KB body, content-type application/json |
| T6 | 7 | dispatch 返回到 runChain → 返回到 gzipMW 的 next() 之后 | 进入压缩条件检查 |
| T7 | 8 | 5 个条件全通过 | compress(body) → compressed (1.4 KB) |
| T8 | 9 | setBody(compressed) + addHeader Content-Encoding/Vary | resp.body=1.4KB, headers+=2 |
| T9 | 10 | onRequest → conn->send(resp.serialize()) | 写出 1.4 KB body 而非 5 KB |

#### 第 1 步：next() 之后立刻进入压缩判断

代码：见 §7.2 lambda 后半。

**代入实参**（dispatch 跑完后）：
- `resp.body() = "[{...},{...},...]"`，size = 5120
- `resp.hasSendFile() = false`
- `req.header("Accept-Encoding") = "gzip, deflate"`
- `resp.header("Content-Type") = "application/json"`
- `resp.header("Content-Encoding") = ""`
- `minSize_ = 256`

**5 个条件依次判断**：
- `hasSendFile()` = false → 跳过 return
- `find("gzip") != npos` = true → 跳过 return
- `isCompressible("application/json")` = true → 跳过 return
- `5120 < 256` = false → 跳过 return
- `Content-Encoding` 空 → 跳过 return
- 进入 compress

#### 第 2 步：调用 zlib 压缩

zlib 的 `deflateInit2(..., 15+16, ...)` 中 `windowBits = 15+16` 表示 gzip 格式（带 gzip header）而非 raw deflate。
压缩级别默认 `Z_DEFAULT_COMPRESSION (-1)` = 6。

#### 第 3 步：检查"压完更小"

```cpp
if (compressed.size() >= body.size()) return;
```

**代入**：`compressed.size() = 1430`、`body.size() = 5120` → `1430 >= 5120` 为假 → 不 return，继续。

#### 第 4 步：替换 body + 添加头

```cpp
resp->setBody(std::move(compressed));
resp->addHeader("Content-Encoding", "gzip");
resp->addHeader("Vary", "Accept-Encoding");
```

**副作用快照**：

```
resp.body           = <1430 字节 gzip 流>
resp.headers["Content-Encoding"] = "gzip"
resp.headers["Vary"]              = "Accept-Encoding"
resp.statusCode                    = 200
```

`Vary: Accept-Encoding` 告诉缓存代理：响应内容因 Accept-Encoding 而异，不能把"未压缩响应"和"压缩响应"混在同一缓存条目下。

#### 第 5 步：函数职责一句话表

| 角色 | 调用时机 | 职责 |
|------|---------|------|
| `GzipMiddleware::toMiddleware` | 启动前 | 返回后置型 lambda |
| 5 个 return 守护 | next() 之后 | 一票否决，确保不做无效压缩 |
| `compress()` | 全部通过守护后 | zlib deflate 实际压缩 |
| `Vary: Accept-Encoding` | 替换 body 后 | 告知下游缓存层有变体响应 |

---

## 8. 第 7 步 — 写 StaticFileHandler

### 8.1 问题背景

三个真实痛点：

**A. 浏览器全量回拉**：用户刷新页面，浏览器重新请求 `index.css` (1.5 MB)。
后端不发 ETag → 浏览器没法做 If-None-Match → 服务器再次返回 200 + 1.5 MB body。
理想：发 304 Not Modified + 0 字节 body。

**B. 视频拖动播放**：用户在 video 标签中拖到 50% 位置。
浏览器发 `Range: bytes=52428800-` 请求只要后半段。
后端不支持 Range → 整个视频重新下载 → 体验崩盘。

**C. 安装包浏览器误识别为文本**：`/downloads/setup.exe` 被服务器返回 `Content-Type: text/plain`，浏览器在新窗口打开二进制流而不是触发下载。

### 8.2 打开 StaticFileHandler.h：定义接口

来自 [HISTORY/day29/include/http/StaticFileHandler.h](HISTORY/day29/include/http/StaticFileHandler.h)（行 38–48）：

```cpp
struct Options {
    std::string downloadName; // 非空时触发 Content-Disposition: attachment
    std::string cacheControl; // Cache-Control 头
    bool enableRange;         // 是否支持 Range 请求
    Options() : cacheControl("public, max-age=3600"), enableRange(true) {}
};

static bool serve(const HttpRequest &req, HttpResponse *resp, const std::string &path,
                  const Options &opts = Options());
```

### 8.3 写实现：集中处理 6 类语义

来自 [HISTORY/day29/include/http/StaticFileHandler.h](HISTORY/day29/include/http/StaticFileHandler.h)（行 49–119，serve 主体）：

```cpp
static bool serve(const HttpRequest &req, HttpResponse *resp, const std::string &path,
                  const Options &opts = Options()) {
    if (path.find("..") != std::string::npos) {
        resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
        resp->setBody("Invalid path\n");
        return true;
    }

    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;

    const size_t fileSize = static_cast<size_t>(st.st_size);

    const std::string etag = buildWeakEtag(st);
    const std::string lastModified = toHttpDate(st.st_mtime);

    resp->addHeader("ETag", etag);
    resp->addHeader("Last-Modified", lastModified);
    resp->addHeader("Cache-Control", opts.cacheControl);
    resp->addHeader("Accept-Ranges", "bytes");

    if (!opts.downloadName.empty()) {
        resp->addHeader("Content-Disposition",
                        "attachment; filename=\"" + opts.downloadName + "\"");
        resp->setContentTypeByFilename(opts.downloadName);
    } else {
        size_t slash = path.find_last_of('/');
        resp->setContentTypeByFilename(slash == std::string::npos ? path
                                                                  : path.substr(slash + 1));
    }

    if (req.header("If-None-Match") == etag) {
        resp->setStatus(HttpResponse::StatusCode::k304NotModified, "Not Modified");
        resp->setBody("");
        return true;
    }

    size_t start = 0;
    size_t length = fileSize;
    const std::string rangeHeader = req.header("Range");

    if (opts.enableRange && !rangeHeader.empty()) {
        if (!parseRange(rangeHeader, fileSize, start, length)) {
            resp->setStatus(HttpResponse::StatusCode::k416RangeNotSatisfiable,
                            "Range Not Satisfiable");
            resp->addHeader("Content-Range", "bytes */" + std::to_string(fileSize));
            resp->setBody("\n");
            return true;
        }
        resp->setStatus(HttpResponse::StatusCode::k206PartialContent, "Partial Content");
        resp->addHeader("Content-Range", "bytes " + std::to_string(start) + "-" +
                                             std::to_string(start + length - 1) + "/" +
                                             std::to_string(fileSize));
    } else {
        resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    }

    resp->setSendFile(path, start, length);
    return true;
}
```

### 8.4 弱 ETag 公式

```cpp
static std::string buildWeakEtag(const struct stat &st) {
    return "W/\"" + std::to_string(static_cast<unsigned long long>(st.st_size)) + "-" +
           std::to_string(static_cast<unsigned long long>(st.st_mtime)) + "\"";
}
```

弱 ETag 仅承诺"语义等价"——文件 size+mtime 都没变就视作内容未变。
强 ETag 需要算 SHA1/MD5，CPU 成本高。对静态资源而言弱 ETag 已经足够。

### 8.5 验证：追踪三种请求场景

#### 业务场景 + 时序总览表

| 场景 | 请求头特征 | 服务器决策 | 响应状态 | body |
|------|-----------|-----------|---------|------|
| (a) 首次访问 | 无 If-None-Match、无 Range | 200 + ETag + sendFile 全文 | 200 | 全部字节 |
| (b) 浏览器回访 | If-None-Match: W/"1500000-1700000000" | 比对 ETag 相等 → 304 | 304 | 空 |
| (c) 视频拖动 | Range: bytes=52428800-104857599 | parseRange 成功 → 206 + Content-Range | 206 | 中段字节 |
| (d) 非法 Range | Range: bytes=999999999-9999999999 | parseRange 失败 → 416 | 416 | "\\n" |

#### 第 1 步：首次访问（场景 a）

**代入实参**：
- `path = "/var/www/index.css"`
- `stat(...)` 成功 → `st.st_size = 1500000`、`st.st_mtime = 1700000000`
- `req.header("If-None-Match") = ""`（首次访问）
- `req.header("Range") = ""`

**分支走向**：
- `..` 检查 → 通过
- `stat` → 通过
- `etag = "W/\"1500000-1700000000\""`
- 写入 ETag、Last-Modified、Cache-Control、Accept-Ranges 4 个头
- `If-None-Match == etag`？`""==…` → false → 跳过 304
- `enableRange && rangeHeader.empty()` → 跳过 Range 分支
- `setStatus(200, "OK")`
- `setSendFile(path, 0, 1500000)`

**副作用快照**：

```
resp.statusCode = 200
resp.headers    = {ETag, Last-Modified, Cache-Control, Accept-Ranges, Content-Type}
resp.useSendFile_ = true, sendFilePath_=path, offset=0, count=1500000
resp.body         = ""（body 不在内存里，由 sendFile 路径负责）
```

#### 第 2 步：浏览器回访（场景 b）

**代入实参**：
- `req.header("If-None-Match") = "W/\"1500000-1700000000\""`

**分支走向**：
- 一路同 (a) 直到 `If-None-Match == etag` → true
- `setStatus(304, "Not Modified")`
- `setBody("")`
- `return true`

**副作用快照**：

```
resp.statusCode = 304
resp.headers    = {ETag, Last-Modified, Cache-Control, Accept-Ranges, Content-Type}
resp.body       = ""
resp.useSendFile_ = false（因为是从 If-None-Match 早 return，未到 setSendFile 那一行）
```

→ 浏览器看到 304 + 空 body 直接复用本地缓存，0 流量。

#### 第 3 步：视频拖动（场景 c）

**代入实参**：
- `req.header("Range") = "bytes=52428800-104857599"`
- `enableRange = true`

**分支走向**：
- `parseRange("bytes=52428800-104857599", fileSize=…, start, length)` → true，设置 `start=52428800, length=52428800`
- `setStatus(206, "Partial Content")`
- `addHeader("Content-Range", "bytes 52428800-104857599/<fileSize>")`
- `setSendFile(path, 52428800, 52428800)` → 只发中间 50 MB

#### 第 4 步：状态机视图

```
                 serve(req, resp, path)
                          │
              path 含 ".."?─yes─► 400 Bad Request
                          │ no
                  stat 失败? ─yes─► return false
                          │ no
                设 ETag/LastMod/Cache-Control/Accept-Ranges
                设 Content-Type（按文件名/downloadName）
                          │
            If-None-Match ─yes─► 304 Not Modified  (return)
              == ETag?       │
                          │ no
                  Range 头存在 + enableRange?
                          │
                ┌─────yes──┴───no──┐
                ▼                  ▼
        parseRange 成功?         200 OK
              │                  setSendFile(path, 0, fileSize)
        ┌─yes─┴─no─┐
        ▼          ▼
       206       416 Range Not Satisfiable
       Content-Range
       setSendFile(path, start, length)
```

#### 第 5 步：函数职责一句话表

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `StaticFileHandler::serve` | 用户路由 handler 内 | 一站式：stat / ETag / 304 / Range / sendFile 描述 |
| `buildWeakEtag` | serve 内 | size-mtime 弱 ETag 字符串 |
| `parseRange` | enableRange 且有 Range 头时 | 解析 `bytes=start-end`；失败返回 416 |
| `setSendFile` | serve 末尾 | 只设描述，不真正读文件 |
| `Connection::sendFile` | onRequest 末尾 | 实际把文件正文送出（零拷贝 / 降级） |

---

## 9. 第 8 步 — 加请求超时保护（408）

### 9.1 问题背景

经典 Slowloris 攻击：脚本打开 1024 个 TCP 连接，每个连接以 "1 字节/秒" 速率发请求行。
Day 21 的空闲超时（60 秒无数据）每秒钟都被触发"有数据交互"而无效。
后果：服务器 fd 耗尽，正常用户被拒绝接入。

请求级超时 = "从首次收到该请求的字节起，必须在 N 秒内完成解析"。
对正常用户，HTTP 请求往往在 1ms 内完成接收；对 Slowloris，15 秒之内远不够发完一行请求行。

### 9.2 打开 HttpServer.h：定义超时配置

来自 [HISTORY/day29/include/http/HttpServer.h](HISTORY/day29/include/http/HttpServer.h)（行 39–43）：

```cpp
struct Options {
    TcpServer::Options tcp;
    bool autoClose{false};
    double idleTimeoutSec{60.0};
    double requestTimeoutSec{15.0};
    HttpContext::Limits limits{};
};
```

`idleTimeoutSec` = 连接级（无任何数据 60s）；`requestTimeoutSec` = 请求级（解析单个请求最长 15s）。两者互补。

### 9.3 写实现：超时检查嵌入 onMessage

来自 [HISTORY/day29/common/http/HttpServer.cpp](HISTORY/day29/common/http/HttpServer.cpp)（行 117–132）：

```cpp
while (buf->readableBytes() > 0) {
    if (!ctx->requestInProgress) {
        ctx->requestInProgress = true;
        ctx->requestStart = TimeStamp::now();
    }

    const double elapsedSec = static_cast<double>(TimeStamp::now().microseconds() -
                                                  ctx->requestStart.microseconds()) /
                              kMicrosecondsToSeconds;
    if (elapsedSec > requestTimeoutSec_) {
        LOG_WARN << "[HttpServer] 请求解析超时，fd=" << conn->getSocket()->getFd()
                 << " elapsedSec=" << elapsedSec;
        conn->send(
            "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        conn->close();
        return;
    }
    ...
}
```

### 9.4 验证：追踪 Slowloris 防护

#### 业务场景 + 时序总览表

`requestTimeoutSec_=15`。攻击者每秒发一个字节 `"G"`、`"E"`、`"T"`、`" "`...：

| 时刻 | 事件 | requestInProgress | requestStart | elapsedSec | 决策 |
|------|------|--------------------|--------------|-----------|------|
| T0 | 第 1 字节 'G' 到达，进入 onMessage | false → true | T0 | 0.0 | 走 parse，未完成 → break |
| T1=T0+1s | 第 2 字节 'E'，onMessage 重入 | true | T0 | 1.0 | 1.0 < 15 → 走 parse |
| ... | ... | ... | ... | ... | ... |
| T15=T0+14s | 第 15 字节，onMessage 重入 | true | T0 | 14.0 | 14.0 < 15 → 走 parse |
| T16=T0+15.1s | 第 16 字节，onMessage 重入 | true | T0 | 15.1 | **> 15** → 408 + close |

#### 第 1 步：首次记录 requestStart

代码：见 §9.3 前 4 行。

**代入实参**（T0）：
- `ctx->requestInProgress = false`
- 进入 if 体：置 true，记 `requestStart = T0`

#### 第 2 步：后续每次 onMessage 检查 elapsedSec

**代入实参**（T16）：
- `TimeStamp::now().microseconds() - ctx->requestStart.microseconds() = 15.1 * 10^6`
- `elapsedSec = 15.1`
- `15.1 > 15.0` → true

**分支走向**：
- `LOG_WARN`、`conn->send(408_response)`、`conn->close()`、`return`
- onMessage 不再继续 parse，连接进入关闭流程

#### 第 3 步：状态机视图

```
            首字节到达
                │
                ▼
      requestInProgress = true
      requestStart      = now()
                │
                ▼
     ┌───────────────────────────┐
     │  下一次 onMessage 重入     │
     │  (任何字节到达都会触发)    │
     └─────────────┬─────────────┘
                   ▼
         elapsedSec = now - start
                   ▼
        elapsedSec > requestTimeoutSec ?
          ┌────yes─────┴────no────┐
          ▼                       ▼
       408 + close            走 parse
          │              （未完成→break，
          │                完成→onRequest，
          │                成功后 reset()
          │                + requestInProgress=false）
          ▼
        连接终止
```

#### 第 4 步：函数职责一句话表

| 字段 / 函数 | 时机 | 职责 |
|-------------|-----|------|
| `requestInProgress` | onMessage 入口 | 标记是否已开始计时本次请求 |
| `requestStart` | 首次入口 | 记录第一次收到本次请求字节的时间 |
| `requestTimeoutSec_` | onMessage 每次入口 | 与 elapsedSec 比较 |
| 408 响应 | 超时分支 | 标准 HTTP "Request Timeout" |
| `requestInProgress=false`（onRequest 成功后） | 外层 while 继续轮询 | 让下一个 keep-alive 请求重新开始计时 |

---

## 10. 第 9 步 — 预留 TLS 接口（条件编译）

### 10.1 问题背景

公网线上服务必须 HTTPS。但完整 TLS 集成涉及：

- OpenSSL 链接（macOS / Ubuntu / 容器镜像各家细节都不同）
- 证书加载、私钥保护、SNI、会话复用、ALPN
- 异步握手与非阻塞 IO 的协同（SSL_accept 可能 WANT_READ / WANT_WRITE）
- 错误处理（握手失败、证书过期）

Day 29 不打算"今天就上 HTTPS"，但要在架构上把所有钩子打好。原则：**有 OpenSSL 时插件式启用，没 OpenSSL 时整段代码被 `#ifdef` 剔除**。

### 10.2 接口预留

来自 [HISTORY/day29/include/Connection.h](HISTORY/day29/include/Connection.h)（行 14–21、66–69、141–155）：

```cpp
#ifdef MCPP_HAS_OPENSSL
struct ssl_st;
using SSL = ssl_st;
struct ssl_ctx_st;
using SSL_CTX = ssl_ctx_st;
#endif
```

```cpp
#ifdef MCPP_HAS_OPENSSL
    bool enableTlsServer(SSL_CTX *ctx);
#endif
    bool tlsEnabled() const;
```

```cpp
#ifdef MCPP_HAS_OPENSSL
    enum class TlsState {
        kDisabled = 0,
        kHandshaking,
        kEstablished,
        kFailed,
    };

    ssize_t readFromTransport(char *buf, size_t len, int *savedErrno);
    ssize_t writeToTransport(const char *buf, size_t len, int *savedErrno);
    bool driveTlsHandshake();

    SSL *ssl_{nullptr};
    TlsState tlsState_{TlsState::kDisabled};
#endif
```

`tlsEnabled()` 是非条件编译接口，让外部代码总能问"是否启用 TLS"，宏关闭时返回 false。

### 10.3 写实现：三个接口桩 + TLS 状态机

来自 [HISTORY/day29/common/Connection.cpp](HISTORY/day29/common/Connection.cpp)（行 124–160，readFromTransport）：

```cpp
#ifdef MCPP_HAS_OPENSSL
ssize_t Connection::readFromTransport(char *buf, size_t len, int *savedErrno) {
    if (!ssl_ || tlsState_ != TlsState::kEstablished)
        return inputBuffer_.readFd(sock_->getFd(), savedErrno);

    int n = SSL_read(
        ssl_, buf,
        static_cast<int>(std::min(len, static_cast<size_t>(std::numeric_limits<int>::max()))));
    if (n > 0)
        return n;

    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        *savedErrno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN)
        return 0;

    unsigned long sslErr = ERR_get_error();
    LOG_ERROR << "[连接] SSL_read 失败，fd=" << sock_->getFd()
              << " sslErr=" << ERR_error_string(sslErr, nullptr);
    *savedErrno = EIO;
    return -1;
}
#endif
```

3 个函数（read / write / handshake）+ TlsState 4 状态机，构成"明文路径与 TLS 路径在同一份 doRead/doWrite 里共存、由 ssl_ 决定走哪条"的设计。

### 10.4 验证：追踪 TLS 握手（宏开启场景）

#### 业务场景 + 时序总览表

| 时刻 | 事件 | tlsState_ | sock 可读? | sock 可写? | 决策 |
|------|------|-----------|-----------|-----------|------|
| T0 | TcpServer::initTlsIfNeeded 加载 SSL_CTX | kDisabled | — | — | 准备 ctx_ |
| T1 | accept 成功，创建 Connection | kDisabled | — | — | enableTlsServer(ctx) |
| T2 | enableTlsServer 内部 SSL_new + SSL_set_fd + SSL_set_accept_state | kHandshaking | — | — | 等待客户端 ClientHello |
| T3 | 客户端 ClientHello 到达，doRead 调 driveTlsHandshake | kHandshaking | yes | — | SSL_accept 返回 WANT_READ |
| T4 | 多次往返直至握手完成 | kEstablished | yes | yes | enableReading 应用层数据 |
| T5 | doRead 走 readFromTransport → SSL_read 解密 | kEstablished | yes | yes | 业务层透明 |

握手细节略。重点：握手期间 doRead/doWrite 不直接访问 socket，全部经 driveTlsHandshake；握手成功后 read/write 路径通过 readFromTransport/writeToTransport 透明加密。

### 10.5 函数职责一句话表

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `enableTlsServer(ctx)` | accept 后立刻 | SSL_new + SSL_set_fd + SSL_set_accept_state |
| `driveTlsHandshake()` | 握手期 doRead/doWrite | SSL_accept 推进，处理 WANT_READ/WANT_WRITE |
| `readFromTransport()` | 握手完成后的 doRead | SSL_read（或明文 read 路径） |
| `writeToTransport()` | 握手完成后的 doWrite | SSL_write（或明文 write 路径） |
| `tlsEnabled()` | 任意外部调用 | 让上层无条件判断"本连接是不是 TLS" |

---

## 11. 工程化收尾

### 11.1 CMake / app_example / README 同步

- `HISTORY/day29/CMakeLists.txt`：add_executable 把 Day 29 新增 / 修改的所有 .cpp 加进来；`include_directories(include)` 让头文件路径与源码同步。
- `HISTORY/day29/app_example/`：用 `find_package(MyCppServerLib CONFIG)` 引用安装出去的核心库，通过外部链接验证"作为第三方库被人用"的接口稳定性。
- `HISTORY/day29/README.md` + `dev-log/day29-生产特性.md`：当日进展双轨记录。README 重构建命令与端到端验证脚本；dev-log 重设计动机与全流程追踪。

### 11.2 兼容遗留

`include/util.h` + `common/util.cpp` 仍然存在（`errif` 工具函数），用于尚未完全切到 Logger 的少量历史代码路径。Day 30 一并清理。

---

## 12. 验证

### 12.1 构建

```sh
cd HISTORY/day29
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build --prefix app_example/external/MyCppServerLib
cmake -S app_example -B app_example/build
cmake --build app_example/build -j$(nproc)
```

### 12.2 测试

```sh
cd HISTORY/day29/build && ctest --output-on-failure
```

期望：BackpressureDecisionTest / TcpServerPolicyTest / SocketPolicyTest / EpollPolicyTest（Linux）/ HttpContextTest / HttpRequestLimitsTest 全部 PASS。

### 12.3 端到端 curl 验证（已实测）

```sh
# 启动 demo
cd HISTORY/day29/app_example/build && MYCPPSERVER_BIND_PORT=8889 ./http_server &

# 1) 根路由
curl -sI http://127.0.0.1:8889/                # → 200 + ETag/Last-Modified/Content-Length

# 2) ETag 304 验证
ETAG=$(curl -sI http://127.0.0.1:8889/ | awk '/^[Ee]Tag:/ {print $2}' | tr -d '\r')
curl -s -o /dev/null -w "%{http_code}\n" -H "If-None-Match: $ETAG" http://127.0.0.1:8889/
# → 304

# 3) CORS 预检
curl -s -o /dev/null -w "%{http_code}\n" -X OPTIONS http://127.0.0.1:8889/
# → 204
curl -sI -X OPTIONS http://127.0.0.1:8889/ | grep -i "access-control"
# → Access-Control-Allow-Origin: ...

# 4) Gzip
curl -sI -H "Accept-Encoding: gzip" http://127.0.0.1:8889/ | grep -i "content-encoding\|vary"
# → Content-Encoding: gzip / Vary: Accept-Encoding
```

---

## 13. 局限与下一步

| 局限 | 说明 |
|------|------|
| TLS 未实际启用 | `MCPP_HAS_OPENSSL` 宏未定义，仅完成接口预留 |
| 前缀路由线性扫描 | `vector<PrefixRoute>` 路由数量大时性能衰退，可优化为 Trie |
| 中间件无异步支持 | 中间件链同步递归，不支持异步中间件（如远程鉴权） |
| sendFile + EAGAIN 降级有重发风险 | 见 §5.5 第 3 步注解，需在循环结束后同步 offset/count |
| 请求超时粗粒度 | 以 onMessage 回调为检查点，非精确计时器 |
| 手工测试框架 | 所有测试用手写 check/fail，缺少 GTest 集成 → Day 30 解决 |
| util.cpp 遗留 | errif 工具函数仍存在，待 Day 30 清理 |

接下来 **Day 30** 会做项目"收官清理"：把所有手工测试迁移到 GTest（FetchContent v1.14.0），清理 util.cpp/h 历史包袱，统一日志/工具入口，让整个仓库以一个"完整可发布的网络库"姿态进入下一个 Phase（WebSocket / 协程 / io_uring）。
