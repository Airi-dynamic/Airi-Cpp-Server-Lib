# Day 26 — HTTP 应用层演示（文件服务 / 静态页面 / 路由体系）

> **主题**：在 Day 25 的 HTTP 协议栈之上构建完整的应用层示例——文件上传下载、登录表单、静态页面渲染，以及空闲连接自动关闭机制。  
> **基于**：Day 25（HTTP 协议层 HttpServer / HttpContext）

---

## 1. 引言

### 1.1 问题上下文

到 Day 25，HTTP 协议栈能跑了，但只有 4 个最小路由，离"真实可用的 HTTP 服务"还差三件事：

1. **静态文件服务**：网页 / 图片 / 下载 文件需要 `sendfile` 或至少能从磁盘读、设置 `Content-Type`、断点续传（暂不做）。
2. **路由编排**：登录、上传、删除、列目录——每个 URL 对应一段业务逻辑，需要清晰的路由分发结构。
3. **空闲连接关闭**：HTTP/1.1 keep-alive 连接如果 60 秒没新请求就该关闭，否则慢速攻击（slowloris）能耗尽连接数。

同时，定时器和 Connection 之间的生命周期协调有一个微妙问题：定时器持有 Connection 指针时，如果 Connection 已被 delete，定时器到期回调就是 UAF。需要 `weak_ptr<bool>` 形式的"alive flag"——Connection 析构时把 flag 置为 false，定时器回调先检查 flag。

### 1.2 动机

这一天把"协议栈"升级为"应用框架"——演示如何在 HttpServer 之上写真实业务，并暴露出生产环境必备的 idle close、alive flag、HTML 模板替换等模式。

它也是后续 Day 28 性能测试、Day 36 muduo 横向对比的"被测物"——没有这层应用代码，benchmark 数字就没有意义。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 全部业务写在一个 callback 里 if-else | 教学 | 简单 | 不可维护 |
| URL → handler 路由表 (本日) | nginx / Express / Spring | 清晰、可扩展 | 路由匹配性能（线性 / trie） |
| 编译期路由（actix / axum 宏） | Rust | 零运行时开销 | 编译时间 |
| `sendfile` 零拷贝下载 | Linux 2.2+ | 零用户态拷贝 | 需 fd 是 regular file |
| `weak_ptr<bool>` alive flag (本日) | muduo / 本项目 | 简单防 UAF、无 shared_ptr 全局成本 | 多了一次原子读 |
| 全 `shared_ptr<Connection>` | muduo `TcpConnection` | 跨线程引用安全 | 引用计数全局开销 |

### 1.4 本日方案概述

本日实现：
1. `http_server.cpp` 重写为完整文件服务器：首页、登录页、文件管理页（列目录、下载、删除、上传）、4xx/5xx 错误页。
2. `Connection` 新增 `alive_` (`shared_ptr<bool>`) + `aliveFlag()` / `touchLastActive()` / `lastActive()`；析构时 `*alive_ = false`。
3. `HttpRequest::appendBody()`：高效追加 body（避免 setBody(body()+...) 的 O(n²) 拷贝）。
4. `HttpResponse`：`k301` → `k302Found`；新增 `addHeader()`。
5. `HttpServer::setAutoClose(true)` + `scheduleIdleClose()`：递归调度，每 idleTimeout 秒检查一次 `Connection::lastActive`，超时则 `conn->close()`。回调中先 `auto alive = conn->aliveFlag(); if (!*alive) return;` 防 UAF。
6. `static/*.html` 模板（首页、登录、文件管理含 `<!--filelist-->` 占位符）+ `files/*` 示例文件。
7. 新增 `test/BenchmarkTest.cpp`：内置压测工具，多线程长连接 GET 测 QPS / 延迟。

至此 HTTP 应用层完整可用，下一阶段（Day 27+）转向测试 / CI / 工程化。

---
## 2. 文件变更总览

