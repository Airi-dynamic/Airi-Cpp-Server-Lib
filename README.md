# Day 07 — Acceptor 拆分

## 项目状态

在 Day 06（EventLoop + Server + Channel 回调）基础上，引入 **Acceptor** 类：

- `Acceptor` 封装 bind/listen/accept，通过 `newConnectionCallback` 回调 Server
- `Server` 精简为创建 Acceptor + 提供 `newConnection()` 回调

## 文件结构

```
day07/
├── CMakeLists.txt
├── server.cpp
├── client.cpp
├── include/
│   ├── Acceptor.h      ← 新增
│   ├── Server.h        ← 持有 Acceptor*
│   ├── EventLoop.h
│   ├── Channel.h
│   ├── Epoll.h
│   ├── Socket.h
│   ├── InetAddress.h
│   └── util.h
└── common/
    ├── Acceptor.cpp    ← 新增
    ├── Server.cpp      ← 修改
    ├── Eventloop.cpp
    ├── Channel.cpp
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

## 已知问题

- 客户端 Channel / Socket 仍有内存泄漏
- 无 Connection 抽象来管理客户端生命周期
