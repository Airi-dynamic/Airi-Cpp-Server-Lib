# Day 05：Channel 抽象——fd 与事件回调的绑定

> 引入 Channel 类，将"fd + 关注的事件 + 返回的事件"打包为一个对象。
> Epoll::poll() 返回 `vector<Channel*>` 而非裸事件结构体。
> server.cpp 通过 Channel 驱动事件循环。

---

## 1. 引言

### 1.1 问题上下文

Day 04 把 socket / epoll / 地址封装成了类，但事件循环里仍然要写 `if (events[i].data.fd == listen_fd) { accept... } else { read... }`——fd 与"该做什么"的对应关系散落在事件循环的 switch 里。当回调种类一多（read / write / error / close / 业务），这种模式就失控了。

Reactor 模式（Schmidt 1995）的核心抽象就是 **Channel/Handle**：把"一个 fd + 关注的事件 + 发生事件时该调用的回调"打包成一个对象，由 Poller 把就绪 Channel 列出来，事件循环只负责"对每个就绪 Channel 调它自己的 handler"。这是 ACE / muduo / Netty / libevent 等所有 Reactor 框架的共同抽象。

### 1.2 动机

没有 Channel，事件循环就必须了解每种 fd 的具体类型；有了 Channel，事件循环只需要 `for (ch : ready) ch->handleEvent()`——多态地分发，新加一种 fd 不需要改循环代码。

对项目来说，Channel 是后续 EventLoop / Acceptor / Connection / TimerQueue / WakeupChannel 的共同父抽象，是整个 Reactor 体系的"原子"。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 事件循环里 if/switch 分类型 | Day 04 / 教科书 | 直观、无额外抽象 | 高耦合、难扩展 |
| Channel + 回调绑定 | Reactor pattern (Schmidt 1995) / muduo | 解耦事件发现与处理、易扩展 | 多了一层 indirection |
| boost.asio `async_*` + completion handler | Asio | 协程友好、跨平台 | proactor 模型，理解成本高 |
| Netty `ChannelPipeline` | Netty (Java) | 责任链可拼装、灵活 | 抽象层多、调试栈深 |
| Rust `mio::Token` + 用户态映射 | mio | 零成本抽象 | 需要自行维护 token→handler 映射 |

### 1.4 本日方案概述

本日实现：
1. 新建 `Channel` 类：持有 `fd_` / `events_` (希望监听) / `revents_` (实际发生) / `inEpoll_` 标记。
2. `Channel::enableReading()` 设置 `events_` 后调 `ep->updateChannel(this)`。
3. `Epoll::updateChannel(Channel*)` 根据 `inEpoll_` 决定 `EPOLL_CTL_ADD`/`MOD`，把 `Channel*` 塞进 `epoll_data.ptr`。
4. `Epoll::poll()` 返回 `vector<Channel*>`，从 `epoll_event.data.ptr` 取出。
5. `server.cpp` 通过 Channel 驱动事件循环；为每个客户端 `new Channel`（暂时泄漏，下一天有 EventLoop 后再统一管理）。

留一个已知 bug：客户端 `Channel*` / `Socket*` 不会被释放——这是为了把"连接生命周期管理"留到 Day 06-08 集中解决。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Channel.h` | **新建** | Channel 类声明：持有 fd + events + revents + inEpoll 标记 |
| `common/Channel.cpp` | **新建** | Channel 实现：`enableReading()` 调用 `ep->updateChannel(this)` |
| `include/Epoll.h` | **修改** | `poll()` 返回 `vector<Channel*>`；新增 `updateChannel()` |
| `common/Epoll.cpp` | **修改** | `updateChannel()` 根据 `inEpoll` 决定 ADD/MOD；`poll()` 从事件中提取 Channel 指针 |
| `include/Socket.h` | **修改** | 新增 `setnonblocking()` 成员函数 |
| `common/Socket.cpp` | **修改** | 实现 `setnonblocking()` |
| `include/util.h` | 不变 | |
| `server.cpp` | **修改** | 使用 Channel + Epoll 组合驱动事件循环 |
| `client.cpp` | 不变 | 同 Day 04 |

---

## 3. 模块全景与所有权树（Day 05）

```
main()
├── Socket* serv_sock              ← new，持有监听 fd
│   └── int fd                     ← RAII
├── InetAddress* serv_addr         ← new
├── Epoll* ep                      ← new，持有 kqfd/epfd + events 数组
├── Channel* servChannel           ← new Channel(ep, serv_sock->getFd())
│   ├── Epoll* ep                  ← 非拥有指针，指向上方 ep
│   ├── int fd                     ← 非拥有，复制自 serv_sock->getFd()
│   ├── uint32_t events            ← 希望监听的事件
│   ├── uint32_t revents           ← 实际发生的事件（由 poll() 填充）
│   └── bool inEpoll               ← 是否已注册
└── [每个客户端]
    ├── Socket* client_sock        ← new Socket(client_sockfd)  ← 内存泄漏！
    └── Channel* clientChannel     ← new Channel(ep, client_sockfd)
```

**关键设计**：Channel 不拥有 fd（不负责 close）。fd 的生命周期仍由调用方管理。

---

## 4. 全流程调用链

**场景 A：Channel 注册到 Epoll**

```
[main()]

① Channel* servChannel = new Channel(ep, serv_sock->getFd())
   └── Channel::Channel(ep, fd)
       events = 0, revents = 0, inEpoll = false

