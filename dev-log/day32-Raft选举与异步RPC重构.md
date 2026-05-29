# Day 32 — Raft 选举层 / 异步 RPC 重构 / 指数退避 / 信号安全 / DoS 加固

> **本文撰写约束**：见 [dev-log/日志模板规范.md](dev-log/日志模板规范.md)。
> **教程性自检**：从 [HISTORY/day31/](HISTORY/day31/) 出发，按本文顺序粘贴代码块，应能重建 day32 状态。

---

## 0. 阅读指南：day31 → day32 改了什么

day31 我们落地了一套**同步短连接** RPC：`RpcServer + RpcClient + RpcMessage`，
目标只是给 Raft 节点之间互发 RequestVote / AppendEntries 提供一条管子。
它能跑，但**客户端是阻塞的**——`RpcClient::call()` 在 EventLoop 线程里执行，
一次 connect 失败（SYN 重传可达数秒）就会把整个 reactor 冻结。
RpcServer 的 handler 也是**同步的**——想在回调里读写 Raft 状态，
要么破坏 single-thread invariant，要么 promise 等待，本质还是阻塞。

day32 的核心任务：**把整条 RPC 路径反 reactor 化的部分修回来，再在其上搭 Raft 选举层。**

本日改动按"读者跟做"顺序分为以下几块：

| 顺序 | 改进 | 涉及代码 |
|------|------|----------|
| §2 | A：vendored `nlohmann/json` | `third_party/nlohmann/json.hpp` + CMake |
| §3 | B：`RpcMessage` 序列化切换为 nlohmann/json | `src/common/rpc/RpcMessage.cpp` |
| §4 | C：`RpcServer` Handler 改异步 Done-callback | `src/include/rpc/RpcServer.h` + `.cpp` |
| §5 | D：`AsyncRpcClient` 全异步长连接客户端 | `src/include/rpc/AsyncRpcClient.h` + `.cpp` |
| §6 | E：`ConnectBackoff` 指数退避 | `AsyncRpcClient.h`（用户追加） |
| §7 | F：`RaftTypes.h` 数据类型 + json 互转 | `src/include/raft/RaftTypes.h` |
| §8 | G：`RaftNode` Actor 模式 Raft 选举节点 | `src/include/raft/RaftNode.h` + `.cpp` |
| §9 | H：`SignalHandler` self-pipe 重写 | `src/include/net/SignalHandler.h` + `.cpp` |
| §10 | I：RPC 帧 length 上限 DoS 加固 | `RpcServer.cpp` / `AsyncRpcClient.cpp` |
| §11 | 日志降噪 + 演示程序 | `examples/src/raft_demo.cpp` 等 |
| §12 | 工程化：CMakeLists.txt | 接入 json、加 raft_demo |
| §13 | 验证：三终端启动 raft_demo | — |

---

## 1. 引言：day31 的三个坏味道

读完 [HISTORY/day31/src/common/rpc/RpcClient.cpp](HISTORY/day31/src/common/rpc/RpcClient.cpp)
与 [HISTORY/day31/src/common/rpc/RpcServer.cpp](HISTORY/day31/src/common/rpc/RpcServer.cpp)
后，三个问题暴露：

**坏味道①：客户端是同步阻塞短连接**

`RpcClient::call()` 内部 `socket → connect → write → read → close`，全栈阻塞。
想从 Raft 节点的 EventLoop 里"广播 RequestVote"，必须额外开线程池，否则
一次失败的 connect（peer 宕机时 SYN 重传可达数秒）就会把整个 reactor 卡死。
线程池线程数固定 → 网络抖动时超时占满所有线程，后续 RPC 全部排队。

**坏味道②：服务端 handler 是同步的**

旧版签名：`using Handler = std::function<std::string(const std::string&)>;`
要求 handler 必须**立刻返回响应字符串**。
但 Raft 的 `handleRequestVote` 要读写 `currentTerm_/votedFor_` 这些只属于
`loop_` 线程的状态——handler 被 sub-reactor 调用，不在 `loop_` 线程。
要么用锁（破坏 single-thread invariant），要么 promise 等待（本质阻塞 sub-reactor）。

**坏味道③：RpcMessage 手拼 JSON 字符串**

```cpp
// 旧版（来自 HISTORY/day31/src/common/rpc/RpcMessage.cpp）
json += "{\"method\":\"";
json += method;
json += "\",\"body\":";
json += payload;
json += "}";
```

只要 `method` 或 `payload` 里含双引号，这段代码就会生成非法 JSON。
Raft 需要发送结构化参数（term、candidateId 等），解码也要严格容错，
必须换成正经 JSON 库。

---

## 2. 改进 A — vendored `nlohmann/json` 单头

### 2.1 业务场景

`brew install nlohmann-json` 因 Homebrew portable-ruby 下载断流失败；
`FetchContent_Declare` 拉 GitHub 依赖网络稳定。
为可复现性 + 离线构建，把 v3.11.3 单头 `json.hpp`（≈898KB）直接 vendor 进仓库。

### 2.2 文件结构

```
third_party/
└── nlohmann/
    └── json.hpp     ← v3.11.3，一次性提交到仓库
```

在干净 day31 仓库里执行：

```bash
mkdir -p third_party/nlohmann
curl -sSL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
  -o third_party/nlohmann/json.hpp
```

### 2.3 CMake 接入

来自 [CMakeLists.txt](CMakeLists.txt)（相对 day31 新增）：

```cmake
# ── 第三方依赖：nlohmann/json（vendored 单头文件版本）──────────────────
set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_include_directories(NetLib
    PUBLIC
        $<BUILD_INTERFACE:${NLOHMANN_JSON_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)
```

（a）**为什么写它**：`target_include_directories` 加 `PUBLIC` 让 `NetLib` 的消费者
（`raft_demo`、测试等）也能 `#include <nlohmann/json.hpp>` 而无需再配置。

（b）**它怎么工作**：`$<BUILD_INTERFACE:...>` generator expression 保证只在本地构建时
暴露这个路径，不会写进 install 后的配置文件（避免绝对路径污染 `AiriConfig.cmake`）。

（c）**它和上下文怎么衔接**：改进 B~G 的所有 `#include <nlohmann/json.hpp>` 都依赖这行。

---

## 3. 改进 B — `RpcMessage` 序列化切换为 nlohmann/json

### 3.1 业务场景

改进 A 引入 json 库后，立刻用它重写 `RpcMessage` 的编解码，解决手拼字符串的注入风险。
帧格式（12 字节定长头 + JSON body）本身不变，改的是 body 的序列化方式。

### 3.2 新版编码实现

来自 [src/common/rpc/RpcMessage.cpp](src/common/rpc/RpcMessage.cpp)：

```cpp
std::string RpcMessage::encode() const {
    // 用 nlohmann::json 构造 body，彻底避免手拼字符串引入的 JSON 注入
    nlohmann::json j;
    j["method"] = method;
    j["body"]   = nlohmann::json::parse(payload); // payload 已是合法 JSON
    std::string json = j.dump();

    uint32_t payloadLen = static_cast<uint32_t>(json.size());
    uint32_t netLen     = htonl(payloadLen);
    uint32_t netType    = htonl(static_cast<uint32_t>(type));
    uint32_t netReqId   = htonl(reqId);

    std::string frame;
    frame.resize(12 + payloadLen);
    memcpy(frame.data() + 0, &netLen,   4);
    memcpy(frame.data() + 4, &netType,  4);
    memcpy(frame.data() + 8, &netReqId, 4);
    memcpy(frame.data() + 12, json.data(), payloadLen);
    return frame;
}
```

（a）**为什么写它**：旧版手拼字符串，payload 含特殊字符就生成非法 JSON。
（b）**它怎么工作**：`j["body"] = json::parse(payload)` 把 payload 作为已解析的 JSON 值嵌入，
再 `dump()` 重新序列化，保证整体合法。若 payload 本身非法 JSON，`parse()` 会抛异常。
（c）**它和上下文怎么衔接**：`encode()` 被 `RpcServer::onMessage` 和 `AsyncRpcClient::doCall` 调用。

### 3.3 新版解码实现（粘包处理核心）

来自 [src/common/rpc/RpcMessage.cpp](src/common/rpc/RpcMessage.cpp)：

```cpp
bool RpcMessage::decode(const char *data, int len,
                        RpcMessage *out, int *consumed) {
    if (len < 12) return false;

    uint32_t netLen, netType, netReqId;
    memcpy(&netLen,   data + 0, 4);
    memcpy(&netType,  data + 4, 4);
    memcpy(&netReqId, data + 8, 4);

    uint32_t payloadLen = ntohl(netLen);
    out->type   = static_cast<RpcMessage::Type>(ntohl(netType));
    out->reqId  = ntohl(netReqId);

    // 粘包处理：payload 字节数不足，等待更多数据
    if (len < 12 + static_cast<int>(payloadLen)) return false;

    std::string json(data + 12, payloadLen);
    try {
        auto j      = nlohmann::json::parse(json);
        out->method  = j.at("method").get<std::string>();
        out->payload = j.at("body").dump();
    } catch (...) {
        return false; // 非法 JSON，拒绝帧
    }

    *consumed = 12 + static_cast<int>(payloadLen);
    return true;
}
```

