# Day 12：优雅关闭 + Channel 细粒度控制 + Epoll::deleteChannel

> 引入信号处理实现优雅关闭 (`setQuit`)，Channel 新增 `enableET/disableET/disableReading/disableAll` 细粒度事件控制。
> Acceptor 添加 `SO_REUSEADDR` 端口复用，Epoll 新增 `deleteChannel` 方法。
> Connection 构造时显式调用 `enableET()`，读/写事件注册解耦。
> `epoll_wait` / `kevent` 增加 `EINTR` 容错。

---

## 1. 引言

### 1.1 问题上下文

到 Day 11，服务器已经能扛压力，但有三个工程级缺陷：

1. **Ctrl+C 不优雅**：`SIGINT` 会让进程立即退出，连接未关闭、缓冲区未冲、worker 线程未 join——再次启动可能因端口未释放而失败。
2. **端口 TIME_WAIT 不可重启**：服务器重启后立即 `bind()` 同一端口会失败（处于 TIME_WAIT 的旧连接占着端口）。
3. **`epoll_wait` / `kevent` 被信号打断**：信号处理函数返回后，`epoll_wait` 返回 -1 + `errno=EINTR`，被错误当成致命错误退出循环。

这三件事是所有教学服务器走向工业级必须修的"小事"——不修，运维场景立刻翻车。

### 1.2 动机

优雅关闭让服务可以做滚动升级、连接迁移、资源释放；`SO_REUSEADDR` 让重启不卡 60 秒 TIME_WAIT；`EINTR` 容错让信号处理与事件循环和平共处。

同时，Channel 的事件控制还需要更细粒度的 API：单独 enable/disable 读、单独切换 ET/LT——这些在协议层（HTTP keep-alive 暂停读）和异步日志（写完触发 disable）等场景都用得到。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 直接 `exit()` | 教科书 | 简单 | 资源泄漏、连接断尾 |
| 信号处理 + `setQuit()` 优雅退出 (本日) | muduo / Linux 守护进程惯用 | 所有线程协同退出 | 信号处理函数能做的事有限 |
| `signalfd` / `kqueue EVFILT_SIGNAL` | Linux/BSD | 信号变成事件，线程安全 | 平台特定 |
| systemd `SIGTERM` + watchdog | systemd | 进程托管标准 | 平台绑定 |
| Kubernetes preStop hook | k8s | 容器编排标准 | 集群特定 |

### 1.4 本日方案概述

本日实现：
1. `server.cpp`：`signal(SIGINT, signalHandler)`；handler 调 `g_loop->setQuit()` 让事件循环退出。
2. `EventLoop::setQuit()`：将 `quit_` 标志置为 true。
3. `Channel` 新增 `enableET()` / `disableET()` / `disableReading()` / `disableAll()` 细粒度 API；`enableReading` 不再硬编码 ET。
4. `Acceptor` 的 listen socket 加 `setsockopt(SO_REUSEADDR)`。
5. `Epoll::poll` 中 `epoll_wait` / `kevent` 返回 -1 时，若 `errno == EINTR` 视为正常继续循环。
6. `Connection` 构造时显式调 `channel->enableET()`。

下一天会把单 Reactor 拆为多 Reactor（Day 13 的 mainReactor + subReactors）。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `server.cpp` | **修改** | 新增 signal(SIGINT, signalHandler)，调用 loop->setQuit() 优雅退出 |
| `include/EventLoop.h` | **修改** | 新增 `setQuit()` 方法 |
| `common/Eventloop.cpp` | **修改** | 新增 `setQuit()` 实现：`this->quit = true` |
| `include/Channel.h` | **修改** | 新增 `enableET()`, `disableET()`, `disableReading()`, `disableAll()` |
| `common/Channel.cpp` | **修改** | 实现新增方法；`enableReading` 改为 `events \|= POLLER_READ`（不再硬编码 ET） |
| `include/Epoll.h` | **修改** | 新增 `deleteChannel()` 声明 |
| `common/Epoll.cpp` | **修改** | 新增 `deleteChannel()` 实现；`epoll_wait` 增加 `errno != EINTR` 判断 |
| `common/Acceptor.cpp` | **修改** | 添加 `setsockopt(SO_REUSEADDR)` 端口复用；enableReading 后不再手动加 ET |
| `common/Connection.cpp` | **修改** | 构造时显式调用 `channel->enableET()`，enableReading 和 ET 解耦 |
| 其余文件 | 不变 | Server/Buffer/ThreadPool/Socket/InetAddress 沿用 Day 11 |

