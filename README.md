# Day 26 — HTTP 应用层演示

在 Day 25 的 HTTP 协议栈之上构建完整的文件服务器应用：首页、登录表单、文件上传/下载/删除，以及空闲连接自动超时关闭。

## 新增 / 变更模块

| 文件 | 说明 |
|------|------|
| `http_server.cpp` | 完整 HTTP 文件服务器（6 路由：首页、登录、文件列表、下载、删除、上传） |
| `include/Connection.h` | 新增 `alive_` (shared_ptr<bool>)、`touchLastActive()`、`lastActive()` |
| `common/Connection.cpp` | 析构时 `*alive_ = false`；Read() 中更新 lastActive |
| `include/http/HttpRequest.h` | 新增 `appendBody()` 避免 O(n²) 拼接 |
| `include/http/HttpResponse.h` | 新增 `k302Found` 状态码、`addHeader()` |
| `include/http/HttpServer.h` | 新增 `setAutoClose()` / `scheduleIdleClose()` |
| `common/http/HttpServer.cpp` | 空闲超时递归调度定时器（weak_ptr 防野指针） |
| `common/http/HttpContext.cpp` | body 解析改用 `appendBody()` |
| `include/log/Logger.h` | 新增 `LOG_FATAL` 宏 |
| `test/BenchmarkTest.cpp` | 多线程 HTTP 内置压测工具 |
| `static/index.html` | 首页 HTML |
| `static/login.html` | 登录表单页 |
| `static/fileserver.html` | 文件管理页模板 |
| `files/readme.txt` / `files/scores.csv` | 示例文件 |

## 构建 & 运行

```bash
cd HISTORY/day26
cmake -S . -B build && cmake --build build -j4

# HTTP 文件服务器
./build/http_server
# 浏览器访问 http://127.0.0.1:8888/

# HTTP 压测
./build/BenchmarkTest 127.0.0.1 8888 / 4 10
```

## 可执行文件

| 名称 | 说明 |
|------|------|
| `http_server` | HTTP 文件服务器（上传/下载/登录/首页） |
| `server` | Echo TCP 服务器 |
| `client` | TCP 客户端 |
| `BenchmarkTest` | 多线程 HTTP 压测工具 |
| `LogTest` | 日志系统测试 |
| `TimerTest` | 定时器测试 |
| `ThreadPoolTest` | 线程池测试 |
| `StressTest` | 压力测试客户端 |

## HTTP 路由

| 方法 | 路径 | 响应 |
|------|------|------|
| GET | `/` | 首页 (index.html) |
| GET | `/login.html` | 登录表单 |
| GET | `/fileserver` | 文件列表页（动态生成） |
| GET | `/download/<name>` | 文件下载 (Content-Disposition) |
| GET | `/delete/<name>` | 删除文件后重定向 |
| POST | `/login` | 表单解析 → 重定向 |
| POST | `/upload` | multipart/form-data 文件上传 |
| 其他 | 任意 | 404 Not Found |

## 核心设计

- **空闲超时**：`alive_` (shared_ptr<bool>) + weak_ptr 防止定时器回调访问已析构的 Connection
- **递归调度**：超时未触发时自动续期，触发时关闭连接
- **文件安全**：`isSafeFilename()` 阻止路径遍历（`..`、`/`、`\0`）
- **appendBody()**：O(1) 均摊追加替代 O(n²) 的 setBody(body()+...)
