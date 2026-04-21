# Day 12 — 优雅关闭 + Channel 细粒度控制

## 项目状态

在 Day 11（StressTest）基础上，引入多项改进：

- `server.cpp` 添加 SIGINT 信号处理，支持 Ctrl+C 优雅关闭
- `Channel` 新增 `enableET()` / `disableET()` / `disableReading()` / `disableAll()` 细粒度事件控制
- `Epoll` 新增 `deleteChannel()` 从多路复用中移除 fd
- `Acceptor` 添加 `SO_REUSEADDR` 端口复用
- `Connection` 构造时显式调用 `enableET()`，读写事件注册解耦
- `EventLoop` 新增 `setQuit()` 方法控制主循环退出

## 文件结构

```
day12/
├── CMakeLists.txt
├── server.cpp              ← 修改：信号处理
├── client.cpp
├── include/
│   ├── ThreadPool.h
│   ├── Connection.h
│   ├── Server.h
│   ├── Buffer.h
│   ├── Channel.h           ← 修改：enableET/disableET/disableAll
│   ├── Acceptor.h
│   ├── EventLoop.h         ← 修改：setQuit
│   ├── Epoll.h             ← 修改：deleteChannel
│   ├── Socket.h
│   ├── InetAddress.h
│   └── util.h
├── common/
│   ├── ThreadPool.cpp
│   ├── Connection.cpp      ← 修改：显式 enableET
│   ├── Server.cpp
│   ├── Buffer.cpp
│   ├── Channel.cpp         ← 修改：细粒度控制实现
│   ├── Acceptor.cpp        ← 修改：SO_REUSEADDR
│   ├── Eventloop.cpp       ← 修改：setQuit 实现
│   ├── Epoll.cpp           ← 修改：deleteChannel + EINTR
│   ├── Socket.cpp
│   ├── InetAddress.cpp
│   └── util.cpp
└── test/
    ├── ThreadPoolTest.cpp
    └── StressTest.cpp
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

# 启动服务器（Ctrl+C 优雅关闭）
./build/server
```

## 与 Day 11 的区别

| 变更 | 说明 |
|------|------|
| `server.cpp` | 新增 signal(SIGINT) 处理，调用 setQuit() |
| `Channel` | 新增 enableET/disableET/disableReading/disableAll |
| `Epoll` | 新增 deleteChannel；poll 增加 EINTR 容错 |
| `Acceptor` | 添加 SO_REUSEADDR 端口复用 |
| `Connection` | 构造时显式 enableET()，与 enableReading 解耦 |
| `EventLoop` | 新增 setQuit() |
