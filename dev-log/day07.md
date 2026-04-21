# Day 07：Acceptor 拆分——将 accept 逻辑从 Server 中分离

> 引入 Acceptor 类，把 Socket 初始化 + bind + listen + accept 从 Server 中提取。
> Server 通过回调函数 `newConnectionCallback` 接收 Acceptor accept 到的新连接。

---

## 1. 引言

### 1.1 问题上下文

Day 06 的 Server 同时承担三件事：管理监听 fd、调用 accept、为新连接创建 Channel。这违反了单一职责原则——当后续要支持多个监听端口、或者要在 accept 前后插入限流/SSL 握手时，Server 类会爆炸。

muduo / Netty / boost.asio 都把"监听 + accept"独立成一个组件——muduo 叫 `Acceptor`，Netty 叫 `ServerBootstrap`，asio 叫 `acceptor`。它们的共同模式是：Acceptor 拥有监听 socket 与 accept Channel，accept 成功后通过回调把新连接的 fd 交给 Server 处理。

### 1.2 动机

拆分后：Acceptor 是一个独立可测试的组件，专注于"把 accept 出来的 fd 通过回调上报"；Server 退化为业务编排者，只关心"拿到 fd 之后做什么"。

回调注入（callback injection）是 Reactor 模式横向扩展的核心——把"决定怎么做"的权力下放给上层，框架代码（Acceptor）保持稳定。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| Server 内联 accept | Day 06 | 简单 | 单一职责违反、难扩展 |
| Acceptor + newConnectionCallback | muduo / 本日 | 职责清晰、可单测 | 多一层抽象 |
| Netty `ServerBootstrap.bind()` | Netty | 链式 API、配置丰富 | 概念繁多 |
| asio `acceptor.async_accept` | Asio | 协程友好 | proactor 模型 |
| Rust `TcpListener::accept` + tokio task | Tokio | spawn-per-connection 极简 | 任务调度开销 |

### 1.4 本日方案概述

本日实现：
1. 新建 `Acceptor` 类：内部持有监听 `Socket*` / 监听地址 `InetAddress*` / accept `Channel*`。
2. 构造函数完成 `bind` + `listen` + `enableReading`；`acceptConnection()` 调 `accept` 拿到 `int connfd`，触发 `newConnectionCallback_(connfd)`。
3. `Server` 持有 `Acceptor*`，构造时设 `newConnectionCallback_ = Server::newConnection`。
4. `Server::newConnection(int connfd)` 暂时仍是裸 echo 逻辑（连接对象抽象留到 Day 08）。

下一天会引入 `Connection` 类彻底解决客户端 fd 的生命周期问题。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Acceptor.h` | **新建** | Acceptor 类：持有监听 Socket/Channel，提供 `newConnectionCallback` |
| `common/Acceptor.cpp` | **新建** | 构造中 bind/listen；`acceptConnection()` accept → 回调 Server |
| `include/Server.h` | **修改** | 持有 `Acceptor*` 代替直接持有 Socket/Channel；新增 `newConnection()` |
| `common/Server.cpp` | **修改** | 构造中创建 Acceptor 并设回调；`newConnection()` 创建客户端 Channel |
| `include/Channel.h` | 不变 | 同 Day 06 |
| `include/Epoll.h` | 不变 | |
| 其余文件 | 不变 | |

---

## 3. 模块全景与所有权树（Day 07）

```
main()
├── Eventloop* loop                    ← new
│   └── Epoll* ep                      ← new
└── Server* server                     ← new Server(loop)
    ├── Eventloop* loop                ← 非拥有
    └── Acceptor* acceptor             ← new（server 拥有）
        ├── Socket* sock               ← new（acceptor 拥有，监听 socket）
        ├── InetAddress* addr          ← new（acceptor 拥有）
        ├── Channel* acceptChannel     ← new（acceptor 拥有）
        │   └── callback = Acceptor::acceptConnection
        └── newConnectionCallback = Server::newConnection
    [每个客户端]
    └── Channel* clientChannel         ← new，泄漏
        └── callback = [lambda echo 闭包]
```

**变化**：Server 不再直接持有监听 Socket/Channel，改由 Acceptor 管理。

---

## 4. 全流程调用链

**场景 A：启动初始化**

```
main()
① Eventloop* loop = new Eventloop()
② Server* server = new Server(loop)
   └── Server::Server(loop)
       acceptor = new Acceptor(loop)
       └── Acceptor::Acceptor(loop)
           sock = new Socket()                       ← socket()
           addr = new InetAddress("127.0.0.1", 8888)
           sock->bind(addr)
           sock->listen()
           sock->setnonblocking()
           acceptChannel = new Channel(loop, sock->getFd())
           cb = std::bind(&Acceptor::acceptConnection, this)
           acceptChannel->setCallback(cb)
           acceptChannel->enableReading()
           └── loop->updateChannel(this)
               └── ep->updateChannel(channel)

       cb = std::bind(&Server::newConnection, this, _1, _2)
       acceptor->setNewConnectionCallback(cb)

