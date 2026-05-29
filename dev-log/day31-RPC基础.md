# Day 31 — RPC 基础层：二进制帧协议 / RpcServer / RpcClient

> **今日目标**：为后续 Raft 选举实现可复用的 RPC 通信层。  
> **背景**：Day 30 结束后项目转向分布式存储方向（见 `dev-log/README.md`）。Raft 节点间需要互发 RequestVote / AppendEntries 两种 RPC；这些调用频率低（< 100/s）、要求低延迟（< election timeout），最适合用**短连接同步阻塞**模型。  
> **基于**：Day 30（全特性 HTTP 服务器）。**结束**：分布式存储 Phase 1 第一步。下一步（Day 32）是 Raft 选主。

---

## 0. 今日构建目标

本日工作分三条主线：

1. **§2** — 设计二进制帧协议 `RpcMessage`：12 字节定长头 + JSON payload，支持编解码与粘包处理
2. **§3** — 实现 `RpcServer`：复用现有 `TcpServer`，通过 `addHandler` 注册方法，以事件驱动模型处理请求
3. **§4** — 实现 `RpcClient`：短连接同步阻塞客户端，每次调用独立建立并关闭 TCP 连接，带超时保护

**新增文件清单：**

| 文件 | 作用 |
|------|------|
| `src/include/rpc/RpcMessage.h` | 消息结构体 + encode/decode 声明 |
| `src/common/rpc/RpcMessage.cpp` | 序列化 / 反序列化实现 |
| `src/include/rpc/RpcServer.h` | RPC 监听端声明 |
| `src/common/rpc/RpcServer.cpp` | RPC 监听端实现 |
| `src/include/rpc/RpcClient.h` | RPC 调用端声明 |
| `src/common/rpc/RpcClient.cpp` | RPC 调用端实现 |
| `examples/src/rpc_demo.cpp` | 端到端演示：echo + add 两个方法 |

---

## 1. 引言——为什么需要独立的 RPC 层

### 1.1 具体触发场景

Raft 算法要求节点之间互发两种消息：

- **RequestVote**：candidate 广播拉票，要求所有 peer 在 election timeout（150–300 ms）内回复
- **AppendEntries**：leader 广播日志条目，要求 quorum 在 heartbeat interval（50 ms）内确认

如果没有专门的 RPC 层，最直接的替代方案是用 HTTP：把 RequestVote 参数 JSON 化、用 `POST /raft/request_vote` 发送。但 HTTP 引入了路由匹配、头部解析、Content-Type 协商等约 3~5 µs 的额外延迟，而 Raft 选举超时通常在 150 ms 量级——虽然不是不可用，但用专门的轻量二进制帧协议会更干净，也更贴近真实系统的做法。

### 1.2 设计约束

| 约束 | 来源 | 选型结果 |
|------|------|---------|
| 不引入 protobuf / flatbuffers 等外部依赖 | 项目使用 FetchContent 管理依赖，需保持精简 | 手写 JSON payload + 二进制定长头 |
| 不实现 TcpClient（项目无此类） | 现有库只有 TcpServer 侧 | RpcClient 直接用 POSIX raw socket |
| 选举 RPC 频率低（< 100/s） | Raft 论文 §5.2 | 短连接同步阻塞，无需连接池 |
| 必须有超时保护 | 网络分区 / peer 宕机场景 | `SO_RCVTIMEO` + `SO_SNDTIMEO` |

---

## 2. 改进 A — 二进制帧协议 `RpcMessage`

### 2.1 业务场景

在三节点 Raft 集群（端口 18901/18902/18903）中，Node 1 发起选举时需要在 200 ms 内收到 Node 2 和 Node 3 的 RequestVote 响应。每条 RequestVote 消息约 80 字节的 JSON payload；如果帧格式设计不合理（比如纯文本换行分隔），就需要在接收端做更复杂的边界检测，且无法在不解析 payload 的情况下得知消息类型。

### 2.2 接口 / 数据结构

来自 [src/include/rpc/RpcMessage.h](src/include/rpc/RpcMessage.h)：

```cpp
#pragma once
#include <cstdint>
#include <string>

struct RpcMessage {
    enum class Type : uint32_t {
        kRequest = 0,
        kResponse = 1,
        kOneWay = 2,
    };

    Type type{Type::kRequest};
    uint32_t reqId{0};
    std::string method;
    std::string payload;

    std::string encode() const;
    static bool decode(const char *data, int len, RpcMessage *out, int *consumed);
};
```