---

## 3. 模块全景与所有权树（Day 12）

```
main()
├── signal(SIGINT, signalHandler)          ← 新增
│   └── g_loop->setQuit()
├── Eventloop* loop
│   └── Epoll* ep
│       └── deleteChannel()                ← 新增
└── Server* server
    ├── Acceptor* acceptor
    │   └── setsockopt(SO_REUSEADDR)       ← 新增
    ├── ThreadPool* threadPool
    └── map<int, Connection*> connection
        └── Connection* conn
            └── channel->enableET()        ← 新增，显式开启 ET
```

---

## 4. 全流程调用链

**场景 A：优雅关闭**
```
用户按 Ctrl+C → SIGINT → signalHandler(signum)
  → g_loop->setQuit()
  → Eventloop::loop() 中 while(!quit) 退出循环
  → main: delete server, delete loop
```

**场景 B：Channel 事件控制（Day 12 改动）**
```
Connection 构造:
  → channel->enableReading()     // events |= POLLER_READ → updateChannel
  → channel->enableET()          // events |= POLLER_ET   → updateChannel

Connection::handleWrite:
  → 写完后 channel->disableWriting()  // events &= ~POLLER_WRITE

Connection::send:
  → 写不完时 channel->enableWriting() // events |= POLLER_WRITE
```

**场景 C：Epoll::deleteChannel（新增）**
```
需要移除 fd 时:
  → epoll_ctl(EPOLL_CTL_DEL) / kevent(EV_DELETE)
  → channel->setInEpoll(false)
```

---

## 5. 代码逐段解析

### 5.1 `server.cpp` — 信号处理

```cpp
Eventloop *g_loop = nullptr;
void signalHandler(int signum) {
    if (g_loop) g_loop->setQuit();
}
int main() {
    signal(SIGINT, signalHandler);
    // ...
}
```

### 5.2 `Channel.cpp` — 细粒度控制

| 方法 | 实现 | 说明 |
|------|------|------|
| `enableReading()` | `events \|= POLLER_READ` | 不再硬编码 ET，与 Day 11 不同 |
| `disableReading()` | `events &= ~POLLER_READ` | 新增 |
| `enableET()` | `events \|= POLLER_ET` | 新增，显式控制边缘触发 |
| `disableET()` | `events &= ~POLLER_ET` | 新增，不触发 updateChannel |
| `disableAll()` | `events = 0` | 新增，清除所有事件 |

### 5.3 `Acceptor.cpp` — SO_REUSEADDR

```cpp
int opt = 1;
setsockopt(sock->getFd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```
解决服务器重启时端口被占用 (TIME_WAIT) 的问题。

### 5.4 `Epoll.cpp` — deleteChannel + EINTR

```cpp
void Epoll::deleteChannel(Channel *channel) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);  // 或 kqueue EV_DELETE
    channel->setInEpoll(false);
}

// poll 中增加 EINTR 容错
errif(nfds == -1 && errno != EINTR, "epoll wait error");
```

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day12/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day12/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| `server.cpp` | 注册信号处理，实现优雅关闭 |
| `EventLoop` | 新增 setQuit() 控制主循环退出 |
| `Channel` | 细粒度事件控制：独立的 ET/读/写/全清操作 |
| `Epoll` | 新增 deleteChannel 从多路复用中移除 fd |
| `Acceptor` | SO_REUSEADDR 端口复用 |
| `Connection` | 显式 enableET()，读写事件解耦 |

---

## 7. 局限

1. **setQuit 非原子**：`quit` 是普通 bool，在信号处理函数中写、主循环中读，存在可见性问题（应使用 `std::atomic<bool>` 或 `volatile sig_atomic_t`）
2. **deleteChannel 未使用**：目前 deleteConnection 直接 delete channel，未调用 Epoll::deleteChannel（后续会完善）
3. **线程安全未解决**：Day 11 遗留的 threadPool 中 conn 指针竞态仍存在
4. **单 Reactor 瓶颈**：所有连接仍在同一个 Eventloop