（a）**为什么写它**：旧版用手写 `findStr` 解析 JSON，脆弱且不能容错。
（b）**它怎么工作**：先检查头 12 字节，再检查 payload 是否到齐（粘包守卫），
最后用 `json::parse` 强解析，任何格式错误都被 `catch` 兜住并返回 `false`。
（c）**它和上下文怎么衔接**：`RpcServer::onMessage` 循环调用 `decode` 处理粘包/分包。


---

## 4. 改进 C — `RpcServer` Handler 改异步 Done-callback

### 4.1 业务场景：同步 Handler 为什么不能用在 Raft 里

旧版 RpcServer 的 handler 签名是：

```cpp
// 旧版（来自 HISTORY/day31/src/include/rpc/RpcServer.h）
using Handler = std::function<std::string(const std::string&)>;
```

handler 必须**立刻返回响应字符串**，整个处理都在 sub-reactor 线程上同步执行。

设想 Raft 节点收到 RequestVote 请求，需要：
1. 读取 `currentTerm_`（Raft 状态，只属于 `loop_` 线程）
2. 判断是否投票，修改 `votedFor_`
3. 返回 `{"voteGranted": true}`

这三步发生在 sub-reactor 线程，但 `currentTerm_/votedFor_` 只有 `loop_` 线程有权访问。
旧方案无解——只能用锁（破坏单线程 invariant）或 promise 同步等（本质阻塞 sub-reactor）。

**新方案**：handler 签名改成"接受请求，接受一个 done 回调"：

```
handler(reqJson, done)
  │
  └─ 把"处理逻辑 + done(result)"包成 lambda，投递到 loop_ 线程
     │  handler 立刻返回，sub-reactor 继续处理下一帧
     │
     └─ loop_ 线程执行 lambda，读写 Raft 状态，完成后调用 done(result)
        │
        └─ done 内部 runInLoop 回到 conn 的 owning loop，写出响应
```

### 4.2 接口变更

来自 [src/include/rpc/RpcServer.h](src/include/rpc/RpcServer.h)：

```cpp
class RpcServer {
  public:
    using Done    = std::function<void(std::string responseJson)>;
    using Handler = std::function<void(const std::string &req, Done done)>;

    RpcServer(const std::string &ip, uint16_t port, int ioThreads = 1);
    void addHandler(const std::string &method, Handler handler);
    void start();
    void stop();

  private:
    void onMessage(Connection *conn);
    void onNewConn(Connection *conn);

    TcpServer                                server_;
    std::unordered_map<std::string, Handler> handlers_;
};
```

（a）**为什么写它**：把响应的控制权从"立刻返回"变成"任意时刻调 done"，
彻底解耦 sub-reactor 的接收和 Raft loop_ 的处理。
（b）**`Done` 是什么**：它是一个闭包，内部已经捕获了把响应写回连接所需的一切
（`connLoop`、`aliveWeak`、`reqId`、`connPtr`）。handler 调用 `done(payload)` 时，
框架自动 `runInLoop` 回到正确的 IO 线程写出帧，调用方不用关心线程问题。
（c）**它和上下文怎么衔接**：`RaftNode::handleRequestVote` 就是这种新签名的 handler。

### 4.3 Done 回调的构造

来自 [src/common/rpc/RpcServer.cpp](src/common/rpc/RpcServer.cpp)（`onMessage` 中）：

```cpp
        Eventloop          *connLoop  = conn->getLoop();
        std::weak_ptr<bool> aliveWeak = conn->aliveFlag();
        const uint32_t      reqId     = msg.reqId;
        std::string         method    = msg.method;
        Connection         *connPtr   = conn;

        Done done = [connLoop, aliveWeak, reqId, method = std::move(method),
                     connPtr](std::string responsePayload) mutable {
            connLoop->runInLoop([connPtr, aliveWeak, reqId, method = std::move(method),
                                 payload = std::move(responsePayload)]() mutable {
                auto a = aliveWeak.lock();
                if (!a || !*a) return; // 连接已销毁，丢弃响应
                RpcMessage resp;
                resp.type    = RpcMessage::Type::kResponse;
                resp.reqId   = reqId;
                resp.method  = std::move(method);
                resp.payload = std::move(payload);
                connPtr->send(resp.encode());
            });
        };

        // handler 立刻返回；sub-reactor 继续解下一帧，永不阻塞。
        it->second(msg.payload, std::move(done));
```

（a）**为什么写它**：Done 需要在任意线程被调用，写回 conn 必须在 conn 的归属 loop 线程。
（b）**它怎么工作**：
- `connLoop->runInLoop(...)` 把写回动作切回 conn 的 IO 线程
- `aliveWeak.lock()` 判活：如果 done 被延迟调用，conn 可能已析构，此时安全丢弃
- `!*a` 二次判活：weak_ptr 有效但连接已被标记关闭

（c）**它和上下文怎么衔接**：`handler(msg.payload, std::move(done))` 调用后，
`done` 的生命周期转移到 handler（Raft 把它捕获到 lambda 里，随 runInLoop 延迟触发）。

### 4.4 全流程追踪：Node 1 收到 RequestVote 请求

**场景**：3 节点集群，Node 0 发起选举，Node 1（Follower）收到 RequestVote 帧。

#### 第 1 步：sub-reactor 线程解帧，onMessage 被调用

进入 `RpcServer::onMessage`。

实参快照：
```
msg.method  = "RequestVote"
msg.reqId   = 42
msg.payload = {"term":2,"candidateId":0,"lastLogIndex":0,"lastLogTerm":0}
```

- `ctx->buf` 拼入新到的字节
- `decode` 成功，得到 `msg`，`consumed=84`
- 找到 `handlers_["RequestVote"]`

#### 第 2 步：构造 Done 闭包，调用 handler 后立刻返回

`done` 闭包被构造，捕获 `connLoop=sub-reactor-loop`、`aliveWeak`、`reqId=42`、`connPtr`。

`it->second(msg.payload, std::move(done))` 被调用，即 `RaftNode::handleRequestVote`。

#### 第 3 步：handleRequestVote 投递 lambda 到 loop_ 线程，立刻返回

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)：

```cpp
    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        bool grant = false;
        if (
            args.term >= currentTerm_.load() &&
            (votedFor_ == -1 || votedFor_ == args.candidateId) &&
            (args.lastLogTerm > lastLogTerm() ||
             (args.lastLogTerm == lastLogTerm() && args.lastLogIndex >= lastLogIndex()))
        ) {
            grant     = true;
            votedFor_ = args.candidateId;
            resetElectionTimer();
        }

        RequestVoteReply reply{currentTerm_.load(), grant};
        done(json(reply).dump());
    });
```

sub-reactor 把 `done` 一同捕获进 lambda，然后立刻返回。此刻 sub-reactor 已可处理下一帧。

#### 第 4 步：loop_ 线程执行 lambda，安全读写 Raft 状态

实参代入：`args.term=2`，`currentTerm_=1`，`votedFor_=-1`

- `args.term > currentTerm_`？2 > 1？**是** → `becomeFollower(2)`
  - `currentTerm_=2`，`votedFor_=-1`，重置选举计时器
- 三个投票条件：
  - `args.term >= currentTerm_`？2 >= 2？**是**
  - `votedFor_ == -1`？**是**
  - `lastLogTerm == lastLogTerm()`？0 == 0 且 `lastLogIndex >= lastLogIndex()`？0 >= 0？**是**
- 全部满足 → `grant=true`，`votedFor_=0`
- `done({"term":2,"voteGranted":true})` 被调用

#### 第 5 步：Done 闭包触发，切回 sub-reactor 写帧

`connLoop->runInLoop(...)` 把写帧动作投入 sub-reactor 队列。
sub-reactor 下一轮事件循环执行：`aliveWeak.lock()` 有效，`*a=true`，写帧成功。

**系统状态快照（loop_ 线程）**：
```
currentTerm_  = 2
votedFor_     = 0 (Node 0)
state_        = Follower
electionEpoch = 旧值+1（被 resetElectionTimer 递增）
```


---

## 5. 改进 D — `AsyncRpcClient`：全异步长连接 RPC 客户端

这是 day32 改动量最大的单个模块，也是理解难度最高的地方。
按照「概念先行四步模板」展开。

### 5.1 步骤一：问题声明

旧版 `RpcClient::call()` 的执行路径：

```
Raft loop_ 线程
  │
  ├─ call("RequestVote", ...)
  │    ├─ socket()
  │    ├─ connect()        ← 阻塞，peer 宕机时 SYN 重传可达 75 秒
  │    ├─ write()          ← 阻塞，kernel 缓冲区满时挂起
  │    ├─ read()           ← 阻塞，等待响应
  │    └─ close()
  │
  └─ 这 75 秒里，loop_ 的 poller 无法执行
       → 无法接收心跳 → 触发不必要的重新选举
       → 无法处理其他 peer 的请求
       → 整个节点逻辑冻结
```