帧格式（共 12 + N 字节）：

```
 0               4               8               12      12+N
 ┌───────────────┬───────────────┬───────────────┬────── ... ──┐
 │   length (4B) │  msgType (4B) │   reqId  (4B) │  JSON body  │
 │  big-endian   │  big-endian   │  big-endian   │   (N bytes) │
 └───────────────┴───────────────┴───────────────┴────── ... ──┘
```

JSON body 格式固定为 `{"method":"<method>","body":<payload>}`。

### 2.3 编码实现

来自 [src/common/rpc/RpcMessage.cpp](src/common/rpc/RpcMessage.cpp)：

```cpp
std::string RpcMessage::encode() const {
    // 1. 构造 JSON body
    //    格式：{"method":"<method>","body":<payload>}
    //    payload 本身已经是合法 JSON 字符串（调用方保证）
    std::string json;
    json += "{\"method\":\"";
    json += method;
    json += "\",\"body\":";
    json += payload;
    json += "}";

    // 2. 计算总长度
    uint32_t payloadLen = static_cast<uint32_t>(json.size());

    // 3. 构造 12 字节定长头（全部转网络字节序）
    uint32_t netLen     = htonl(payloadLen);
    uint32_t netType    = htonl(static_cast<uint32_t>(type));
    uint32_t netReqId   = htonl(reqId);

    // 4. 拼装完整帧
    std::string frame;
    frame.resize(12 + payloadLen);
    memcpy(frame.data() + 0, &netLen,   4);
    memcpy(frame.data() + 4, &netType,  4);
    memcpy(frame.data() + 8, &netReqId, 4);
    memcpy(frame.data() + 12, json.data(), payloadLen);
    return frame;
}
```

### 2.4 解码实现（粘包处理核心）

来自 [src/common/rpc/RpcMessage.cpp](src/common/rpc/RpcMessage.cpp)：

```cpp
bool RpcMessage::decode(const char *data, int len,
                        RpcMessage *out, int *consumed) {
    // 1. 至少要有 12 字节头
    if (len < 12) return false;

    uint32_t netLen, netType, netReqId;
    memcpy(&netLen,   data + 0, 4);
    memcpy(&netType,  data + 4, 4);
    memcpy(&netReqId, data + 8, 4);

    uint32_t payloadLen = ntohl(netLen);
    out->type   = static_cast<RpcMessage::Type>(ntohl(netType));
    out->reqId  = ntohl(netReqId);

    // 2. 检查 payload 是否到齐（粘包处理核心）
    if (len < 12 + static_cast<int>(payloadLen)) return false;

    // 3. 解析 JSON：只提取 method 和 body
    std::string json(data + 12, payloadLen);

    auto findStr = [&](const std::string &key) -> std::string {
        std::string needle = "\"" + key + "\":\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    };

    out->method = findStr("method");

    std::string bodyKey = "\"body\":";
    auto bodyPos = json.find(bodyKey);
    if (bodyPos != std::string::npos) {
        out->payload = json.substr(bodyPos + bodyKey.size(),
                                   json.size() - (bodyPos + bodyKey.size()) - 1);
    }

    *consumed = 12 + static_cast<int>(payloadLen);
    return true;
}
```

### 2.5 全流程追踪

#### 第 1 块：业务场景与时序总览

场景：Node 1 (client) 向 Node 2 (server, port 18901) 发送一次 `echo` RPC，请求正文 `{"msg":"hello-0"}`（17 字节）。

| 时刻 | 动作 | 关键状态量 | 结果 |
|------|------|-----------|------|
| T0 | `encode()` 被调用 | `method="echo"`, `payload={"msg":"hello-0"}` (17B) | JSON body = `{"method":"echo","body":{"msg":"hello-0"}}` = 38B |
| T1 | 拼装帧头 | `payloadLen=38`, `netLen=htonl(38)=0x00000026`, `type=kRequest=0`, `reqId=1` | `frame` = 50B（12头 + 38体） |
| T2 | 帧到达服务端 | `recvBuf.size()` 从 0 增加到 50 | `decode()` 触发 |
| T3 | `decode()` 检查头 | `len=50 >= 12` ✓；`payloadLen=38`；`len=50 >= 50` ✓ | 继续解析 JSON |
| T4 | 解析 method/body | `findStr("method")` 找到 `"echo"`；`bodyPos` 找到 `{"msg":"hello-0"}` | `out->method="echo"`, `out->payload={"msg":"hello-0"}`, `*consumed=50` |