③ loop->loop()
```

**场景 B：新连接到达**

```
Eventloop::loop() → ep->poll() → acceptChannel->handleEvent()
└── Acceptor::acceptConnection()
    ① client_addr = new InetAddress()
    ② client_fd = sock->accept(client_addr)
    ③ client_sock = new Socket(client_fd)
       client_sock->setnonblocking()
    ④ newConnectionCallback(client_sock, client_addr)
       └── Server::newConnection(client_sock, client_addr)
           clientChannel = new Channel(loop, client_sockfd)
           cb = [lambda echo 闭包]
           clientChannel->setCallback(cb)
           clientChannel->enableReading()
    ⑤ delete client_addr
```

**场景 C：客户端数据可读**

```
Eventloop::loop() → ep->poll() → clientChannel->handleEvent()
└── [lambda]
    read() → write() echo
    bytes_read == 0 → close(fd) + break
```

---

## 5. 代码逐段解析

### 5.1 Acceptor.h — 监听 socket 的管理者

```cpp
class Acceptor {
  Eventloop *loop;
  Socket *sock;
  InetAddress *addr;
  Channel *acceptChannel;
  std::function<void(Socket *, InetAddress *)> newConnectionCallback;
public:
  Acceptor(Eventloop *_loop);
  void acceptConnection();
  void setNewConnectionCallback(std::function<void(Socket *, InetAddress *)> _cb);
};
```

> Acceptor 单一职责：bind + listen + accept，然后通过回调把新连接交给 Server。
> `newConnectionCallback` 的参数是 `(Socket*, InetAddress*)`，让 Server 决定如何处理。

### 5.2 Acceptor::acceptConnection()

```cpp
void Acceptor::acceptConnection() {
  InetAddress *client_addr = new InetAddress();
  int client_fd = sock->accept(client_addr);
  if (client_fd == -1) { delete client_addr; return; }

  Socket *client_sock = new Socket(client_fd);
  client_sock->setnonblocking();

  if (newConnectionCallback) {
    newConnectionCallback(client_sock, client_addr);
  }
  delete client_addr;
}
```

> accept → setnonblocking → 调用回调。
> 错误情况下提前返回并清理。

### 5.3 Server — 简化后的构造

```cpp
Server::Server(Eventloop *_loop) : loop(_loop) {
  acceptor = new Acceptor(loop);
  std::function<void(Socket *, InetAddress *)> cb =
    std::bind(&Server::newConnection, this, _1, _2);
  acceptor->setNewConnectionCallback(cb);
}
```

> 对比 Day 06：Server 构造函数从 10 行（直接操作 Socket/Channel）缩减为 4 行。
> 所有底层细节委托给 Acceptor。

### 5.4 Server::newConnection()

```cpp
void Server::newConnection(Socket *client_sock, InetAddress *client_addr) {
  int client_sockfd = client_sock->getFd();
  Channel *clientChannel = new Channel(loop, client_sockfd);
  std::function<void()> cb = [=] { /* read/write echo loop */ };
  clientChannel->setCallback(cb);
  clientChannel->enableReading();
}
```

> 这就是 Day 06 `handleReadEvent()` 中 accept 之后的部分，现在独立为回调函数。
> lambda 捕获 client_sockfd 做 echo。

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day07/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day07/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 07）

| 模块 | 职责 |
|------|------|
| `Acceptor` | 持有监听 Socket/Channel；accept → 回调 newConnectionCallback |
| `Server` | 创建 Acceptor；newConnection() 创建客户端 Channel + echo lambda |
| `Eventloop` | poll → handleEvent 循环 |
| `Channel` | fd + events + callback |
| `Epoll` | kqueue/epoll 封装 |

---

## 7. Day 07 的局限

1. **客户端 Channel 仍泄漏**：new 后无 delete
2. **无 Connection 抽象**：客户端的读写逻辑散在 lambda 中，无法管理生命周期
3. **Acceptor 传出的 client_sock 也泄漏**：newConnection 拿到 Socket* 后未保存

→ Day 08 引入 Connection 类，接管客户端 Socket 和 Channel 的生命周期。

---

## 8. 对应 HISTORY

→ `HISTORY/day07/`