解法的核心思想：**把"发送 + 等待 + 超时"全部嫁接到 EventLoop 的 IO 多路复用上**，
让 `callAsync` 立刻返回，响应或超时通过回调在 `loop_` 线程触发。

具体来说，这个类解决了三个子问题：
1. 非阻塞 connect：`O_NONBLOCK + EINPROGRESS + Channel(EPOLLOUT) + getsockopt(SO_ERROR)`
2. 请求多路复用：同一连接上并发多个请求，用 `reqId` 路由回包
3. 超时不阻塞：用 `loop_->runAfter` 定时器，超时回调直接在 `loop_` 线程触发

### 5.2 步骤二：线程模型图

```
外部线程（任意，如 Raft loop_、其他线程）
────────────────────────────────────────────
callAsync(method, json, cb, timeoutMs)
    │
    └─ loop_->runInLoop(lambda)  ← 投递后立刻返回
                │
                ▼
         loop_ 线程（归属线程，所有状态读写在此）
         ─────────────────────────────────────────
         doCall()
           ├─ state_==kConnected? → conn_->send(frame), pending_[reqId]=cb
           ├─ state_==kIdle?      → startConnectLocked(), pendingFrames_.push_back(frame)
           └─ state_==kStopped?   → cb(false, "")

         startConnectLocked()
           ├─ socket() + O_NONBLOCK + connect()
           ├─ EINPROGRESS → connectChannel_(EPOLLOUT) + runAfter(3s, timeout)
           └─ 立即完成    → onConnected(fd)

         onConnectWritable()
           ├─ getsockopt(SO_ERROR)==0 → onConnected(fd)
           └─ err != 0 → close + kIdle + failAllPending

         onConnected()
           ├─ ++connectEpoch_    ← 让遗留 connect-timer 失效
           ├─ Connection(fd, loop_) + enableInLoop()
           └─ flushPendingFrames()

         onResponse()
           └─ decode → pending_[reqId].cb(true, resp)

stop() ─ queueInLoop ─▶ state_=kStopped, failAllPending, conn_.reset()
```

### 5.3 步骤三：数据成员生命周期表

来自 [src/include/rpc/AsyncRpcClient.h](src/include/rpc/AsyncRpcClient.h)（数据成员部分）：

| 分组 | 成员 | 类型 | 说明 |
|------|------|------|------|
| **不可变配置** | `loop_` | `Eventloop*` | 构造后不变；所有内部操作的归属 loop |
| **不可变配置** | `ip_`, `port_` | `string`, `uint16_t` | 目标 peer 地址，构造后不变 |
| **loop_ 独占状态** | `state_` | `enum State` | 连接状态机：kIdle→kConnecting→kConnected→kStopped |
| **loop_ 独占状态** | `conn_` | `unique_ptr<Connection>` | 已建立的长连接；nullptr 表示未连接 |
| **loop_ 独占状态** | `connectChannel_` | `unique_ptr<Channel>` | connect 进行中时临时持有的 Channel；连接完成后置 null |
| **loop_ 独占状态** | `connectFd_` | `int` | connect 阶段的 raw fd（onConnectWritable 用）；连接完成后被 Connection 接管 |
| **loop_ 独占状态** | `connectEpoch_` | `uint64_t` | 版本号；每次发起新 connect 时 ++；用于让遗留 connect-timer 失效 |
| **loop_ 独占状态** | `pending_` | `unordered_map<uint32_t, PendingCall>` | 进行中的请求：reqId → {callback, timerEpoch} |
| **loop_ 独占状态** | `pendingFrames_` | `deque<string>` | connect 进行中时缓存的待发帧；连接建立后 flush |
| **loop_ 独占状态** | `nextReqId_` | `uint32_t` | 单调递增的请求 ID 生成器 |
| **loop_ 独占状态** | `backoff_` | `ConnectBackoff` | 指数退避计时（见 §6） |

**理解关键**：`connectChannel_` 和 `conn_` 这两个字段是**不重叠的**：
- connect 阶段：持有 `connectChannel_`（注册 EPOLLOUT 等待 connect 完成），`conn_` 为 null
- 连接建立后：`connectChannel_` 被销毁，`conn_` 被创建（接管 fd）
- 类比：Channel 是"借基础设施用 fd"，Connection 是"把 fd 升格为有状态的连接对象"

### 5.4 步骤四：函数依赖层次表

| 层次 | 函数 | 被谁调用 | 核心职责 |
|------|------|---------|---------|
| 叶层 | `failAllPending(reason)` | stop / startConnect / handleConnectionClosed / onConnectWritable | 遍历 `pending_`，全部以 `ok=false` 触发回调，并清空 |
| 叶层 | `cleanupConnectChannel()` | onConnectWritable / connect-timer | disableAll + deleteChannel + reset connectChannel_；connectFd_=-1 |
| 叶层 | `completeWithTimeout(reqId, epoch)` | loop_->runAfter 回调 | 若 pending_[reqId].timerEpoch 匹配，以 ok=false 触发并从 pending_ 删除 |
| 叶层 | `flushPendingFrames()` | onConnected | 把 pendingFrames_ 队列里缓存的帧逐一发出 |
| 中层 | `startConnectLocked()` | doCall（kIdle 时）| 检查退避 → socket → O_NONBLOCK → connect → 处理三种结果 |
| 中层 | `onConnectWritable(fd, epoch)` | connectChannel_ 的 write 回调 | getsockopt(SO_ERROR)→0 则 onConnected；否则记录失败 |
| 中层 | `onConnected(fd)` | startConnectLocked（立即完成）/ onConnectWritable | ++epoch（让旧 timer 失效）→ 构造 Connection → enableInLoop → flushPendingFrames |
| 中层 | `onResponse(conn)` | Connection 的 onMessage 回调 | 解帧，按 reqId 路由到 pending_ 对应的回调 |
| 中层 | `handleConnectionClosed()` | Connection 的 deleteCallback（via queueInLoop） | conn_.reset() → state_=kIdle → failAllPending |
| 根层 | `stop()` | 外部（RaftNode::stop） | queueInLoop 切回 loop_ 线程：kStopped + failAllPending + conn_.reset |
| 根层 | `callAsync(...)` | 外部任意线程（RaftNode） | runInLoop 切回 loop_ 线程，再调 doCall |
| 根层 | `doCall(...)` | callAsync 内（loop_ 线程） | 分配 reqId + 注册超时 + 编帧 + 投递（kConnected 直发 / kIdle 触发 connect） |

### 5.5 编码实现（自底向上）

先从叶层开始，最后看根层。这样每读一个新函数，它调用的所有东西你都已经看过了。

#### 叶层：`failAllPending` 和 `completeWithTimeout`

来自 [src/common/rpc/AsyncRpcClient.cpp](src/common/rpc/AsyncRpcClient.cpp)（§5 错误路径）：

```cpp
void AsyncRpcClient::failAllPending(const char *reason) {
    // 注意：不能在遍历时修改 map，先拷贝出来再遍历触发
    auto tmp = std::move(pending_);
    for (auto &[id, pc] : tmp) {
        if (pc.cb) pc.cb(false, reason ? reason : "");
    }
}

void AsyncRpcClient::completeWithTimeout(uint32_t reqId, uint64_t timerEpoch) {
    auto it = pending_.find(reqId);
    if (it == pending_.end()) return;           // 已被正常回包，不重复触发
    if (it->second.timerEpoch != timerEpoch) return; // 旧 timer 触发，已被替换
    Callback cb = std::move(it->second.cb);
    pending_.erase(it);
    cb(false, "timeout");
}
```

（b）**怎么工作**：
- `failAllPending`：`std::move(pending_)` 先把 map 搬出来（清空 `pending_`），再遍历触发。
  这样即使回调里再次调用 `callAsync`（重入），新的 reqId 会进入空的 `pending_` 而不是脏数据。
- `completeWithTimeout`：双重守卫——"reqId 还在 pending_" 且 "epoch 匹配"，才触发超时回调。
  epoch 设计成 `reqId` 本身（每个 reqId 唯一），杜绝同一个请求被触发两次（正常回包后 timer 延迟触发的情况）。

#### 叶层：`cleanupConnectChannel`

来自 [src/common/rpc/AsyncRpcClient.cpp](src/common/rpc/AsyncRpcClient.cpp)：

```cpp
void AsyncRpcClient::cleanupConnectChannel() {
    if (connectChannel_) {
        connectChannel_->disableAll();
        loop_->deleteChannel(connectChannel_.get());
        connectChannel_.reset();
    }
    connectFd_ = -1;
}
```

（b）**怎么工作**：`disableAll()` 清除所有事件关注，`deleteChannel` 从 poller 注销，
`reset()` 析构 Channel 对象。`connectFd_` 置 -1 表示无进行中的 connect fd。
注意这里**不 close fd**——fd 的命运由调用方决定（成功时传给 Connection，失败时 close）。

#### 中层：`startConnectLocked`

来自 [src/common/rpc/AsyncRpcClient.cpp](src/common/rpc/AsyncRpcClient.cpp)（§3）：

