# Day 09：Buffer 缓冲区——解决半包/粘包与读写分离

> 引入 Buffer 类，替代之前的栈上 `char buf[1024]`。
> Connection 拆分为 readCallback/writeCallback 双回调，实现非阻塞写 + 应用层缓冲。
> Channel 新增 `enableWriting()` / `disableWriting()` / `isWriting()`，支持读写事件分发。

---

## 1. 引言

### 1.1 问题上下文

Day 08 的 Connection 用了一个栈上的 `char buf[1024]` 作为读缓冲区，写也是一次性 `write(fd, buf, n)`。这暴露了三个真实工程问题：

1. **半包/粘包**：TCP 是字节流，一次 `read` 可能读到半条业务消息或多条粘在一起的消息——固定大小 buf 无法表达"还差多少字节"。
2. **阻塞写**：非阻塞 socket 的 `write` 可能写不完（内核发送缓冲区满），返回 `EAGAIN`，剩余数据必须暂存。
3. **零拷贝优化**：内核到用户态、用户态到 socket 至少两次拷贝，应用层有缓冲区后可以做 `writev` / `sendfile` 等优化。

muduo `Buffer`、Netty `ByteBuf`、asio `streambuf` 都是为解决这三个问题而生的应用层缓冲区。

### 1.2 动机

没有应用层缓冲区，HTTP / Redis / MySQL 等任何"消息边界由协议本身定义"的协议都没法实现——你既不知道"已经收齐一条请求了吗"，也无法在内核写不下时把剩余数据暂存。

引入 Buffer 后，read/write 都变成"在缓冲区上读写"，IO 与协议解析正式分层。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 栈上 `char buf[N]` | Day 08 | 零分配 | 无法处理半包、写阻塞 |
| `vector<char>` + 读写指针 | muduo `Buffer` / 本日 | 简单、自动扩容、prepend 区方便协议头 | 拷贝开销 |
| 双缓冲 / 链式 buffer | Netty `CompositeByteBuf` | 减少拷贝、零拷贝拼接 | 实现复杂 |
| `iovec` + `readv`/`writev` | POSIX scatter-gather | 一次系统调用读写多段 | API 略复杂 |
| `sendfile` / `splice` | Linux 零拷贝 | 文件→socket 零次用户态拷贝 | 仅限文件→socket |

### 1.4 本日方案概述

本日实现：
1. 新建 `Buffer` 类：底层 `vector<char>`，维护 `readerIndex_` / `writerIndex_`；前置 `kCheapPrepend` 区为协议头预留空间。
2. `Buffer::readFd(fd, &savedErrno)` 用 `readv` 把数据先读到 buffer，剩余溢出的部分读到栈上 64KB extra buf 再 append——一次系统调用读完缓冲区。
3. `Connection` 拆为 `handleRead()` / `handleWrite()` / `send(data)`：send 先尝试直接 write，写不完的部分追加到 `outputBuffer_` 并 `enableWriting()`。
4. `Channel` 新增 `enableWriting()` / `disableWriting()` / `isWriting()` + 拆 `callback_` 为 `readCallback_` / `writeCallback_`。

后续协议层（HTTP / 自定义 RPC）都将基于 Buffer 之上实现解析。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Buffer.h` | **新建** | Buffer 类：vector 底层 + 读写指针 + prepend 区 |
| `common/Buffer.cpp` | **新建** | makeSpace / readFd(readv) / append / retrieve |
| `include/Channel.h` | **修改** | 新增 `enableWriting` / `disableWriting` / `isWriting`；callback 拆为 readCallback + writeCallback |
| `common/Channel.cpp` | **修改** | enableReading 使用 POLLER_READ；handleEvent 分发读/写；增加 enableWriting/disableWriting |
| `include/Connection.h` | **修改** | 新增 `inputBuffer_` / `outputBuffer_`；拆为 handleRead + handleWrite + send |
| `common/Connection.cpp` | **修改** | handleRead 通过 Buffer::readFd 读；send 先尝试直接写，剩余追加到 outputBuffer |
| `include/Epoll.h` | 不变 | |
| `include/Server.h` | 不变 | |
| `server.cpp` | 不变 | |
| 其余文件 | 不变 | |

---

## 3. 模块全景与所有权树（Day 09）

```
main()
├── Eventloop* loop
│   └── Epoll* ep
└── Server* server
    ├── Acceptor* acceptor
    │   ├── Socket* sock (监听)
    │   ├── Channel* acceptChannel
    │   └── newConnectionCallback → Server::newConnection
    └── map<int, Connection*> connection
        └── Connection* conn (每个客户端)
            ├── Socket* sock
            ├── Channel* channel
            │   ├── readCallback  → Connection::handleRead
            │   └── writeCallback → Connection::handleWrite
            ├── Buffer inputBuffer_   ← 新增
            ├── Buffer outputBuffer_  ← 新增
            └── deleteConnectionCallback → Server::deleteConnection
```

---

## 4. 全流程调用链

**场景 A：新连接**

```
ep->poll() → acceptChannel->handleEvent()
└── Acceptor::acceptConnection()
    client_sock = new Socket(accept_fd)
    newConnectionCallback(client_sock, client_addr)
    └── Server::newConnection(client_sock, client_addr)
        conn = new Connection(loop, client_sock)
        └── Connection::Connection()
            channel = new Channel(loop, sock->getFd())
            channel->setReadCallback(bind(&Connection::handleRead))
            channel->setWriteCallback(bind(&Connection::handleWrite))
            channel->enableReading()
        conn->setDeleteConnectionCallback(bind(&Server::deleteConnection))
        connection[fd] = conn
