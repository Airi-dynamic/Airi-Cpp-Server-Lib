# Day 05 — Channel 抽象

## 项目状态

在 Day 04（Socket / InetAddress / Epoll 封装）基础上，引入 **Channel** 类：

- `Channel` 将 fd、关注事件（events）、返回事件（revents）封装为一个对象
- `Epoll::poll()` 返回 `vector<Channel*>`，不再暴露底层事件结构体
- `Socket::setnonblocking()` 从 `util` 移入 Socket 类

## 文件结构

```
day05/
├── CMakeLists.txt
├── server.cpp          ← Channel 驱动的事件循环 + echo
├── client.cpp          ← 交互式客户端
├── include/
│   ├── Channel.h       ← 新增：fd + events + revents + inEpoll
│   ├── Epoll.h         ← 改为返回 vector<Channel*>
│   ├── Socket.h        ← 新增 setnonblocking()
│   ├── InetAddress.h
│   └── util.h
└── common/
    ├── Channel.cpp     ← 新增
    ├── Epoll.cpp       ← kqueue(macOS)/epoll(Linux) 双路径
    ├── Socket.cpp
    ├── InetAddress.cpp
    └── util.cpp
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

# 终端 1
./build/server

# 终端 2
./build/client
```

## 已知问题

- `client_sock` / `client_addr` 在每次 accept 后 new 但未 delete（内存泄漏）
- 事件处理逻辑硬编码在 main 的 for 循环中，无回调机制