```cpp
void AsyncRpcClient::startConnectLocked() {
    if (backoff_.inBackoff(nowSteadyMs())) {
        failAllPending("connect backoff");
        return;
    }
    state_ = State::kConnecting;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { /* ... */ state_ = State::kIdle; failAllPending("socket() failed"); return; }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    ::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (rc == 0) {
        onConnected(fd); // 极少见：loopback 立即完成
        return;
    }
    if (errno != EINPROGRESS) {
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] connect 立即失败 ...";
        ::close(fd); state_ = State::kIdle; failAllPending("connect failed");
        return;
    }
    // EINPROGRESS：等可写
    connectFd_               = fd;
    const uint64_t connEpoch = ++connectEpoch_;
    connectChannel_          = std::make_unique<Channel>(loop_, fd);
    connectChannel_->setWriteCallback([this, fd, connEpoch] {
        if (connEpoch != connectEpoch_) return;
        onConnectWritable(fd, connEpoch);
    });
    connectChannel_->enableWriting();
    loop_->runAfter(3.0, [this, connEpoch] {
        if (connEpoch != connectEpoch_) return;
        if (state_ != State::kConnecting) return;
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] connect 超时 ...";
        cleanupConnectChannel();
        state_ = State::kIdle;
        failAllPending("connect timeout");
    });
}
```

（b）**它怎么工作**（关键分支）：
- `O_NONBLOCK` 后 `connect()` 通常立刻返回 `EINPROGRESS`（"正在建立中"）
- 这时我们**不能直接判断连接是否成功**，需要注册 EPOLLOUT 等 socket 可写
- 可写时用 `getsockopt(SO_ERROR)` 查实际结果（0=成功，其他值=失败）
- `++connectEpoch_` 是关键：如果 3 秒内有新的 connect 发起（旧的先超时了），
  新的 epoch 值不同，旧的 write 回调会发现 `connEpoch != connectEpoch_` 而提前返回

#### 中层：`onConnectWritable` 和 `onConnected`

来自 [src/common/rpc/AsyncRpcClient.cpp](src/common/rpc/AsyncRpcClient.cpp)：

```cpp
void AsyncRpcClient::onConnectWritable(int fd, uint64_t /*connEpoch*/) {
    int       err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    cleanupConnectChannel();
    if (err != 0) {
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] async connect 失败 ...";
        ::close(fd);
        state_ = State::kIdle;
        failAllPending("async connect failed");
        return;
    }
    onConnected(fd);
}

void AsyncRpcClient::onConnected(int fd) {
    ++connectEpoch_; // 让任何遗留的 connect-timer 立刻失效
    backoff_.reset();
    state_ = State::kConnected;

    conn_ = std::make_unique<Connection>(fd, loop_);
    conn_->setDeleteConnectionCallback([this](int) {
        loop_->queueInLoop([this] { handleConnectionClosed(); });
    });
    conn_->setOnMessageCallback([this](Connection *c) { onResponse(c); });
    conn_->enableInLoop();

    flushPendingFrames();
}
```

（b）**`onConnected` 怎么工作**：
- `++connectEpoch_`：即使刚才 connect 已经成功，也递增 epoch，
  让任何还没触发的 connect-timer 回调检查 epoch 时发现不匹配而提前退出
- `backoff_.reset()`：连接成功，退避计数清零（详见 §6）
- `Connection(fd, loop_)` 把 raw fd 包装成有完整生命周期管理的连接对象
- `enableInLoop()` 让 poller 开始监听这个 fd 的 EPOLLIN
- `setDeleteConnectionCallback`：连接断开时的清理入口（注意 queueInLoop 异步化，
  避免在 Connection 自身的析构调用链上再 reset conn_，这会造成 UAF）

#### 叶层：`flushPendingFrames` 和 `onResponse`

来自 [src/common/rpc/AsyncRpcClient.cpp](src/common/rpc/AsyncRpcClient.cpp)（§3、§4）：

```cpp
void AsyncRpcClient::flushPendingFrames() {
    if (!conn_) return;
    while (!pendingFrames_.empty()) {
        conn_->send(std::move(pendingFrames_.front()));
        pendingFrames_.pop_front();
    }
}
```

```cpp
void AsyncRpcClient::onResponse(Connection *conn) {
    Buffer *buf = conn->getInputBuffer();
    std::string data(buf->peek(), buf->readableBytes());
    buf->retrieveAll();

    while (!data.empty()) {
        RpcMessage msg;
        int        consumed = 0;
        if (!RpcMessage::decode(data.data(), static_cast<int>(data.size()), &msg, &consumed))
            break;
        data.erase(0, consumed);

        if (msg.type != RpcMessage::Type::kResponse) continue;
        auto it = pending_.find(msg.reqId);
        if (it == pending_.end()) continue; // 超时后到达的迟到回包，直接丢弃
        Callback cb = std::move(it->second.cb);
        pending_.erase(it);
        cb(true, std::move(msg.payload));
    }
}
```

（b）**`onResponse` 怎么工作**：
- 把 Buffer 里的数据一次性拷贝出来（避免回调触发期间 Buffer 变化）
- 循环 decode，处理粘包：一次可能到多个响应帧
- `pending_.find(msg.reqId)`：如果找不到（超时已清理），直接丢弃这个迟到回包
- 找到则先 erase，再调回调（防止回调里递归触发再次处理）

#### 根层：`stop`、`callAsync`、`doCall`

来自 [src/common/rpc/AsyncRpcClient.cpp](src/common/rpc/AsyncRpcClient.cpp)（§1、§2）：

```cpp
void AsyncRpcClient::stop() {
    loop_->queueInLoop([this] {
        if (state_ == State::kStopped) return;
        state_ = State::kStopped;
        failAllPending("client stopped");
        cleanupConnectChannel();
        conn_.reset();
    });
}
```

```cpp
void AsyncRpcClient::callAsync(const std::string &method, const std::string &requestJson,
                               Callback cb, int timeoutMs) {
    loop_->runInLoop(
        [this, method, requestJson, cb = std::move(cb), timeoutMs]() mutable {
            doCall(method, requestJson, std::move(cb), timeoutMs);
        });
}

void AsyncRpcClient::doCall(const std::string &method, const std::string &requestJson,
                            Callback cb, int timeoutMs) {
    if (state_ == State::kStopped) { cb(false, ""); return; }

    const uint32_t reqId = ++nextReqId_;
    PendingCall    pc;
    pc.cb         = std::move(cb);
    pc.timerEpoch = reqId;
    pending_.emplace(reqId, std::move(pc));

    loop_->runAfter(timeoutMs / 1000.0,
                    [this, reqId] { completeWithTimeout(reqId, reqId); });

    RpcMessage msg;
    msg.type    = RpcMessage::Type::kRequest;
    msg.reqId   = reqId;
    msg.method  = method;
    msg.payload = requestJson;
    std::string frame = msg.encode();

    if (state_ == State::kConnected && conn_) {
        conn_->send(std::move(frame));
        return;
    }
    pendingFrames_.emplace_back(std::move(frame));
    if (state_ == State::kIdle) startConnectLocked();
}
```

（b）**`doCall` 的四个动作**：
1. **分配 reqId + 登记 pending**：为这次请求分配唯一 ID，保存回调
2. **注册超时定时器**：`timeoutMs` 后调 `completeWithTimeout`，若请求还在 pending_ 则触发 ok=false
3. **编码帧**：调 `RpcMessage::encode()` 得到二进制帧
4. **投递**：已连接就直接发；否则入队，如果是 kIdle 则触发 connect

**为什么 `stop()` 用 `queueInLoop` 而 `callAsync` 用 `runInLoop`？**
- `callAsync` 需要 reqId 立刻分配（调用方可能紧接着就想取消），`runInLoop` 保证尽快执行
- `stop()` 不需要立刻完成，只需保证在 loop_ 退出前跑完；`queueInLoop` 更安全
  （`runInLoop` 若在 loop_ 线程调用会立即执行，但 stop() 是从外部线程调用的，两者效果相同；
  用 `queueInLoop` 是更保守的选择）

### 5.6 全流程追踪

#### 路径 1：首次 callAsync，对端可达（kIdle → kConnecting → kConnected → 发帧 → 收回包）

**场景**：RaftNode（loop_ 线程）调 `client.callAsync("RequestVote", args_json, cb, 150)`。
此时 `state_=kIdle`，尚未建立连接。

**第 1 步：callAsync 切入 loop_ 线程**

`loop_->runInLoop(lambda)` 投递（若在 loop_ 线程调用，立即执行）。

**第 2 步：doCall 分配 reqId=1，注册 150ms 超时，入帧队列，触发 connect**

```
state_     = kIdle
pendingFrames_ = ["<编码好的帧>"]
pending_   = {1: {cb, epoch=1}}
→ startConnectLocked()
```

**第 3 步：startConnectLocked 发起非阻塞 connect**

```
socket(AF_INET, SOCK_STREAM) → fd=7
fcntl(7, F_SETFL, O_NONBLOCK)
connect(7, 127.0.0.1:18902) → errno=EINPROGRESS
→ state_ = kConnecting
→ connectFd_ = 7
→ connectEpoch_ = 1
→ connectChannel_ = Channel(loop_, 7)  [注册 EPOLLOUT]
→ loop_->runAfter(3.0, timer_epoch=1)  [3s 兜底超时]
```