#### 第 2 块：逐步代码追踪

**第 1 步：encode() 构造帧**

进入 `RpcMessage::encode()`，`this->method="echo"`, `this->payload={"msg":"hello-0"}`。

代入实参：
- `json = {"method":"echo","body":{"msg":"hello-0"}}` → `json.size() = 38`
- `payloadLen = 38`
- `netLen = htonl(38) = 0x26000000`（注意内存中大端存储）
- `netType = htonl(0) = 0x00000000`
- `netReqId = htonl(1) = 0x01000000`
- `frame.resize(50)` → 前 12 字节写入三个 big-endian uint32，后 38 字节写入 JSON

帧内容（十六进制前12字节）：
```
00 00 00 26  00 00 00 00  00 00 00 01  7b 22 6d 65 ...
└── len=38 ┘ └ type=0(Req)┘ └ reqId=1 ┘ └── JSON start ──
```

**第 2 步：decode() 验证粘包边界**

进入 `RpcMessage::decode(data=buf, len=50, out, consumed)`。

判断分支走向：
- `len=50 >= 12`？是 → 读取头部三个 uint32
- `payloadLen = ntohl(0x26000000) = 38`
- `len=50 >= 12+38=50`？是（等于）→ 数据恰好完整，继续

副作用：
```
out->type   = kRequest (0)
out->reqId  = 1
out->method = "echo"
out->payload = {"msg":"hello-0"}
*consumed   = 50
```

若此刻只到达了前 20 字节（`len=20 < 50`）：第二个 `if` 判断为 false，`decode()` 返回 `false`，接收方继续等待更多数据——这就是定长头解决粘包的核心机制。

#### 第 3 块：帧状态机

```
              收到 < 12 字节
                   │
   ┌───────────────▼──────────────┐
   │      等待头部（< 12B）        │◄─── 继续 read()
   └───────────────┬──────────────┘
                   │ 收到 ≥ 12 字节，读取 payloadLen
                   ▼
   ┌───────────────────────────────┐
   │  等待 body（< 12+payloadLen）  │◄─── 继续 read()
   └───────────────┬───────────────┘
                   │ 收到 ≥ 12+payloadLen 字节
                   ▼
   ┌───────────────────────────────┐
   │  完整帧，decode() 返回 true    │
   │  *consumed = 12+payloadLen    │
   └───────────────────────────────┘
```

#### 第 4 块：职责表

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `encode()` | 发送方组帧 | 把 method + payload 打包成 12B头 + JSON，返回完整字节串 |
| `decode()` | 接收方每次数据到达后 | 尝试从字节流头部解析一条完整消息；用 `*consumed` 告知调用方消费了多少字节 |

---

## 3. 改进 B — `RpcServer`：复用 TcpServer 的监听端

### 3.1 业务场景

三节点集群启动后，每个节点需要作为 RPC 服务端等待其他节点的调用。服务端不知道会收到哪种请求（RequestVote 还是 AppendEntries），需要一种**方法名 → 处理函数**的注册表机制，且不能硬编码方法名。

### 3.2 接口

来自 [src/include/rpc/RpcServer.h](src/include/rpc/RpcServer.h)：

```cpp
class RpcServer {
    public:
    using Handler = std::function<std::string(const std::string&)>;
    RpcServer(const std::string& ip, uint16_t port, int ioThreads = 1);

    void addHandler(const std::string& method, Handler handler);
    void start();
    void stop();

    private:
    void onMessage(Connection* conn);
    void onNewConn(Connection* conn);
    TcpServer server_;
    std::unordered_map<std::string, Handler> handlers_;
};
```

### 3.3 连接上下文与消息处理

来自 [src/common/rpc/RpcServer.cpp](src/common/rpc/RpcServer.cpp)：

```cpp
struct RpcConnCtx {
    std::string buf; // 已收到但未解析完整帧的字节数据
};

RpcServer::RpcServer(const std::string &ip, uint16_t port, int ioThreads)
    : server_([&] {
          TcpServer::Options opt;
          opt.listenIp = ip;
          opt.listenPort = port;
          opt.ioThreads = ioThreads;
          return opt;
      }()) {
    server_.newConnect([this](Connection *conn) { onNewConn(conn); });
    server_.onMessage([this](Connection *conn) { onMessage(conn); });
}
```