```

**场景 B：数据可读**

```
ep->poll() → clientChannel->handleEvent()
    revents & POLLER_READ
    └── Connection::handleRead()
        n = inputBuffer_.readFd(sockfd, &savedErrno)
        if n > 0:
            msg = inputBuffer_.retrieveAllAsString()
            send(msg)   ← Echo 业务
        if n == 0:
            deleteConnectionCallback(sock)
```

**场景 C：send 写数据**

```
Connection::send(msg)
    1. outputBuffer 为空 → 尝试直接 write()
       if 全部写完 → return
       if 部分写完 → remaining = msg.size() - nwrote
    2. 剩余数据 → outputBuffer_.append(msg + nwrote, remaining)
       if !channel->isWriting() → channel->enableWriting()
```

**场景 D：内核通知可写**

```
ep->poll() → clientChannel->handleEvent()
    revents & POLLER_WRITE
    └── Connection::handleWrite()
        write(fd, outputBuffer_.peek(), readableBytes)
        outputBuffer_.retrieve(n)
        if readableBytes == 0 → channel->disableWriting()
```

---

## 5. 代码逐段解析

### 5.1 Buffer.h — 应用层缓冲区

```cpp
class Buffer {
    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
    static const size_t kCheapPrepand = 8;
    static const size_t kInitialSize = 1024;
};
```

> 三段式布局：`[prepend | readable | writable]`
> - `readerIndex_` 初始 = kCheapPrepand (8)
> - `writerIndex_` 初始 = kCheapPrepand (8)
> - 可读 = writerIndex_ - readerIndex_，可写 = buffer_.size() - writerIndex_

### 5.2 Buffer::readFd — readv 分散读

```cpp
ssize_t Buffer::readFd(int fd, int *savedErrno) {
    char extrabuf[65536];
    struct iovec vec[2];
    vec[0].iov_base = beginWrite();  // Buffer 尾部
    vec[0].iov_len  = writable;
    vec[1].iov_base = extrabuf;      // 栈上 64KB
    vec[1].iov_len  = sizeof(extrabuf);
    const ssize_t n = ::readv(fd, vec, iovcnt);
}
```

> 用 `readv` 做分散读：先填 Buffer 可写区，溢出部分进栈缓冲 extrabuf。
> 一次系统调用最多读 64KB + Buffer 剩余，避免反复 read。

### 5.3 Buffer::makeSpace — 自动扩容 / 内部腾挪

```cpp
void Buffer::makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepand) {
        buffer_.resize(writerIndex_ + len);  // 扩容
    } else {
        // 把已读区域回收：数据前移到 prepend 之后
        std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepand);
    }
}
```

> 优先内部腾挪（回收 prepend 前的废弃空间），不够才 resize。

### 5.4 Channel — 读写事件分发

```cpp
void Channel::handleEvent() {
    if (revents & (POLLER_READ | POLLER_PRI))
        readCallback();
    if (revents & POLLER_WRITE)
        writeCallback();
}

void Channel::enableWriting()  { events |= POLLER_WRITE;  loop->updateChannel(this); }
void Channel::disableWriting() { events &= ~POLLER_WRITE; loop->updateChannel(this); }
```

> Day 08 只有一个 callback，现在拆成 readCallback + writeCallback。
> 写完所有数据后 disableWriting 避免 busy loop。

### 5.5 Connection::send — 先直接写，再缓冲

```cpp
void Connection::send(const std::string &msg) {
    if (!channel->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(sock->getFd(), msg.data(), msg.size());
        // ...
    }
    if (!faultError && remaining > 0) {
        outputBuffer_.append(msg.data() + nwrote, remaining);
        channel->enableWriting();
    }
}
```

> 优化路径：如果 outputBuffer 空且内核可写，直接 write 跳过缓冲。
> 只在写不完时才追加到 outputBuffer 并注册 EPOLLOUT。

---

### 5.6 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day09/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day09/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 09）

| 模块 | 职责 |
|------|------|
| `Buffer` | 应用层读写缓冲区：readFd(readv) / append / retrieve / 自动扩容 |
| `Connection` | 拥有 Socket + Channel + 双 Buffer；handleRead/handleWrite/send |
| `Channel` | fd + 读写事件 + 双回调；enableWriting/disableWriting |
| `Server` | Acceptor + Connection map；newConnection/deleteConnection |
| `Acceptor` | bind/listen/accept → 回调 |
| `Eventloop` | poll → handleEvent 循环 |

---

## 7. Day 09 的局限

1. **业务逻辑仍硬编码**：handleRead 中写死了 Echo（`send(msg)`），无法自定义业务
2. **单线程**：所有 IO 和业务在一个线程，长耗时操作会阻塞 epoll loop
3. **Buffer 非线程安全**：如果未来引入多线程，需要额外保护

→ Day 10 引入 ThreadPool 实现业务逻辑异步执行。

---

## 8. 对应 HISTORY

→ `HISTORY/day09/`