函数返回。loop_ 继续处理其他事件（其余 peer 的 callAsync 等）。

**第 4 步：内核完成 TCP 握手，EPOLLOUT 触发，onConnectWritable**

```
getsockopt(7, SO_ERROR) = 0  → 连接成功
cleanupConnectChannel()      → connectChannel_.reset()
onConnected(7)
```

**第 5 步：onConnected 建立 Connection，flush 缓存帧**

```
++connectEpoch_ = 2   → 3s 兜底 timer 触发时 epoch=1≠2 → 自动放弃
backoff_.reset()
state_ = kConnected
conn_ = Connection(7, loop_)  [enableInLoop → poller 监听 EPOLLIN]
flushPendingFrames()           → conn_->send("<帧>")
pendingFrames_ = []
```

**第 6 步：对端写回响应帧，EPOLLIN 触发，onResponse**

```
decode 成功，msg.reqId=1
pending_.find(1) → 找到
cb(true, "{"term":2,"voteGranted":true}")
pending_.erase(1)
```

150ms 超时定时器仍在队列里，但 `completeWithTimeout(1, 1)` 执行时 `pending_.find(1)` 返回 end → 直接 return。

**最终状态**：
```
state_        = kConnected
conn_         = 有效（保持长连接，下次 callAsync 直接发）
pending_      = {}
pendingFrames_ = []
```

#### 路径 2：对端不可达，connect 超时（kIdle → kConnecting → 3s 后 kIdle）

**第 1 步**：同上，`startConnectLocked` 成功发起非阻塞 connect，`connectEpoch_=1`。

**第 2 步：3 秒过去，connect-timer 触发**

```
connEpoch=1 == connectEpoch_=1? 是
state_=kConnecting? 是
backoff_.recordFailure(nowMs) → durationMs_=500, failures_=1, 返回 true → 打 WARN
cleanupConnectChannel()
state_ = kIdle
failAllPending("connect timeout")
  → pending_[1].cb(false, "connect timeout")
  → pending_.erase(1)
```

`pending_` 里注册的 150ms 业务超时也已触发（发生在第 2.5 秒），
但此时 reqId=1 已被 failAllPending 清空，`completeWithTimeout` 找不到 → return。

**最终状态**：
```
state_        = kIdle
backoff_      = {untilMs_=now+500, durationMs_=500, failures_=1}
pending_      = {}
```

下次 callAsync 发起 connect 时，`backoff_.inBackoff(nowMs)` 会返回 true（在退避期），
直接 `failAllPending("connect backoff")` 而不尝试 TCP——这就是 §6 ConnectBackoff 的作用。

#### 路径 3：连接建立后，连接断开（kConnected → kIdle）

**第 1 步**：对端关闭连接，EPOLLIN 触发，`Connection::handleRead()` 读到 EOF。

**第 2 步**：`Connection::close()` 触发 `deleteConnectionCallback`

```cpp
conn_->setDeleteConnectionCallback([this](int) {
    loop_->queueInLoop([this] { handleConnectionClosed(); });
});
```

**注意**：`setDeleteConnectionCallback` 里用 `queueInLoop` 而不是直接调。
因为 `deleteConnectionCallback` 是在 `Connection::close()` 的调用栈上触发的，
此时 `conn_` 还存在。如果直接 `handleConnectionClosed()` 里 `conn_.reset()`，
就会在 `Connection` 自身的调用栈上析构自己（UAF）。
`queueInLoop` 让 `handleConnectionClosed` 在当前调用链返回后才执行。

**第 3 步**：handleConnectionClosed

```cpp
void AsyncRpcClient::handleConnectionClosed() {
    conn_.reset();         // 此时已不在 Connection 的调用栈上，安全
    state_ = State::kIdle;
    failAllPending("connection closed");
}
```


---

## 6. 改进 E — `ConnectBackoff`：指数退避防连接风暴

> 这是用户在 day32 完成后自行追加的改进。

### 6.1 业务场景

路径 2 中，对端宕机后每次 callAsync 都立刻发起 TCP connect，
3 秒后超时再立刻发起下一次——对长时间宕机的 peer 会持续刷 SYN 包。

设想 3 节点集群，Node 1 宕机 10 分钟：
- 无退避：Node 0 和 Node 2 每 3 秒一次 connect 尝试 → 200 次无用 SYN
- 有指数退避：第 1 次失败等 500ms，第 2 次等 1s，第 3 次等 2s ... 上限 30s
  → 30s 后每分钟约 2 次，而不是 20 次

### 6.2 数据结构

来自 [src/include/rpc/AsyncRpcClient.h](src/include/rpc/AsyncRpcClient.h)：

```cpp
    struct ConnectBackoff {
        static constexpr int64_t kInitMs = 500;
        static constexpr int64_t kMaxMs  = 30'000;
        int64_t untilMs_   {0};
        int64_t durationMs_{0};
        int     failures_  {0};
        bool inBackoff(int64_t nowMs) const { return nowMs < untilMs_; }
        // 记录一次失败，指数增长退避时长；首次失败返回 true（建议打 WARN）
        bool recordFailure(int64_t nowMs) {
            durationMs_ = (durationMs_ == 0) ? kInitMs
                        : std::min(durationMs_ * 2, kMaxMs);
            untilMs_    = nowMs + durationMs_;
            return (failures_++ == 0);
        }
        void reset() { untilMs_ = 0; durationMs_ = 0; failures_ = 0; }
    };
```

（a）**为什么写它**：把退避逻辑封装成独立结构体，避免在 `startConnectLocked` 里散落多个时间变量。
（b）**怎么工作**：
- `durationMs_` 初始为 0，第一次失败设为 500ms，之后每次翻倍直到 30s
- `untilMs_` = 下一次允许 connect 的最早时刻
- `recordFailure` 返回 `(failures_++ == 0)` —— 只有**第一次连续失败**返回 true，
  外层代码据此决定是否打 WARN（否则对长期宕机的节点会频繁刷日志）
- `reset()` 在 `onConnected` 里调用，连接成功即清零所有退避状态

（c）**接入点**：

| 位置 | 动作 |
|------|------|
| `startConnectLocked()` 开头 | `inBackoff()` 为真 → 直接 failAllPending，不尝试 connect |
| `startConnectLocked()`，connect 立即失败 | `recordFailure()` |
| `startConnectLocked()`，connect 超时（3s timer） | `recordFailure()` |
| `onConnectWritable()`，SO_ERROR 非零 | `recordFailure()` |
| `onConnected()` | `reset()`，连接成功清零 |

### 6.3 全流程追踪：第二次 connect 在退避期内被拒

**前提**：第一次 connect 超时，`backoff_={untilMs_=T+500, durationMs_=500, failures_=1}`。

**T+100ms**：第二次 callAsync 触发 doCall → `startConnectLocked()`

```
backoff_.inBackoff(T+100) → T+100 < T+500 → true
failAllPending("connect backoff")
return   ← 不发起 TCP connect
```

**T+600ms**：第三次 callAsync 触发 `startConnectLocked()`

```
backoff_.inBackoff(T+600) → T+600 < T+500 → false
正常发起 connect
```

若第三次也失败：`recordFailure()` → `durationMs_=1000`，下次要等 1 秒。

---

## 7. 改进 F — `RaftTypes.h`：数据类型 + JSON 互转

### 7.1 业务场景

Raft 协议需要 5 种结构体在节点间序列化传输：
`LogEntry`、`RequestVoteArgs`、`RequestVoteReply`、`AppendEntriesArgs`、`AppendEntriesReply`。

手写每种结构体的 `to_json`/`from_json` 很繁琐，nlohmann/json 提供了
`NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` 宏，一行搞定。

### 7.2 完整实现

来自 [src/include/raft/RaftTypes.h](src/include/raft/RaftTypes.h)：

```cpp
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace raft {

enum class State { Follower, Candidate, Leader };

struct LogEntry {
    uint64_t    term{0};
    std::string cmd;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LogEntry, term, cmd)

struct RequestVoteArgs {
    uint64_t term{0};
    int      candidateId{-1};
    uint64_t lastLogIndex{0};
    uint64_t lastLogTerm{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RequestVoteArgs, term, candidateId, lastLogIndex, lastLogTerm)

struct RequestVoteReply {
    uint64_t term{0};
    bool     voteGranted{false};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RequestVoteReply, term, voteGranted)

// AppendEntries（Day32 只用心跳；Day33 会补 prevLogIndex / entries 等字段）
struct AppendEntriesArgs {
    uint64_t term{0};
    int      leaderId{-1};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesArgs, term, leaderId)

struct AppendEntriesReply {
    uint64_t term{0};
    bool     success{false};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesReply, term, success)

inline const char* stateName(State s) {
    switch (s) {
        case State::Follower:  return "Follower";
        case State::Candidate: return "Candidate";
        case State::Leader:    return "Leader";
    }
    return "?";
}

} // namespace raft
```