| 文件 | 状态 | 说明 |
|------|------|------|
| `http_server.cpp` | **重写** | 完整 HTTP 文件服务器：首页、登录、文件管理、下载、删除、上传 |
| `server.cpp` | **修改** | 添加 `touchLastActive()` 演示空闲超时、定时器安全调用注释 |
| `include/Connection.h` | **修改** | 新增 `alive_` (shared_ptr<bool>) + `aliveFlag()` / `touchLastActive()` / `lastActive()` |
| `common/Connection.cpp` | **修改** | 析构时 `*alive_ = false`；`Read()` 中调用 `touchLastActive()` |
| `include/http/HttpRequest.h` | **修改** | 新增 `appendBody()` 高效追加 body（避免 O(n²) 拼接） |
| `include/http/HttpResponse.h` | **修改** | `k301` → `k302Found`（临时重定向）；新增 `addHeader()` |
| `include/http/HttpServer.h` | **修改** | 新增 `setAutoClose()` / `scheduleIdleClose()` / `autoClose_` / `idleTimeout_` |
| `common/http/HttpServer.cpp` | **修改** | 实现空闲超时定时器递归调度逻辑 |
| `common/http/HttpContext.cpp` | **修改** | body 解析改用 `appendBody()` 替代 `setBody(body()+...)` |
| `include/timer/TimerQueue.h` | **微调** | 补充 `runEvery()` 的文档注释 |
| `include/log/Logger.h` | **微调** | 新增 `LOG_FATAL` 宏 |
| `common/log/LogFile.cpp` | **微调** | 文件滚动日志格式调整 |
| `common/log/LogStream.cpp` | **微调** | 格式化输出优化 |
| `common/log/Logger.cpp` | **微调** | 日志级别字符串映射补全 |
| `test/LogTest.cpp` | **修改** | 添加异步日志测试场景 |
| `test/BenchmarkTest.cpp` | **新增** | HTTP 内置压测工具：多线程长连接 GET 测 QPS / 延迟 |
| `static/index.html` | **新增** | 首页 HTML 模板 |
| `static/login.html` | **新增** | 登录表单页 |
| `static/fileserver.html` | **新增** | 文件管理页模板（含 `<!--filelist-->` 占位符） |
| `files/readme.txt` | **新增** | 示例文本文件 |
| `files/scores.csv` | **新增** | 示例 CSV 文件 |
| `files/server.log` | **新增** | 示例日志文件 |

---

## 3. 模块全景与所有权树

```
http_server main()
  │ HttpServer srv
  │ srv.setAutoClose(true, 60.0)
  │ srv.setHttpCallback(handleRequest)
  │ srv.start()
  ▼
HttpServer
├── TcpServer（组合）
│   ├── mainReactor: Eventloop
│   │   ├── Poller (KqueuePoller / EpollPoller)
│   │   ├── TimerQueue
│   │   └── Acceptor → accept → 分配 Connection
│   ├── EventLoopThreadPool → subReactor × N
│   └── connections: map<int, unique_ptr<Connection>>
│       └── Connection
│           ├── Socket, Channel, Buffer(in/out)
│           ├── context_: std::any ← HttpContext
│           ├── alive_: shared_ptr<bool> ★ NEW
│           └── lastActive_: TimeStamp   ★ NEW
├── autoClose_  → 启用空闲超时检测
├── idleTimeout_ → 超时阈值（秒）
├── scheduleIdleClose(conn)
│   └── conn->getLoop()->runAfter(idleTimeout_, [wk=conn->aliveFlag()] {
│       if (auto p = wk.lock(); p && *p) {  // 连接仍存活
│           if (TimeStamp::now() - conn->lastActive() > timeout)
│               conn->close();
│           else
│               scheduleIdleClose(conn);     // 递归再调度
│       }
│   })
└── httpCallback_ → handleRequest()
    ├── GET /                  → index.html
    ├── GET /login.html        → login.html
    ├── GET /fileserver        → 动态生成文件列表
    ├── GET /download/<name>   → 文件下载 (Content-Disposition)
    ├── GET /delete/<name>     → 删除后重定向
    ├── POST /login            → 表单解析 → 重定向
    ├── POST /upload           → multipart/form-data 解析
    └── * 其他                 → 404
```

---

## 4. 全流程调用链

### 4.1 文件上传全流程

