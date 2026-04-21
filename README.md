# Day 09 — Buffer 缓冲区

## 项目状态

在 Day 08（Connection 类）基础上，引入 **Buffer** 缓冲区：

- `Buffer` 替代栈上 `char buf[1024]`，支持自动扩容、readv 分散读
- `Connection` 拆为 handleRead + handleWrite + send，实现非阻塞写
- `Channel` 新增 enableWriting / disableWriting / isWriting，支持读写事件分发

## 文件结构

```
day09/
├── CMakeLists.txt
├── server.cpp
├── client.cpp
├── include/
│   ├── Buffer.h        ← 新增
│   ├── Connection.h    ← 修改：双 Buffer + handleRead/handleWrite/send
│   ├── Channel.h       ← 修改：读写双回调 + enableWriting
│   ├── Acceptor.h
│   ├── Server.h
│   ├── EventLoop.h
│   ├── Epoll.h
│   ├── Socket.h
│   ├── InetAddress.h
│   └── util.h
└── common/
    ├── Buffer.cpp      ← 新增
    ├── Connection.cpp  ← 修改
    ├── Channel.cpp     ← 修改
    ├── Acceptor.cpp
    ├── Server.cpp
    ├── Eventloop.cpp
    ├── Epoll.cpp
    ├── Socket.cpp
    ├── InetAddress.cpp
    └── util.cpp
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

./build/server    # 终端 1
./build/client    # 终端 2
```

## 改进

- 应用层缓冲区解决半包/粘包问题
- 非阻塞写：先尝试直接 write，写不完才缓冲 + 注册 EPOLLOUT
- readv 分散读：一次系统调用最多读取 Buffer 剩余 + 64KB 栈缓冲