（a）**为什么写它**：把所有 Raft 消息类型集中在一个头文件，`RaftNode.cpp` 和日后的测试都只需包含这一个头。
（b）**`NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` 怎么工作**：这个宏展开后生成两个
自由函数 `to_json(j, obj)` 和 `from_json(j, obj)`，注册到 nlohmann/json 的 ADL 扩展点。
之后 `json(args).dump()` 就能序列化，`json::parse(str).get<T>()` 就能反序列化，
无需修改结构体本身（Non-Intrusive 的含义）。
（c）**它和上下文怎么衔接**：`RaftNode.cpp` 里所有的 `json(args).dump()` 和
`json::parse(reqJson).get<RequestVoteArgs>()` 都依赖这里定义的宏展开。

---

## 8. 改进 G — `RaftNode`：Actor 模式 Raft 选举节点

这是 day32 的核心新功能。同样按概念先行四步模板展开。

### 8.1 步骤一：问题声明——我们在实现什么

Raft 是一个分布式共识算法，解决的核心问题是：
**多台机器如何对"谁是 Leader"这件事达成一致？**

三节点集群启动时，每个节点都是 Follower，等待 Leader 的心跳。
如果在随机超时（150~300ms）内没收到心跳，Follower 认为 Leader 失联，
转为 Candidate 并发起选举：自增 term、给自己投票、向所有 peer 广播 RequestVote。
如果收到超过半数（quorum）的投票，就成为 Leader，开始每 50ms 广播一次心跳，
压制其余节点的选举计时器。

**这个类要实现的**：上述状态机，用本项目的 EventLoop 做异步 IO 底座，不引入额外线程池。

### 8.2 步骤二：两线程架构图

```
┌─────────────────────────────────────────────────────────────────┐
│  loopThread_（Raft 状态机 + 出站 RPC IO）                       │
│                                                                 │
│  loop_.loop()                                                   │
│    ├─ resetElectionTimer()    ← 启动时注册，收到心跳时重置      │
│    ├─ runEvery(50ms) → heartbeatTick()                          │
│    ├─ AsyncRpcClient IO 回调（onConnect / onResponse / 等）     │
│    └─ runInLoop 投递来的 Raft 状态读写（handleRequestVote 等）  │
│                                                                 │
│  数据成员（loop_ 线程独占）：                                   │
│    currentTerm_(atomic)  votedFor_  log_  state_(atomic)        │
│    electionEpoch_  currentElectionVotes_  leaderId_             │
│    peerClients_  rng_                                           │
└─────────────────────────────────────────────────────────────────┘
         ↑ runInLoop (处理 inbound RPC)
         │ queueInLoop (stop 等)
┌─────────────────────────────────────────────────────────────────┐
│  rpcServerThread_（接收入站 RPC）                               │
│                                                                 │
│  rpcServer_.start()                                             │
│    └─ TcpServer（sub-reactor）                                  │
│         └─ onMessage → handler(req, done)                       │
│              ├─ handleRequestVote  → loop_.runInLoop(lambda)    │
│              └─ handleAppendEntries → loop_.runInLoop(lambda)   │
└─────────────────────────────────────────────────────────────────┘
```

两个线程的分工：
- `loopThread_`：**Raft 状态机的唯一所有者**。所有状态读写都在这里。
- `rpcServerThread_`：**仅负责接收并路由 inbound RPC**。它不读写任何 Raft 状态，
  只把请求和 `done` 回调一起打包投入 `loop_` 队列，立刻返回。

这个设计消除了所有需要锁的地方：Raft 状态永远只在一个线程上跑。

### 8.3 步骤三：数据成员生命周期表

| 分组 | 成员 | 说明 |
|------|------|------|
| **不可变配置** | `id_` `peers_` `quorum_` | 构造时确定，之后只读 |
| **不可变配置** | `rng_` | 线程本地，仅 loop_ 线程调用，实际是"loop_ 独占"状态 |
| **loop_ 独占状态** | `votedFor_` `log_` `leaderId_` `currentElectionVotes_` | 只在 loop_ 线程读写 |
| **loop_ 独占状态** | `electionEpoch_` | 版本号；每次 resetElectionTimer 递增 |
| **loop_ 独占状态** | `peerClients_` | 每个 peer 一个 AsyncRpcClient，仅 loop_ 线程访问 |
| **对外原子快照** | `currentTerm_` `state_` | atomic；外部线程可读（如 raft_demo 轮询状态展示） |
| **基础设施** | `loop_` `loopThread_` `running_` | 生命周期管理 |
| **基础设施** | `rpcServer_` `rpcServerThread_` | inbound RPC 接收 |

### 8.4 步骤四：函数依赖层次表

| 层次 | 函数 | 被谁调用 | 核心职责 |
|------|------|---------|---------|
| 叶层 | `lastLogIndex()` `lastLogTerm()` | handleRequestVote, startElection | 读 log_ 末尾，返回 index/term |
| 叶层 | `getOrCreateClient(peer)` | startElection, heartbeatTick | lazy 构造对应 peer 的 AsyncRpcClient |
| 中层 | `becomeFollower(term)` | handleRequestVote/AppendEntries, onVoteReply, onHeartbeatReply | 降级：更新 term, votedFor_=-1, resetElectionTimer |
| 中层 | `becomeCandidate()` | electionTimerFired | 升级为候选人：++term, 自投票, votes=1 |
| 中层 | `becomeLeader()` | onVoteReply（票数达 quorum） | 升级为 Leader：++epoch（停选举计时器）, 立刻广播心跳 |
| 中层 | `resetElectionTimer()` | becomeFollower, becomeCandidate, handleRequestVote（投票后）, handleAppendEntries（合法心跳） | ++electionEpoch, runAfter(150~300ms) |
| 中层 | `electionTimerFired(epoch)` | loop_->runAfter 回调 | epoch 守卫 → becomeCandidate → startElection |
| 中层 | `startElection()` | electionTimerFired | 广播 RequestVote 到所有 peer（AsyncRpcClient） |
| 中层 | `onVoteReply(electionTerm, peerId, ok, reply)` | callAsync 的回调 | 过滤旧回包 → 累积票数 → 达 quorum 则 becomeLeader |
| 中层 | `heartbeatTick()` | loop_->runEvery(50ms) | 仅 Leader 广播 AppendEntries（心跳） |
| 中层 | `onHeartbeatReply(peerId, ok, reply)` | callAsync 的回调 | 检查 reply.term，发现更高 term 则退位 |
| 中层 | `handleRequestVote(req, done)` | rpcServer_ 的 handler | 解析 args → loop_.runInLoop → 三条件判断 → done(reply) |
| 中层 | `handleAppendEntries(req, done)` | rpcServer_ 的 handler | 解析 args → loop_.runInLoop → term 检查 → done(reply) |
| 根层 | `start()` | 外部（raft_demo） | 启动 loopThread_ + rpcServerThread_ |
| 根层 | `stop()` | 外部（raft_demo / ~RaftNode） | 停 rpcServer_ → join → 清空 peerClients_ → loop_.setQuit → join loopThread_ |

### 8.5 编码实现

#### 构造函数：哨兵条目 + handler 注册

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§1）：

```cpp
RaftNode::RaftNode(int id, std::vector<Peer> peers, uint16_t rpcPort)
    : id_(id),
      peers_(std::move(peers)),
      quorum_(static_cast<int>(peers_.size()) / 2 + 1),
      rng_(std::random_device{}()),
      rpcServer_("0.0.0.0", rpcPort, /*ioThreads=*/1) {
    // 哨兵条目：让 lastLogIndex() 和 lastLogTerm() 在日志为空时也能安全返回。
    log_.push_back(LogEntry{0, ""});

    rpcServer_.addHandler(
        "RequestVote",
        [this](const std::string &req, RpcServer::Done done) {
            handleRequestVote(req, std::move(done));
        });
    rpcServer_.addHandler(
        "AppendEntries",
        [this](const std::string &req, RpcServer::Done done) {
            handleAppendEntries(req, std::move(done));
        });
}
```

（b）**哨兵条目**：`log_[0] = {term=0, cmd=""}`。
`lastLogIndex()` 返回 `log_.size()-1`，日志为空时返回 0。
`lastLogTerm()` 返回 `log_.back().term`，日志为空时返回 0。
真实日志条目的 term >= 1，不会与哨兵混淆。没有哨兵，空日志时 `log_.back()` 是 UB。

（b）**quorum 计算**：`peers_.size()/2 + 1`——这里 `peers_` 包含自己。
3 节点：quorum=2；5 节点：quorum=3。即使 1/2 个节点宕机仍可选出 Leader。

#### start 和 stop

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§1）：

```cpp
void RaftNode::start() {
    if (running_.exchange(true)) return;

    loopThread_ = std::thread([this] {
        loop_.runInLoop([this] { resetElectionTimer(); });
        loop_.runEvery(0.05, [this] { heartbeatTick(); });
        loop_.loop();
    });

    rpcServerThread_ = std::thread([this] { rpcServer_.start(); });
}

void RaftNode::stop() {
    if (!running_.exchange(false)) return;

    rpcServer_.stop();
    if (rpcServerThread_.joinable()) rpcServerThread_.join();

    loop_.queueInLoop([this] { peerClients_.clear(); });
    loop_.setQuit();
    loop_.wakeup();
    if (loopThread_.joinable()) loopThread_.join();
}
```