```
客户端浏览器：
  POST /upload HTTP/1.1
  Content-Type: multipart/form-data; boundary=----WebKitXXX
  Content-Length: 2345
  
  ------WebKitXXX
  Content-Disposition: form-data; name="file"; filename="test.txt"
  Content-Type: text/plain
  
  <文件内容>
  ------WebKitXXX--

  ① TCP 数据到达 + 分段重组
  ──────────────────────────────────────────────────
  Connection::Read() → inputBuffer_
  HttpContext::parse()
    ├── 状态机解析 headers → Content-Length = 2345
    ├── kBody 状态：appendBody() 逐段累积
    │   └── 多次 TCP 分段到达 → 重入 parse() → 继续追加
    └── body.size() >= Content-Length → kComplete

  ② 路由到 POST /upload
  ──────────────────────────────────────────────────
  handleRequest(req, resp)
    ├── extractBoundary(req.header("Content-Type"))
    │   → "----WebKitXXX"
    ├── parseMultipart(req.body(), boundary, file)
    │   ├── 定位 --boundary + \r\n
    │   ├── 提取 headers → filename="test.txt"
    │   ├── 定位 \r\n\r\n → 文件内容起始
    │   └── 截取到下一个 \r\n--boundary → file.data
    ├── isSafeFilename(file.filename)   ← 安全校验
    │   └── 禁止 ".." / "/" / "\0"
    ├── ofstream(kFilesDir + "/" + file.filename)
    │   └── write(file.data)
    └── redirect(resp, "/fileserver")   ← 302 重定向
```

### 4.2 空闲连接超时关闭

```
  新连接建立
  ──────────────────────────────────────────────────
  HttpServer::onConnection(conn)
    └── scheduleIdleClose(conn)
        └── conn->getLoop()->runAfter(60.0, callback)

  定时器触发
  ──────────────────────────────────────────────────
  TimerQueue::handleExpired()
    └── callback()
        ├── wk.lock() → 检查连接存活
        │   └── alive_ == false? → 连接已析构，直接返回
        ├── TimeStamp::now() - conn->lastActive() > timeout?
        │   ├── YES → conn->close()
        │   └── NO  → scheduleIdleClose(conn) // 递归再调度
        └── ★ 线程安全：回调始终在 conn 所属的 sub-reactor 线程执行
```

---

## 5. 代码逐段解析

### 5.1 alive_ / aliveFlag() — 定时器安全回调

```cpp
// Connection.h
class Connection {
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    TimeStamp lastActive_;
public:
    std::weak_ptr<bool> aliveFlag() const { return alive_; }
    void touchLastActive() { lastActive_ = TimeStamp::now(); }
    TimeStamp lastActive() const { return lastActive_; }
};
```

**设计动机**：Connection 由 `unique_ptr` 管理，定时器回调执行时连接可能已被销毁。
使用 `shared_ptr<bool>` + `weak_ptr<bool>` 模式判断连接存活性：
- Connection 持有 `shared_ptr<bool>(true)`
- 定时器持有 `weak_ptr<bool>`
- Connection 析构 → `*alive_ = false` → `weak_ptr::lock()` 返回的指针解引用为 false
- 比裸指针判空更安全：避免 dangling pointer 的 UB

### 5.2 multipart/form-data 解析

```cpp
static bool parseMultipart(const std::string &body,
                           const std::string &boundary,
                           UploadedFile &out) {
    const std::string delim = "--" + boundary;
    size_t partStart = body.find(delim);
    partStart += delim.size() + 2;          // 跳过 \r\n
    size_t headersEnd = body.find("\r\n\r\n", partStart);
    // 提取 filename="xxx" → out.filename
    size_t dataStart = headersEnd + 4;
    size_t dataEnd = body.find("\r\n" + delim, dataStart);
    out.data = body.substr(dataStart, dataEnd - dataStart);
    return !out.filename.empty();
}
```

解析步骤：
1. 定位 `--boundary\r\n` → part 起始
2. 定位 `\r\n\r\n` → headers 与 body 分界
3. 从 `Content-Disposition` 提取 `filename`
4. 截取 headers 结束到下一个 `\r\n--boundary` 之间的内容作为文件数据

### 5.3 appendBody() — 避免 O(n²) body 拼接

```cpp
// HttpRequest.h — Day 25 原版：
//   setBody(body() + std::string(data, len))
//   每次调用：先拷贝旧 body（O(n)），再追加新数据
//   k 次分段 body 总复杂度：O(n₁) + O(n₁+n₂) + ... = O(k·n)

// Day 26 改进：
void appendBody(const char *data, int len) {
    body_.append(data, len);  // 直接追加到已有 string 尾部
}
// 均摊 O(1)，总复杂度 O(n)，对大文件上传性能提升显著
```

