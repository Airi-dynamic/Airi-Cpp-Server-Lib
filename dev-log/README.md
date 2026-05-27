# Airi-Cpp-Server-Lib 开发日志索引

本目录包含项目从零开始到当前状态的完整开发过程记录。每个文件对应一个开发阶段，
包含知识背景解析、实现思路、代码示例、所有权树、全流程调用链和职责划分表。

> **方向说明（2026-05-27）**：day31–day36 为未实施的旧规划（WebSocket / 协程 / io_uring /
> 无锁队列 / 内存池 / muduo 对比），对应的 HISTORY/day31-36/ 快照保留作为"曾考虑路线"。
> **自 day37 起，项目转向分布式存储方向**（Raft 共识 + 自研 LSM-Tree + 分布式 KV），
> 详见 AGENT_MEMORY/PLAN.md。

---

## 目录

| 日志文件 | 主题 | 核心内容 |
|----------|------|----------|
| [day01.md](day01.md) | 原始 TCP 服务器 | socket/bind/listen/accept 阻塞模型 |
| [day02.md](day02.md) | Echo 循环 + errif | while(read/write) echo 循环，错误检查宏 |
| [day03.md](day03.md) | epoll + 非阻塞 IO | epoll_create/ctl/wait，非阻塞读写，ET vs LT |
| [day04.md](day04.md) | 第一次 OOP 封装 | Socket / InetAddress / Epoll 类 |
| [day05.md](day05.md) | Channel 类 | fd 与事件回调的绑定，Channel 作为事件分发器 |
| [day06.md](day06.md) | EventLoop | 事件循环独立为类 |
| [day07.md](day07.md) | Acceptor | 监听逻辑抽取 |
| [day08.md](day08.md) | Connection + TcpServer 原型 | 连接封装 + 完整生命周期 |
| [day09.md](day09.md) | Buffer | readv scatter read，双指针缓冲区 |
| [day10.md](day10.md) | ThreadPool（上） | 通用任务队列引入 |
| [day11.md](day11.md) | ThreadPool（下） | ThreadPool 稳定化 + StressTest |
| [day12.md](day12.md) | deleteChannel + Channel 细化 | Channel 细粒度 API + 优雅退出 |
| [day13.md](day13.md) | Main-Sub Reactor 多线程 | fd % N 负载均衡，TcpServer 顶层调度 |
| [day14.md](day14.md) | Macros.h + 代码规范 | 工程化宏定义 |
| [day15.md](day15.md) | Connection State + 回调解耦 | Connection 状态枚举，Server 回调解耦 |
| [day16.md](day16.md) | Exception + SignalHandler + pine.h | 异常处理 + 信号 + 统一头文件 |
| [day17.md](day17.md) | Poller 抽象（#ifdef 过渡） | epoll/kqueue 条件编译 |
| [day18.md](day18.md) | 多后端 Poller（继承 + 工厂） | Poller 基类 + EpollPoller / KqueuePoller |
| [day19.md](day19.md) | unique_ptr 重构 | TcpServer 自包含，RAII 所有权链 |
| [day20.md](day20.md) | 跨线程生命周期安全 | 延迟析构，deleteConnection 投递 |
| [day21.md](day21.md) | Ctrl+C 修复 + **全架构梳理** | 所有权树、初始化/析构顺序、全流程调用链、线程安全分析 |
| [day22.md](day22.md) | EventLoopThreadPool | one-loop-per-thread，EventLoopThread 集成 |
| [day23.md](day23.md) | Timer 系统 | TimeStamp / Timer / TimerQueue |
| [day24.md](day24.md) | Log 系统 | LogStream / Logger / AsyncLogging / LogFile，双缓冲异步 |
| [day25.md](day25.md) | HTTP 协议层 | HttpContext 15 状态 FSM，HttpRequest / HttpResponse，HttpServer 组合 TcpServer |
| [day26.md](day26.md) | HTTP 应用层演示 | 静态文件服务 / 文件上传 / 302 重定向 / 空闲超时 / 内置压测（已被 day36 wrk 取代）|
| [day27.md](day27.md) | 调试与性能优化 | Sanitizer 验证，编译警告清理，热路径优化 |
| [day28.md](day28.md) | 测试框架 + 回压机制 | GoogleTest / CTest，三层水位线回压，纯策略函数提取 |
| [day29.md](day29.md) | 生产特性 | 请求限流 / TLS / sendFile 零拷贝 / OPTIONS / 工具方法下沉 |
| [day30.md](day30.md) | 路由表 + 中间件链 | 精确/前缀路由，洋葱模型中间件，HttpServer::Options 配置聚合 |

---

## 架构演进路线

```
Day 01-03  纯 POSIX 系统调用，单线程阻塞 → epoll 多路复用
    │
Day 04-09  面向对象封装：Socket / Channel / EventLoop / Acceptor / Connection / Buffer
    │
Day 10-11  ThreadPool（通用线程池，后被 EventLoopThreadPool 替代）
    │
Day 12     Channel 细粒度 API + 优雅退出
    │
Day 13-16  Main-Sub Reactor 多线程，代码规范，异常/信号处理
    │
Day 17-18  Poller 抽象：epoll/kqueue 双后端（继承 + 工厂模式）
    │
Day 19-20  RAII 智能指针重构，跨线程生命周期安全
    │
Day 21     Ctrl+C 修复 + 全架构梳理 ★ 建议阅读
    │
Day 22     EventLoopThreadPool（one-loop-per-thread 最终形态）
    │
Day 23-24  Timer 系统 + 异步日志系统（双缓冲，业务线程零阻塞）
    │
Day 25-26  HTTP/1.1 协议层（状态机解析器）+ 应用层演示
    │
Day 27     调试与性能优化（Sanitizer + 热路径优化）
    │
Day 28     测试框架 + 回压机制（GoogleTest / 三层水位线 / 纯策略函数）
    │
Day 29     生产特性（请求限流 / TLS / sendFile 零拷贝）
    │
Day 30     路由表 + 中间件链 + 配置聚合（最终架构）
```

---

## 关键设计决策索引

| 决策 | 位置 |
|------|------|
| Buffer::readFd 为何用 readv + 栈缓冲区 | [day09.md](day09.md) |
| Channel 细粒度 API 设计 | [day12.md](day12.md) |
| delete Connection 为何必须投递到 Sub Reactor 线程 | [day20.md](day20.md)、[day21.md](day21.md) §全流程调用链·场景C |
| queueInLoop 的"swap-and-execute"模式 | [day21.md](day21.md) §线程安全分析 |
| 为什么 EventLoop 必须在其归属线程内构造 | [day22.md](day22.md) |
| HttpContext 15 状态机设计与 std::any context 槽 | [day25.md](day25.md) |
| 空闲超时为何用 weak_ptr alive flag | [day26.md](day26.md) |
| 回压三层水位线设计 | [day28.md](day28.md) |
| 请求限流在解析阶段即拒绝 | [day29.md](day29.md) |
| 精确路由 hash + 前缀路由 vector 的权衡 | [day30.md](day30.md) |
| 中间件洋葱模型 vs pipeline | [day30.md](day30.md) |

---

## HISTORY 对应关系

每个 dev-log 文件对应 `HISTORY/` 目录下的同名子项目：

`dev-log/dayNN.md` ↔ `HISTORY/dayNN/`