（b）**stop 的顺序**：
1. 先停 rpcServer_（这样不会再有新的 done 回调被触发）
2. 把 peerClients_ 的清理投入 loop_（AsyncRpcClient 的析构必须在 loop_ 线程）
3. setQuit + wakeup 让 loop_ 退出，但在退出前会 doPendingFunctors() 执行第 2 步的清理
4. join loopThread_（等待 loop_ 真正退出）

**为什么 peerClients_ 清理要投入 loop_？**
`AsyncRpcClient` 持有 `Connection` 和 `Channel`，这两个对象的析构需要在归属 loop_ 线程
（`loop_->deleteChannel` 是 poller 操作，必须在 loop_ 线程调用）。

#### 角色切换三函数

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§3）：

```cpp
void RaftNode::becomeFollower(uint64_t term) {
    state_.store(State::Follower);
    currentTerm_.store(term);
    votedFor_ = -1;
    resetElectionTimer();
}

void RaftNode::becomeCandidate() {
    currentTerm_.store(currentTerm_.load() + 1);
    state_.store(State::Candidate);
    votedFor_             = id_;  // 给自己投一票
    currentElectionVotes_ = 1;
    resetElectionTimer(); // 如果这轮平票，超时后再发起下一轮
}

void RaftNode::becomeLeader() {
    state_.store(State::Leader);
    leaderId_ = id_;
    ++electionEpoch_; // 让所有未触发的 electionTimerFired 失效
    heartbeatTick();  // 立刻广播心跳，不等 50ms runEvery
}
```

（b）**becomeCandidate 为什么先递增 term？**
这防止网络延迟导致的旧投票响应污染新选举。
旧回包的 term 是上一轮的值，在 `onVoteReply` 里会因为 `currentTerm_ != electionTerm` 被丢弃。

（b）**becomeLeader 为什么立刻调 heartbeatTick？**
新当选的 Leader 的第一个 50ms runEvery 窗口可能要等 0~50ms。
在这段时间内，Follower 的选举计时器可能到期，触发竞争选举。
立刻广播一次心跳可以在这个窗口内压制所有 Follower。

#### resetElectionTimer 和 electionTimerFired

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§3）：

```cpp
void RaftNode::resetElectionTimer() {
    ++electionEpoch_;
    uint64_t myEpoch = electionEpoch_;
    int timeoutMs = std::uniform_int_distribution<int>(150, 300)(rng_);
    loop_.runAfter(timeoutMs / 1000.0,
                   [this, myEpoch] { electionTimerFired(myEpoch); });
}

void RaftNode::electionTimerFired(uint64_t epoch) {
    if (epoch != electionEpoch_) return; // 已被新的 reset 覆盖
    if (state_.load() == State::Leader) return; // Leader 不发起选举
    becomeCandidate();
    startElection();
}
```

（b）**epoch 机制**：EventLoop 的定时器无法主动取消（`runAfter` 没有返回 handle 用于取消）。
用版本号代替取消：每次 `resetElectionTimer` 都 `++electionEpoch_`，旧的定时器触发时
`epoch != electionEpoch_` 直接 return。
这是"软取消"模式——定时器还会触发，但 callback 是幂等的。

（b）**为什么随机化 150~300ms？**
如果所有节点超时相同，它们会在同一时刻都发起选举，导致无休止的平票。
随机化后，最快超时的节点抢先发起，其余节点收到其 RequestVote 后重置计时器，
大概率只有一个节点真正进入 Candidate 状态。


#### startElection 和 onVoteReply

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§4）：

```cpp
void RaftNode::startElection() {
    uint64_t term     = currentTerm_.load();
    uint64_t lastIdx  = lastLogIndex();
    uint64_t lastTerm = lastLogTerm();

    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        RequestVoteArgs args{term, id_, lastIdx, lastTerm};
        std::string     reqJson = json(args).dump();
        int             peerId  = peer.id;

        AsyncRpcClient *client = getOrCreateClient(peer);
        client->callAsync("RequestVote", reqJson,
                          [this, term, peerId](bool ok, std::string respJson) {
                              RequestVoteReply reply{};
                              if (ok) {
                                  try { reply = json::parse(respJson).get<RequestVoteReply>(); }
                                  catch (...) { ok = false; }
                              }
                              onVoteReply(term, peerId, ok, reply);
                          },
                          /*timeoutMs=*/150);
    }
}

void RaftNode::onVoteReply(uint64_t electionTerm, int peerId, bool ok, RequestVoteReply reply) {
    // 过滤旧回包：state_!=Candidate 说明已退位；term 变了说明是旧选举的回包
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) return;
    if (!ok) return;
    if (reply.term > currentTerm_.load()) { becomeFollower(reply.term); return; }
    if (reply.voteGranted) {
        ++currentElectionVotes_;
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}
```

（b）**startElection 的关键设计**：
- `getOrCreateClient(peer)` lazy 构建 AsyncRpcClient（不预先建连，避免启动时大量 connect 风暴）
- lambda 回调捕获 `term`（发起选举时的 term），在 `onVoteReply` 里和 `currentTerm_` 对比，过滤迟到回包

（b）**onVoteReply 的过期过滤**：
双重守卫缺一不可：
- `state_ != Candidate`：在等待回包期间收到了更高 term 的消息，已退回 Follower，放弃计票
- `currentTerm_ != electionTerm`：网络延迟，旧选举的回包才到，不能累积到新选举的票数里

#### heartbeatTick 和 onHeartbeatReply

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§5）：

```cpp
void RaftNode::heartbeatTick() {
    if (state_.load() != State::Leader) return; // Follower/Candidate 零开销退出
    uint64_t term = currentTerm_.load();

    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        AppendEntriesArgs args{term, id_};
        std::string       reqJson = json(args).dump();
        int               peerId  = peer.id;

        AsyncRpcClient *client = getOrCreateClient(peer);
        client->callAsync("AppendEntries", reqJson,
                          [this, peerId](bool ok, std::string respJson) {
                              AppendEntriesReply reply{};
                              if (ok) {
                                  try { reply = json::parse(respJson).get<AppendEntriesReply>(); }
                                  catch (...) { ok = false; }
                              }
                              onHeartbeatReply(peerId, ok, reply);
                          },
                          /*timeoutMs=*/100);
    }
}

void RaftNode::onHeartbeatReply(int /*peerId*/, bool ok, AppendEntriesReply reply) {
    if (!ok) return;
    // 发现更高 term：说明集群已选出更新任期的 Leader，自己是僵尸 Leader
    if (reply.term > currentTerm_.load()) becomeFollower(reply.term);
}
```

（b）**heartbeatTick 的自检**：`runEvery(50ms)` 每 50ms 无条件触发回调，
但第一行就检查 `state_ != Leader`，Follower/Candidate 直接 return，零开销。
这比"成为 Leader 时注册、退位时取消"的设计更简单——不需要管理心跳 timer 的生命周期。

（b）**onHeartbeatReply 的脑裂防护**：
如果网络分区期间另一个分区选出了新 Leader（更高 term），
旧 Leader 的心跳回包里会携带新 term，`becomeFollower` 立刻退位。
避免两个 Leader 同时认为自己有效（脑裂）。

### 8.6 全流程追踪

#### 路径 1：三节点集群，Node 2 最先超时，成为 Leader

**初始状态**（三节点刚 start）：
```
Node 0: Follower, term=0, electionTimeout=~273ms
Node 1: Follower, term=0, electionTimeout=~201ms
Node 2: Follower, term=0, electionTimeout=~156ms  ← 最快到期
```

**T+156ms：Node 2 的选举计时器触发**

`electionTimerFired(epoch=1)` → `epoch==electionEpoch_`，`state_=Follower`
→ `becomeCandidate()`：
```
currentTerm_ = 1
state_       = Candidate
votedFor_    = 2
currentElectionVotes_ = 1
resetElectionTimer()   → electionEpoch_=2，再注册一个 ~230ms 超时
```
→ `startElection()`：向 Node 0 和 Node 1 各发一次 callAsync("RequestVote", {term:1, ...})

**T+156ms+δ：Node 0 的 sub-reactor 收到 RequestVote 帧**

`handleRequestVote(req={term:1,candidateId:2,...}, done)` 在 Node 0 的 rpcServerThread_ 被调。
→ `loop_.runInLoop(lambda)`，立刻返回。
→ Node 0 的 loop_ 执行 lambda：
```
args.term=1 > currentTerm_=0 → becomeFollower(1)
  currentTerm_=1, votedFor_=-1, resetElectionTimer → electionEpoch_=2
三个投票条件全满足 → grant=true, votedFor_=2
done({"term":1,"voteGranted":true})
```
→ done 里 `connLoop->runInLoop`，把响应写回 Node 2。

**Node 1 类似**，也投票给 Node 2。

**T+156ms+RTT：Node 2 的 callAsync 回调触发**