② servChannel->enableReading()
   └── Channel::enableReading()
       events = POLLER_READ | POLLER_ET
       ep->updateChannel(this)
       └── Epoll::updateChannel(channel)
           fd = channel->getFd()
           [macOS] EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void*)channel)
                   kevent(epfd, &change, 1, nullptr, 0, nullptr)
           [Linux] ev.data.ptr = channel; ev.events = channel->getEvents()
                   if (!channel->getInEpoll())
                       epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev)
                   else
                       epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev)
           channel->setInEpoll(true)
```

**场景 B：Epoll::poll() 返回 Channel**

```
[事件循环]

① ep->poll()
   └── Epoll::poll()
       [macOS] nfds = kevent(kqfd, nullptr, 0, events, MAX, nullptr)
               for i in 0..nfds:
                   ch = (Channel*)events[i].udata    ← 从 kevent 的 udata 取回 Channel 指针
                   ch->setRevents(POLLER_READ)
       [Linux] nfds = epoll_wait(epfd, events, MAX, -1)
               for i in 0..nfds:
                   ch = (Channel*)events[i].data.ptr ← 从 epoll_event 的 data.ptr 取回
                   ch->setRevents(events[i].events)
       return activeChannels
```

> **核心技巧**：注册时将 Channel 指针存入内核事件结构体的 `udata`(kqueue) / `data.ptr`(epoll)，
> poll 返回时直接取回——零查找开销的 fd→Channel 映射。

**场景 C：新连接到达**

```
[事件循环]

① activeChannels[i]->getFd() == serv_sock->getFd()
② InetAddress* client_addr = new InetAddress()
③ serv_sock->accept(client_addr) → client_sockfd
④ Socket* client_sock = new Socket(client_sockfd)
   └── 注意：Socket(int fd) 构造函数不调用 socket()，只保存 fd
   client_sock->setnonblocking()
⑤ Channel* clientChannel = new Channel(ep, client_sockfd)
   clientChannel->enableReading()
   └── 注册到 Epoll

⚠️ client_sock 和 client_addr 在此后被丢弃但未 delete → 内存泄漏
```

**场景 D：客户端断开**

```
① bytes_read == 0
② close(chFd)                    ← 关闭 fd
③ delete activeChannels[i]       ← 释放 Channel 对象
   activeChannels[i] = nullptr
```

---

## 5. 代码逐段解析

### 5.1 Channel.h — fd + 事件的封装

```cpp
class Channel {
private:
  Epoll *ep;
  int fd;
  uint32_t events;   // 希望监听的事件
  uint32_t revents;  // 实际发生的事件
  bool inEpoll;      // 是否已注册到 Epoll
public:
  Channel(Epoll *_ep, int _fd);
  ~Channel();
  void enableReading();
  // ... getter/setter
};
```

> Channel 是 Reactor 模式的核心抽象：一个 Channel 代表一个"正在被监控的 fd"。
> `events` 是用户设置的（想监听什么），`revents` 是内核返回的（实际发生了什么）。
> `inEpoll` 标记让 `updateChannel()` 知道是 ADD 还是 MOD。

### 5.2 Channel::enableReading()

```cpp
void Channel::enableReading() {
  events = POLLER_READ | POLLER_ET;
  ep->updateChannel(this);
}
```

> 设置 events 后立即调用 Epoll 注册。
> `POLLER_READ | POLLER_ET` 是跨平台常量，在 macOS 上映射为自定义值，
> Epoll 内部实际使用 kqueue 的 `EVFILT_READ + EV_CLEAR`。

### 5.3 Epoll::updateChannel() — kqueue 路径

```cpp
void Epoll::updateChannel(Channel *channel) {
  int fd = channel->getFd();
  struct kevent change;
  EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void *)channel);
  errif(kevent(epfd, &change, 1, nullptr, 0, nullptr) == -1,
        "kqueue update error");
  channel->setInEpoll();
}
```

> `udata` 字段存储 Channel 指针——这是 kqueue 的用户数据透传机制。
> kqueue 的 `EV_ADD` 对已存在的 event 会自动覆盖（相当于 MOD），所以不需要 ADD/MOD 分支。

### 5.4 server.cpp — Channel 驱动的事件循环

```cpp
std::vector<Channel *> activeChannels = ep->poll();
for (int i = 0; i < nfds; ++i) {
    int chFd = activeChannels[i]->getFd();
    if (chFd == serv_sock->getFd()) {
        // 新连接：accept → 创建 Channel → enableReading
    } else if (activeChannels[i]->getRevents() & POLLER_READ) {
        // 数据可读：循环 read → write echo
    }
}
```

> 与 Day 04 对比：不再直接操作 `epoll_event` / `kevent` 结构体，
> 而是通过 `Channel->getRevents()` 查询事件类型。

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day05/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day05/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 05）

| 模块 | 职责 |
|------|------|
| `Channel` | 封装 fd + 事件注册；通过 Epoll 指针驱动注册 |
| `Epoll` | 管理 kqueue/epoll 实例；`updateChannel()` 注册/修改 Channel；`poll()` 返回活跃 Channel 列表 |
| `Socket` | RAII fd + bind/listen/accept/connect + setnonblocking |
| `InetAddress` | sockaddr_in 封装 |
| `errif()` | 条件错误退出 |
| `main()` | 组装 Socket/Epoll/Channel + 事件循环 |

---

## 7. Day 05 的局限

1. **内存泄漏**：`client_sock` 和 `client_addr` new 后未 delete
2. **事件处理硬编码在 main 中**：新连接/数据读取的逻辑散落在 for 循环
3. **Channel 无回调函数**：判断事件类型靠 if/else 而非回调
4. **无 EventLoop 抽象**：事件循环直接写在 main

→ Day 06 引入 EventLoop，将事件循环封装为独立类。

---

## 8. 对应 HISTORY

→ `HISTORY/day05/`