```cpp
void RpcServer::onMessage(Connection *conn) {
    auto *ctx = conn->getContextAs<RpcConnCtx>();

    Buffer *buf = conn->getInputBuffer();
    size_t n = buf->readableBytes();
    ctx->buf.append(buf->peek(), n);
    buf->retrieve(n);

    while (true) {
        RpcMessage msg;
        int consumed = 0;
        bool ok = RpcMessage::decode(ctx->buf.data(), static_cast<int>(ctx->buf.size()), &msg,
                                     &consumed);
        if(!ok) break;
        ctx->buf.erase(0, consumed);
        if(msg.type != RpcMessage::Type::kRequest) continue;
        std::string responsePayload = "{\"error\":\"unknown method\"}";
        auto it = handlers_.find(msg.method);
        if(it != handlers_.end()){
            responsePayload = it->second(msg.payload);
        }
        RpcMessage resp;
        resp.type    = RpcMessage::Type::kResponse;
        resp.reqId   = msg.reqId;
        resp.method  = msg.method;
        resp.payload = responsePayload;
        conn->send(resp.encode());
    }
}
```

### 3.4 嵌入执行路径

`onMessage` 在 TcpServer 的 sub-reactor 线程上被调用——即每当 TCP 缓冲区有新数据可读时，底层 KqueuePoller 触发 POLLIN，EventLoop 调用 `Channel::handleRead()`，最终到达 `Connection::onReadable()`，后者调用用户注册的 message 回调，也就是这里的 `onMessage`。

### 3.5 全流程追踪

#### 第 1 块：时序总览

场景：客户端发来一条完整的 50 字节 echo RPC 帧，`handlers_["echo"]` 已注册为原样返回。

| 时刻 | 动作 | 关键状态量 | 结果 |
|------|------|-----------|------|
| T0 | sub-reactor POLLIN 触发 | `buf->readableBytes() = 50` | 进入 `onMessage` |
| T1 | 追加到 ctx->buf | `ctx->buf.size()` 从 0 → 50 | `buf->retrieve(50)` 清空 TcpBuffer |
| T2 | `decode()` 尝试解析 | `len=50`, `payloadLen=38`, `50 >= 50` ✓ | 解析成功，`consumed=50` |
| T3 | `erase(0, 50)` | `ctx->buf.size()` 从 50 → 0 | 防止重复处理 |
| T4 | 查找 `"echo"` handler | `handlers_.find("echo")` ✓ | 调用 handler，返回 `{"msg":"hello-0"}` |
| T5 | 构造响应帧 | `resp.type=kResponse`, `resp.reqId=1`, `resp.payload={"msg":"hello-0"}` | `conn->send(50B frame)` |

#### 第 2 块：逐步追踪

**第 1 步：追加字节到 ctx->buf**

判断分支走向：
- `buf->readableBytes() = 50 > 0` → `ctx->buf.append(buf->peek(), 50)`
- `buf->retrieve(50)` 清空 TcpBuffer，保证同一批数据不被重复消费

**第 2 步：decode() 循环**

- 第一次 `decode()` 返回 `true`，`consumed=50` → `erase(0,50)` → `ctx->buf.size()=0`
- `msg.type == kRequest` ✓ → 不 continue
- `handlers_.find("echo")` 找到 → `responsePayload = handler({"msg":"hello-0"}) = {"msg":"hello-0"}`
- 第二次循环：`ctx->buf.size()=0 < 12` → `decode()` 返回 false → break

**第 3 步：发送响应**

```
resp.type    = kResponse (1)
resp.reqId   = 1          （与请求 reqId 一致，客户端用此校验）
resp.payload = {"msg":"hello-0"}
→ encode() → 50B frame → conn->send()
```

#### 第 3 块：职责表

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `onNewConn` | 新 TCP 连接建立时 | 在 conn 上附加 `RpcConnCtx{}`，初始化拼包缓冲区 |
| `onMessage` | sub-reactor POLLIN 事件 | 追加字节 → 循环解帧 → 派发 handler → 发送响应 |
| `addHandler` | 服务端启动前 | 注册方法名到处理函数的映射 |

---

## 4. 改进 C — `RpcClient`：短连接同步阻塞客户端

### 4.1 业务场景