`onVoteReply(electionTerm=1, peerId=0, ok=true, {voteGranted:true})`
```
state_=Candidate? 是  currentTerm_=1==electionTerm=1? 是
reply.term=1 <= currentTerm_=1? 是（不退位）
voteGranted=true → currentElectionVotes_=2
2 >= quorum_=2 → becomeLeader()
```
→ `becomeLeader()`：`state_=Leader`，`++electionEpoch_`，立刻调 `heartbeatTick()`

**最终状态**：
```
Node 2: Leader,   term=1
Node 0: Follower, term=1, votedFor_=2
Node 1: Follower, term=1, votedFor_=2
```

#### 路径 2：Leader 持续广播心跳，维持统治

每 50ms，Node 2 的 `heartbeatTick()` 向 Node 0 和 Node 1 各发一次
`callAsync("AppendEntries", {term:1, leaderId:2}, cb, 100ms)`。

Node 0 和 Node 1 各自在 `handleAppendEntries` 里：

```cpp
if (args.term >= currentTerm_.load()) {
    state_.store(State::Follower);
    leaderId_ = args.leaderId;
    success   = true;
    resetElectionTimer(); // 关键：心跳到达 → 重置选举计时器 → Follower 不发起选举
}
```

`resetElectionTimer()` 每次心跳都把选举超时推迟 150~300ms。
只要心跳间隔（50ms）< 选举超时（150~300ms），Follower 的计时器就永远不会到期。
Leader 用心跳维持统治的核心机制就在这里。

---

## 9. 改进 H — `SignalHandler` self-pipe 重写

### 9.1 业务场景

旧版 `SignalHandler` 直接在 OS 信号处理函数里执行 `handlers_[sig]()`。
问题：`std::map::operator[]` 会在 key 不存在时 `malloc`（不在 async-signal-safe 白名单），
`std::function::operator()` 同样不安全。
生产中表现为：偶发崩溃、收到未注册信号直接 terminate。

修复方案：**经典 self-pipe trick**——
信号处理函数只做一件事：把信号编号 write 到管道（`write` 是 async-signal-safe）。
一个后台 dispatcher 线程 read 管道，在正常线程上下文中调用注册的回调。

### 9.2 接口

来自 [src/include/net/SignalHandler.h](src/include/net/SignalHandler.h)：

```cpp
class Signal {
  public:
    DISALLOW_COPY_AND_MOVE(Signal)
    static void signal(int sig, std::function<void()> handler);
  private:
    Signal() = delete;
};
```

接口与旧版完全兼容，调用方零改动：

```cpp
// raft_demo.cpp 中
Signal::signal(SIGINT,  [&node] { node.stop(); });
Signal::signal(SIGTERM, [&node] { node.stop(); });
```

（b）**为什么不需要改调用方**：接口签名不变，旧版用的是 `Signal::signal`，新版同名。
内部实现从"直接调用"改成"pipe 转发 + dispatcher 线程"，对外透明。

---

## 10. 改进 I — RPC 帧 length 上限 DoS 加固

### 10.1 业务场景

旧版 `RpcServer::onMessage` 直接 `decode`，恶意客户端可以构造
`length=4294967295`（4GB）的帧头，让服务端分配 4GB 缓冲区，造成 OOM DoS。

### 10.2 防御代码

来自 [src/common/rpc/RpcServer.cpp](src/common/rpc/RpcServer.cpp)（`onMessage` 中）：

```cpp
namespace {
constexpr uint32_t kMaxRpcPayloadBytes = 16u * 1024u * 1024u; // 16 MiB
} // namespace

// onMessage 里 decode 之前：
        if (ctx->buf.size() >= 4) {
            uint32_t netLen = 0;
            std::memcpy(&netLen, ctx->buf.data(), 4);
            uint32_t payloadLen = ntohl(netLen);
            if (payloadLen > kMaxRpcPayloadBytes) {
                LOG_WARN << "[RpcServer] 拒绝过大帧并关闭连接 fd=" << conn->getSocket()->getFd()
                         << " claimed=" << payloadLen << " limit=" << kMaxRpcPayloadBytes;
                conn->close();
                return;
            }
        }
```

同样的 `kMaxRpcPayloadBytes` 也在 `AsyncRpcClient.cpp` 里检查入站响应帧：

```cpp
namespace {
constexpr uint32_t kMaxRpcPayloadBytes = 16u * 1024u * 1024u;
} // namespace
```

（b）**为什么 16 MiB**：足够容纳 Raft 的日志条目（Day32 只有心跳，几十字节），
但小到能防止单帧 OOM 攻击。Day33 引入快照时会重新评估这个上限。

---

## 11. 日志降噪 + 演示程序调整

### 11.1 Connection / TcpServer 日志降噪

每次连接建立/断开都会打印 DEBUG 级别日志，三节点集群频繁重连时噪音很大。
`raft_demo.cpp` 启动时设置：

```cpp
Logger::setLogLevel(Logger::INFO);
```

来自 [examples/src/raft_demo.cpp](examples/src/raft_demo.cpp)。

### 11.2 raft_demo 演示程序

来自 [examples/src/raft_demo.cpp](examples/src/raft_demo.cpp)（完整主体）：

```cpp
int main(int argc, char **argv) {
    int myId  = -1;
    int nodes = 3;

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--id") == 0)    myId  = std::stoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--nodes") == 0) nodes = std::stoi(argv[i + 1]);
    }

    if (nodes < 2 || nodes > 10) { std::cerr << "错误：--nodes 必须在 2~10 范围内\n"; return 1; }
    if (myId < 0 || myId >= nodes) { /* 打印用法 */ return 1; }

    Logger::setLogLevel(Logger::INFO);

    const std::vector<raft::Peer> allPeers = {
        {0, "127.0.0.1", 18901}, {1, "127.0.0.1", 18902}, {2, "127.0.0.1", 18903},
        // ... 最多 10 个
    };

    std::vector<raft::Peer> clusterPeers(allPeers.begin(), allPeers.begin() + nodes);
    raft::RaftNode node(myId, clusterPeers, allPeers[myId].port);
    node.start();

    Signal::signal(SIGINT,  [&node] { node.stop(); std::exit(0); });
    Signal::signal(SIGTERM, [&node] { node.stop(); std::exit(0); });

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        LOG_INFO << "[Demo] Node " << myId
                 << "  state=" << raft::stateName(node.getState())
                 << "  term="  << node.getCurrentTerm();
    }
}
```

---

## 12. 工程化：CMakeLists.txt 变更

来自 [CMakeLists.txt](CMakeLists.txt)（相对 day31 的主要变更）：

```cmake
# ── 新增：nlohmann/json vendored ──────────────────────────────────
set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_include_directories(NetLib PUBLIC
    $<BUILD_INTERFACE:${NLOHMANN_JSON_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# ── 新增：raft_demo 可执行文件 ────────────────────────────────────
add_executable(raft_demo examples/src/raft_demo.cpp)
target_link_libraries(raft_demo PRIVATE NetLib)

# ── 删除：GoogleTest FetchContent ─────────────────────────────────
# Day32 暂时移除 GoogleTest，等 Day33 引入 RaftTestHarness 时再接回来。
```

（a）**为什么删除 GoogleTest FetchContent**：
`FetchContent_Declare` 在无网络环境下失败，阻塞构建。
Day32 没有新增 GoogleTest 测试，暂时移除。Day33 接回 RaftTestHarness。

---

## 13. 验证：三终端启动 raft_demo

```bash
# 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target raft_demo -j

# 终端 1
./build/examples/raft_demo --id 0 --nodes 3

# 终端 2
./build/examples/raft_demo --id 1 --nodes 3

# 终端 3
./build/examples/raft_demo --id 2 --nodes 3
```

**预期观察**：
1. 启动后 150~300ms 内，某个节点打印 `*** 成为 LEADER ***`（term=1）
2. 其余两个节点打印 `Follower, term=1`
3. 每 500ms 各节点打印当前 state 和 term
4. Ctrl+C 杀掉 Leader 后 → 约 300ms 内剩余节点重新选出新 Leader（term 递增）
5. 逐一启动节点时，未就绪对端的 connect 失败属于正常，有退避机制

---

## 14. 局限与下一步

1. **Raft 只实现了选举，没有日志复制**：目前 AppendEntries 只携带 `term` 和 `leaderId`，
   作为纯心跳用。Day33 会补 `prevLogIndex`/`prevLogTerm`/`entries`，实现真正的日志同步。

2. **没有持久化**：`currentTerm_/votedFor_/log_` 全在内存，重启即丢失。
   Raft 要求 term 和 votedFor 持久化（落盘后再响应 RPC），Day33 接入简单的文件存储。

3. **没有单元测试**：`startElection`/`onVoteReply` 等核心路径全靠人工观察 raft_demo。
   Day33 引入 `RaftTestHarness`（单进程内直接调 `handleRequestVote/handleAppendEntries`，
   绕开 RPC，覆盖白盒分支）。

4. **RPC 帧 16 MiB 上限对 InstallSnapshot 不足**：Day33 暂时不引入快照，此上限够用；
   日后引入时需要 chunked 传输协议或扩大上限。

接下来 **Day 33** 将实现 Raft 日志复制（AppendEntries 完整语义）、持久化、以及
`RaftTestHarness` 白盒测试框架，完成"复制状态机"的最小可用版本。