### 5.4 URL 解码 + 文件名安全校验

```cpp
static std::string urlDecode(const std::string &str) {
    // %E7%AE%80 → 中文字符    + → 空格
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int hex;
            std::istringstream iss(str.substr(i + 1, 2));
            iss >> std::hex >> hex;
            result += static_cast<char>(hex);
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
}

static bool isSafeFilename(const std::string &name) {
    if (name.find("..") != std::string::npos) return false;  // 路径遍历
    if (name.find('/') != std::string::npos)  return false;  // 目录跳转
    if (name.find('\0') != std::string::npos) return false;  // 空字节注入
    return !name.empty();
}
```

### 5.5 302 重定向

```cpp
static void redirect(HttpResponse *resp, const std::string &location) {
    resp->setStatus(HttpResponse::StatusCode::k302Found, "Found");
    resp->addHeader("Location", location);
    resp->setContentType("text/html; charset=utf-8");
    resp->setBody("<html><body>Redirecting to ...</body></html>");
}
```

HTTP 302 Found：客户端收到后自动跳转到 `Location` 头指定的 URL。  
用于 POST 后重定向（PRG 模式 — Post/Redirect/Get），防止浏览器刷新时重复提交表单。

### 5.6 BenchmarkTest — 内置 HTTP 压测工具

```cpp
// 工作线程函数
void workerThread(const Config &cfg, Stats &stats) {
    int fd = connectToServer(cfg.host, cfg.port);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, ...);  // 禁用 Nagle
    std::string request = "GET " + cfg.url + " HTTP/1.1\r\n"
                          "Host: " + cfg.host + "\r\n"
                          "Connection: keep-alive\r\n\r\n";
    while (g_running) {
        auto t0 = steady_clock::now();
        doRequest(fd, request, recvBuf, sizeof(recvBuf));
        auto t1 = steady_clock::now();
        stats.requests++;
        stats.latencyUs += duration_cast<microseconds>(t1 - t0).count();
    }
}
```

特点：
- 多线程，每线程复用长连接（keep-alive）
- 统计 QPS、平均延迟、最小/最大延迟
- 零外部依赖，项目自包含

---

### 5.7 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day26/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day26/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 组件 | 职责 | 不变量 |
|------|------|--------|
| `HttpServer` | HTTP 协议封装 + 连接管理 + 空闲超时 | 每个连接持有独立 HttpContext |
| `Connection::alive_` | 异步安全判活标志 | 析构时置 false，不可逆 |
| `Connection::lastActive_` | 最近活跃时间戳 | 每次 Read() 更新 |
| `scheduleIdleClose()` | 递归定时器调度 | weak_ptr 防野指针 |
| `http_server.cpp` | 路由分发 + 文件 I/O + 安全校验 | isSafeFilename 阻止路径遍历 |
| `BenchmarkTest` | HTTP 内置压测 | TCP_NODELAY + keep-alive |
| `appendBody()` | O(1) 均摊 body 追加 | 替代 setBody() 避免 O(n²) |

---

## 7. 局限与后续

| 局限 | 影响 | 后续方案 |
|------|------|----------|
| multipart 解析仅支持单文件 | 多文件上传场景不可用 | 循环查找每个 part |
| 静态文件从磁盘实时读取 | 高并发时 I/O 瓶颈 | Day 27 引入内存缓存 |
| alive_ 使用 shared_ptr<bool> | 额外堆分配 | 可改为 Connection 内嵌原子标志 |
| URL 路由硬编码 if-else 链 | 路由数量增长时不优雅 | 注册式路由表 map<string, handler> |
| BenchmarkTest 仅支持 GET | 无法测试 POST 性能 | 扩展请求类型参数 |

---

## 8. 构建 & 运行

```bash
cd HISTORY/day26 && rm -rf build && mkdir build && cd build
cmake .. && make -j4

# 启动 HTTP 服务器（需要从 build/ 目录运行，以找到 static/ 和 files/）
./http_server

# 另一终端：压测
./BenchmarkTest 127.0.0.1 8888 / 4 10

# 浏览器访问
# http://127.0.0.1:8888/           首页
# http://127.0.0.1:8888/login.html 登录表单
# http://127.0.0.1:8888/fileserver  文件管理
```