Raft 节点发起 RequestVote 时，需要在 election timeout（150–300 ms）内完成"连接 → 发送 → 接收"全程。如果 peer 宕机，不能无限等待，必须在 `timeoutMs` 内超时返回 `false`，触发 Raft 层的降级处理（视为投票拒绝）。

### 4.2 接口

来自 [src/include/rpc/RpcClient.h](src/include/rpc/RpcClient.h)：

```cpp
class RpcClient {
  public:
    RpcClient(const std::string &serverIp, uint16_t serverPort, int timeoutMs = 200);

    // 发起一次 RPC 调用
    //   method      ：方法名，须与 RpcServer::addHandler 注册的一致
    //   requestJson ：请求正文（JSON 字符串）
    //   responseJson：成功时填入响应正文
    //   返回 true = 调用成功
    bool call(const std::string &method, const std::string &requestJson,
              std::string &responseJson);

  private:
  std::string ip_;
  uint16_t port_;
  int timeoutMs_;

  static uint32_t nextReqId();
};
```

### 4.3 实现

来自 [src/common/rpc/RpcClient.cpp](src/common/rpc/RpcClient.cpp)：

```cpp
bool RpcClient::call(const std::string &method, const std::string &requestJson,
                     std::string &responseJson) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    // 设置发送/接收超时，防止 Raft 选举被阻塞
    struct timeval tv;
    tv.tv_sec = timeoutMs_ / 1000;
    tv.tv_usec = (timeoutMs_ % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    ::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    RpcMessage req;
    req.type = RpcMessage::Type::kRequest;
    req.reqId = nextReqId();
    req.method = method;
    req.payload = requestJson;
    std::string frame = req.encode();

    // ── 1. 发送完整帧（循环 write 直到所有字节发完）
    const char *p = frame.data();
    int rem = static_cast<int>(frame.size());
    while (rem > 0) {
        int n = static_cast<int>(::write(fd, p, rem));
        if (n <= 0) { ::close(fd); return false; }
        p += n;
        rem -= n;
    }

    // ── 2. 接收响应帧（循环 read 直到能解析出一条完整消息）
    std::string recvBuf;
    char tmp[4096];
    while (true) {
        int n = static_cast<int>(::read(fd, tmp, sizeof(tmp)));
        if (n <= 0) { ::close(fd); return false; }
        recvBuf.append(tmp, n);

        RpcMessage resp;
        int consumed = 0;
        if (RpcMessage::decode(recvBuf.data(),
                               static_cast<int>(recvBuf.size()),
                               &resp, &consumed)) {
            if (resp.reqId == req.reqId) {
                responseJson = resp.payload;
                ::close(fd);
                return true;
            }
        }
    }
}
```

### 4.4 全流程追踪

#### 第 1 块：时序总览

场景：`call("echo", '{"msg":"hello-0"}', resp)` 在本机回环（127.0.0.1:18901）RTT ≈ 0.2 ms，`timeoutMs=500`。

| 时刻 | 动作 | 关键状态量 | 结果 |
|------|------|-----------|------|
| T0 | `socket()` + `setsockopt()` | `tv = {0s, 500000µs}` | fd 建立，收发各 500ms 超时 |
| T1 | `connect()` | `addr = 127.0.0.1:18901` | 三次握手完成 (RTT ≈ 0.2ms) |
| T2 | `encode()` + 发送循环 | `frame.size() = 50`，`write()` 返回 50 | `rem = 0`，全部发完 |
| T3 | 接收循环第一次 `read()` | `n = 50`，`recvBuf.size() = 50` | `decode()` 成功 |
| T4 | 验证 `resp.reqId == req.reqId` | `resp.reqId = 1 == req.reqId = 1` ✓ | `responseJson` 被填入，`close(fd)`，返回 `true` |

#### 第 2 块：超时路径追踪

若 peer 宕机（T1 之后 `connect()` 无响应）：

- `connect()` 本身不受 `SO_RCVTIMEO` 影响（它由系统 TCP 超时控制，约 75s）。
- 实际 Raft 层会在调用前先确认 peer 可达，或通过 election timeout 触发重新选举而非等待单次 RPC 超时。

> **注**：这是 Day 31 已知的一个局限，见 §11.1。

#### 第 3 块：发送循环 vs 接收循环的串行关系

```
call() {
    ── 发送阶段 ──────────────────────────────────
    while (rem > 0) {          ← 循环直到发完所有字节
        write(fd, p, rem)      ← SO_SNDTIMEO 兜底
        p += n; rem -= n;
    }
    ── 接收阶段 ──────────────────────────────────
    while (true) {             ← 循环直到收到完整响应帧
        read(fd, tmp, 4096)    ← SO_RCVTIMEO 兜底
        decode(recvBuf)
        if ok && reqId match → return true
    }
}
```

两个循环**串行**，不嵌套。这是本日调试中修复的主要逻辑错误（原先接收循环被嵌套在发送循环内，且 `p += n; rem -= n;` 误置于 `return false` 之后成为死代码）。

#### 第 4 块：职责表

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| 构造函数 | 使用前 | 记录 ip/port/timeout，不建立连接 |
| `call()` | 每次 RPC 调用 | 建立连接 → 发帧 → 收帧 → 关闭连接；完全同步阻塞 |
| `nextReqId()` | `call()` 内部 | 生成单调递增的请求 ID，用于响应匹配 |

---

## 9. 工程化收尾

### 9.1 CMakeLists.txt 变动

`GLOB_RECURSE "src/common/*.cpp"` 已自动覆盖 `src/common/rpc/*.cpp`，无需手动追加源文件到 `NetLib`。

新增 `rpc_demo` 可执行目标（追加在 `http_server` 目标后）：

```cmake
add_executable(rpc_demo examples/src/rpc_demo.cpp)
target_link_libraries(rpc_demo NetLib pthread)
set_target_properties(rpc_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MCPP_EXAMPLE_DIR})
```

### 9.2 端口约定

| 用途 | 端口 |
|------|------|
| Raft RPC Node 1/2/3 | 18901 / 18902 / 18903 |
| HTTP KV 接口 Node 1/2/3 | 28901 / 28902 / 28903 |

---

## 10. 验证

### 10.1 编译验证

```bash
# 三个 RPC 文件独立编译
clang++ -std=c++17 -Isrc/include \
    src/common/rpc/RpcMessage.cpp \
    src/common/rpc/RpcServer.cpp \
    src/common/rpc/RpcClient.cpp \
    -c && echo "✅ ALL OK"
```

输出：
```
✅ ALL OK
```

### 10.2 端到端验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rpc_demo -j

./build/examples/rpc_demo --server --port 18901 &
SERVER_PID=$!
sleep 0.3
./build/examples/rpc_demo --client --port 18901 --n 3
kill $SERVER_PID
```

实际输出：

```
[server] RPC server listening on 0.0.0.0:18901
[client] echo #0 ✓  resp={"msg":"hello-0"}
[client] echo #1 ✓  resp={"msg":"hello-1"}
[client] echo #2 ✓  resp={"msg":"hello-2"}
[client] add(3,4) => {"result":7}
[client] done: 3/3 success
[server] stopped.
```

3/3 全部通过，`add(3,4)` 正确返回 7。

---

## 11. 局限与下一步

### 11.1 已知局限

| 局限 | 描述 |
|------|------|
| `connect()` 无独立超时 | `SO_RCVTIMEO/SO_SNDTIMEO` 只影响 `read/write`，`connect()` 超时由系统 TCP 栈控制（约 75s），在 peer 完全不可达时会长时间阻塞 |
| `g_reqId` 非线程安全 | `static uint32_t g_reqId{0}` 是非原子递增，多线程并发调用 `call()` 时存在 data race；Raft 的使用场景下每个节点通常在单线程中发 RPC，暂可接受 |
| 无连接池 | 每次 `call()` 都 connect+close，适合低频 Raft RPC（< 100/s），不适合高频场景 |
| JSON 解析极简 | `decode()` 里用字符串搜索提取 method/body，对嵌套 JSON 中包含 `"body":` 子串的 payload 可能误匹配；后续引入 nlohmann/json 可彻底消除 |

### 11.2 下一步

接下来 Day 32 会在今天的 RPC 层之上实现 **Raft 选主**：

- 新建 `src/include/raft/RaftTypes.h`——定义 `State`（Follower/Candidate/Leader）、`LogEntry`、`RequestVoteArgs/Reply`、`AppendEntriesArgs/Reply`
- 新建 `src/include/raft/RaftNode.h` / `src/common/raft/RaftNode.cpp`——三角色状态机、随机选举超时（复用 `EventLoop::runAfter()`）、RequestVote 逻辑
- 目标：三节点进程能在 1 秒内选出 Leader，`kill -9` Leader 后能在 400 ms 内重新选出
