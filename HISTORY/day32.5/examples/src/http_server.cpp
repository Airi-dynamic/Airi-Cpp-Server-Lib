// =============================================================================
//  Airi-Cpp-Server-Lib 全特性演示服务器
//
//  此文件演示 day30 收官状态下库的全部核心特性：
//    - 路由表（精确路径 + 前缀路径）
//    - 中间件链（洋葱模型）
//    - per-IP 令牌桶限流（429）
//    - Bearer Token / API Key 双模式鉴权（403）
//    - CORS 跨域（OPTIONS 预检 → 204）
//    - gzip 响应压缩
//    - 静态文件服务（ETag / Range / 304 / sendfile 零拷贝）
//    - Prometheus 风格 /metrics 端点
//    - 优雅关闭（SIGINT / SIGTERM）
//
//  启动方式：
//      ./examples/http_server                    # 默认 127.0.0.1:8888
//      MYCPPSERVER_BIND_PORT=9090 ./examples/http_server
//      MYCPPSERVER_BIND_IP=0.0.0.0 ./examples/http_server
//
//  快速验证脚本（curl）：
//      curl -i http://localhost:8888/                                       # 首页
//      curl -i http://localhost:8888/api/users                              # 鉴权失败 → 403
//      curl -i -H "Authorization: Bearer demo-token-2024" \
//           http://localhost:8888/api/users                                 # → 200
//      curl -i -H "X-API-Key: api-key-001" http://localhost:8888/api/users  # → 200
//      curl -i -X OPTIONS http://localhost:8888/api/users                   # 预检 → 204
//      curl -i -H "Accept-Encoding: gzip" http://localhost:8888/            # 压缩
//      curl -i http://localhost:8888/files/readme.txt                       # 静态文件
//      curl -i -H "Range: bytes=0-99" http://localhost:8888/files/readme.txt        # 206
//      curl -s http://localhost:8888/metrics                                # Prometheus
//
//  压力测试（触发限流）：
//      for i in $(seq 1 200); do
//          curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8888/
//      done | sort | uniq -c                                                # 200 与 429 混合
// =============================================================================

#include <http/AuthMiddleware.h>
#include <http/CorsMiddleware.h>
#include <http/GzipMiddleware.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
#include <http/RateLimiter.h>
#include <http/ServerMetrics.h>
#include <http/StaticFileHandler.h>
#include <log/AsyncLogging.h>
#include <log/Logger.h>
#include <net/SignalHandler.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// ── 配置 ──────────────────────────────────────────────────────────────────────
namespace {
constexpr const char *kStaticDir = "static"; // 静态资源目录（相对 CWD）
constexpr const char *kFilesDir = "files";   // 用户文件目录（相对 CWD）

// 演示用凭据（生产环境请使用环境变量 / 密钥管理服务）
constexpr const char *kDemoBearerToken = "demo-token-2024";
constexpr const char *kDemoApiKey = "api-key-001";
} // namespace

// ── 工具：读取整个文件到 string ──────────────────────────────────────────────
static bool readFile(const std::string &path, std::string *out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    *out = oss.str();
    return true;
}

// ── 路由处理器：首页（直接返回 static/index.html，前后端分离）─────────────
//   首次请求时读盘并缓存到内存；后续请求直接命中缓存。
//   静态资源全部位于 examples/static/，由 CMake POST_BUILD 复制到 build/static/。
static void handleIndex(const HttpRequest &, HttpResponse *resp) {
    static std::string cached;
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = readFile(std::string(kStaticDir) + "/index.html", &cached); });
    if (!ok) {
        resp->setStatus(HttpResponse::StatusCode::k500InternalServerError, "Internal Server Error");
        resp->setContentType("text/plain; charset=utf-8");
        resp->setBody("index.html not found under " + std::string(kStaticDir) +
                      "/. Please run ./app_demo from the build directory or "
                      "copy examples/static/ next to the binary.\n");
        return;
    }
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("text/html; charset=utf-8");
    resp->setBody(cached);
}

// ── 路由处理器：JSON API ─────────────────────────────────────────────────────
static void handleApiUsers(const HttpRequest &, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("application/json");
    resp->setBody(R"({"users":[
  {"id":1,"name":"Alice","role":"admin"},
  {"id":2,"name":"Bob","role":"user"},
  {"id":3,"name":"Charlie","role":"user"}
]})");
}

// ── 路由处理器：健康检查（白名单，无需鉴权）──────────────────────────────────
static void handleHealth(const HttpRequest &, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("application/json");
    resp->setBody(R"({"status":"ok"})");
}

// ── 路由处理器：Prometheus 指标（白名单，无需鉴权）───────────────────────────
static void handleMetrics(const HttpRequest &, HttpResponse *resp) {
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("text/plain; version=0.0.4; charset=utf-8");
    resp->setBody(ServerMetrics::instance().toPrometheus());
}

// ── 文件管理 API（/api/files）─────────────────────────────────────────────────
//   GET    /api/files            → JSON 列出 ./files 下所有文件 [{name,size,mtime}]
//   POST   /api/files            → multipart/form-data 上传单文件，写入 ./files
//   DELETE /api/files/<name>     → 删除指定文件
// 路径穿越防护：拒绝含 '/'、'..' 或以 '.' 开头的文件名。
static bool isSafeFilename(const std::string &name) {
    if (name.empty() || name.size() > 200)
        return false;
    if (name[0] == '.')
        return false;
    return name.find('/') == std::string::npos && name.find("..") == std::string::npos;
}

static std::string jsonEscape(const std::string &s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n";  break;
        case '\r': r += "\\r";  break;
        case '\t': r += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                r += buf;
            } else {
                r += c;
            }
        }
    }
    return r;
}

static void handleApiFilesList(const HttpRequest &, HttpResponse *resp) {
    DIR *dir = opendir(kFilesDir);
    std::string body = "[";
    bool first = true;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name(ent->d_name);
            if (name == "." || name == "..")
                continue;
            std::string path = std::string(kFilesDir) + "/" + name;
            struct stat st{};
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            if (!first) body += ',';
            first = false;
            body += "{\"name\":\"" + jsonEscape(name) +
                    "\",\"size\":" + std::to_string(st.st_size) +
                    ",\"mtime\":" + std::to_string(st.st_mtime) + "}";
        }
        closedir(dir);
    }
    body += "]";
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("application/json");
    resp->setBody(body);
}

static void handleApiFilesUpload(const HttpRequest &req, HttpResponse *resp) {
    HttpRequest::MultipartFile file;
    if (!req.parseMultipart(file) || file.filename.empty()) {
        resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
        resp->setContentType("application/json");
        resp->setBody(R"({"error":"invalid multipart body"})");
        return;
    }
    if (!isSafeFilename(file.filename)) {
        resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
        resp->setContentType("application/json");
        resp->setBody(R"({"error":"unsafe filename"})");
        return;
    }
    std::string path = std::string(kFilesDir) + "/" + file.filename;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        resp->setStatus(HttpResponse::StatusCode::k500InternalServerError, "Internal Server Error");
        resp->setContentType("application/json");
        resp->setBody(R"({"error":"cannot write file"})");
        return;
    }
    ofs.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
    LOG_INFO << "[文件] 已上传 " << file.filename << "（" << file.data.size() << " 字节）";
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("application/json");
    resp->setBody("{\"ok\":true,\"name\":\"" + jsonEscape(file.filename) +
                  "\",\"size\":" + std::to_string(file.data.size()) + "}");
}

static void handleApiFilesDelete(const HttpRequest &req, HttpResponse *resp) {
    std::string url = req.url();
    constexpr const char *kPrefix = "/api/files/";
    if (url.compare(0, std::strlen(kPrefix), kPrefix) != 0) {
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setContentType("application/json");
        resp->setBody(R"({"error":"missing filename"})");
        return;
    }
    std::string name = url.substr(std::strlen(kPrefix));
    if (!isSafeFilename(name)) {
        resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
        resp->setContentType("application/json");
        resp->setBody(R"({"error":"unsafe filename"})");
        return;
    }
    std::string path = std::string(kFilesDir) + "/" + name;
    if (::unlink(path.c_str()) != 0) {
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setContentType("application/json");
        resp->setBody(R"({"error":"file not found"})");
        return;
    }
    LOG_INFO << "[文件] 已删除 " << name;
    resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
    resp->setContentType("application/json");
    resp->setBody(R"({"ok":true})");
}

// /api/files 与 /api/files/<name> 共用入口（按 method 分发）
static void handleApiFiles(const HttpRequest &req, HttpResponse *resp) {
    using M = HttpRequest::Method;
    switch (req.method()) {
    case M::kGet:    handleApiFilesList(req, resp); break;
    case M::kPost:   handleApiFilesUpload(req, resp); break;
    case M::kDelete: handleApiFilesDelete(req, resp); break;
    default:
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setContentType("application/json");
        resp->setBody(R"({"error":"method not allowed"})");
    }
}

// ── 路由处理器：静态文件（前缀路由 /static/* → ./static/，/files/* → ./files/）
//   - 自动 MIME 推断
//   - ETag + If-None-Match → 304
//   - Range → 206 分段传输
//   - sendfile() 零拷贝（Linux）/ 降级 read+send（macOS）
static void handleStatic(const HttpRequest &req, HttpResponse *resp) {
    std::string url = req.url();
    std::string fsPath;
    if (url.compare(0, 8, "/static/") == 0) {
        fsPath = std::string(kStaticDir) + "/" + url.substr(8);
    } else if (url.compare(0, 7, "/files/") == 0) {
        fsPath = std::string(kFilesDir) + "/" + url.substr(7);
    } else {
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setBody("");
        return;
    }
    StaticFileHandler::Options opts;
    opts.cacheControl = "public, max-age=3600";
    opts.enableRange = true;
    bool found = StaticFileHandler::serve(req, resp, fsPath, opts);
    if (!found) {
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setContentType("text/plain");
        resp->setBody("File not found: " + url + "\n");
    }
}

// ── 中间件：访问日志 + Metrics 计时 ──────────────────────────────────────────
//   洋葱模型最外层：next() 之前打"入"日志，next() 之后打"出"日志和耗时
static HttpServer::Middleware makeAccessLogMiddleware() {
    return [](const HttpRequest &req, HttpResponse *resp, const HttpServer::MiddlewareNext &next) {
        auto start = std::chrono::steady_clock::now();
        next();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        ServerMetrics::instance().onRequestComplete(static_cast<int>(resp->statusCode()), elapsed);
        LOG_INFO << "[access] " << req.methodString() << ' ' << req.url() << " -> "
                 << static_cast<int>(resp->statusCode()) << ' ' << elapsed << "us";
    };
}

// ── 主程序 ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    Logger::setLogLevel(Logger::WARN);  // 性能测试：关闭 INFO 热路径日志

    // 异步日志：将所有 LOG_* 输出重定向到文件（双缓冲，不阻塞业务线程）。
    // 必须在 chdir 之前创建，以便日志文件落在二进制所在目录（build/examples/）。
    // 日志文件命名格式： airi_server.<日期>.<pid>.log
    AsyncLogging asyncLog("airi_server");
    asyncLog.start();
    Logger::setOutput([&asyncLog](const char *data, int len) { asyncLog.append(data, len); });
    Logger::setFlush([]() {});  // 刷写由后端线程按间隔自动完成，前端无需毎条刷

    // 自动 chdir 到二进制所在目录，使 ./examples/http_server 在 build/ 下也能
    // 找到 static/、files/，无需用户手动 cd 进 build/examples/。
    if (argc > 0 && argv[0] != nullptr) {
        std::string exe(argv[0]);
        auto slash = exe.find_last_of('/');
        if (slash != std::string::npos) {
            std::string dir = exe.substr(0, slash);
            if (chdir(dir.c_str()) != 0) {
                std::cerr << "warning: chdir(" << dir << ") failed\n";
            }
        }
    }

    // ── HTTP Server 配置 ──────────────────────────────────────────────────────
    HttpServer::Options options;
    if (const char *envIp = std::getenv("MYCPPSERVER_BIND_IP")) {
        options.tcp.listenIp = envIp;
    }
    if (const char *envPort = std::getenv("MYCPPSERVER_BIND_PORT")) {
        options.tcp.listenPort = static_cast<uint16_t>(std::atoi(envPort));
    }
    options.tcp.ioThreads = 0; // 固定 2 IO 线程（对齐 benchmark_report.md）
    options.tcp.maxConnections = 10000;
    options.requestTimeoutSec = 15.0; // Slowloris 防护
    options.idleTimeoutSec = 60.0;
    options.autoClose = true;
    // 三层请求大小限流（防 OOM）
    options.limits.maxRequestLineBytes = 8 * 1024;
    options.limits.maxHeaderBytes = 16 * 1024;
    options.limits.maxBodyBytes = 8 * 1024 * 1024;

    HttpServer srv(options);

    // ── 中间件链（注册顺序 = 洋葱外层到内层）─────────────────────────────────
    //
    //   1. AccessLog       — 计时 + 日志（最外层）
    //   2. RateLimiter     — per-IP 令牌桶（早截断）
    //   3. CorsMiddleware  — 处理 OPTIONS 预检 / 注入 CORS 头（必须在 Auth 之前）
    //   4. AuthMiddleware  — Bearer / API Key（次内层）
    //   5. GzipMiddleware  — 响应压缩（最内层，next() 之后才能看到完整 body）
    //
    srv.use(makeAccessLogMiddleware());

    // 限流：每 IP 最多 200 个突发请求，稳态 100 RPS
    //   ⚠️ 压测模式已放宽至 1M RPS（与 ioThreads=2 / Logger::WARN 同属 benchmark 配置）；
    //      生产展示请改回 capacity=200, refillRate=100
    RateLimiter rateLimiter({/*capacity=*/2000000.0, /*refillRate=*/1000000.0});
    srv.use(rateLimiter.toMiddleware());

    // CORS：必须排在 Auth 之前，使 OPTIONS 预检请求能直接 204 返回
    CorsMiddleware cors;
    cors.allowOrigin("*")
        .allowMethods({"GET", "POST", "PUT", "DELETE", "OPTIONS"})
        .allowHeaders({"Content-Type", "Authorization", "X-API-Key"})
        .maxAge(86400);
    srv.use(cors.toMiddleware());

    // 鉴权：Bearer + API Key 双模式，/health /metrics /static/* 白名单
    AuthMiddleware auth;
    auth.addBearerToken(kDemoBearerToken);
    auth.addApiKey(kDemoApiKey);
    auth.addPublicPath("/");
    auth.addPublicPath("/health");
    auth.addPublicPath("/metrics");
    auth.addPublicPath("/favicon.ico");
    auth.addPublicPrefix("/static/");
    auth.addPublicPrefix("/files/");
    auth.addPublicPrefix("/api/files");
    srv.use(auth.toMiddleware());

    // gzip：响应体 ≥ 256 字节 + Content-Type 可压缩 + 客户端支持 → 压缩
    GzipMiddleware gzip;
    gzip.setMinSize(256);
    srv.use(gzip.toMiddleware());

    // ── 路由表 ────────────────────────────────────────────────────────────────
    //   精确路径用 addRoute；前缀匹配用 addPrefixRoute
    using M = HttpRequest::Method;
    srv.addRoute(M::kGet, "/", handleIndex);
    srv.addRoute(M::kGet, "/health", handleHealth);
    srv.addRoute(M::kGet, "/metrics", handleMetrics);
    srv.addRoute(M::kGet, "/api/users", handleApiUsers);
    srv.addRoute(M::kGet, "/api/files", handleApiFiles);
    srv.addRoute(M::kPost, "/api/files", handleApiFiles);
    srv.addPrefixRoute(M::kDelete, "/api/files/", handleApiFiles);
    srv.addPrefixRoute(M::kGet, "/static/", handleStatic);
    srv.addPrefixRoute(M::kGet, "/files/", handleStatic);

    // ── 优雅关闭 ──────────────────────────────────────────────────────────────
    Signal::signal(SIGINT, [&srv]() {
        LOG_INFO << "[主进程] 收到 SIGINT，正在停止服务...";
        srv.stop();
    });
    Signal::signal(SIGTERM, [&srv]() {
        LOG_INFO << "[主进程] 收到 SIGTERM，正在停止服务...";
        srv.stop();
    });

    // ── 启动 ──────────────────────────────────────────────────────────────────
    LOG_INFO << "──────────────────────────────────────────────────────────────";
    LOG_INFO << "  Airi-Cpp-Server-Lib 示例服务已启动，监听 http://" << options.tcp.listenIp << ":"
             << options.tcp.listenPort;
    LOG_INFO << "  路由：  /  /health  /metrics  /api/users  /api/files  /static/*  /files/*";
    LOG_INFO << "  演示 Bearer token  : " << kDemoBearerToken;
    LOG_INFO << "  演示 API key       : " << kDemoApiKey;
    LOG_INFO << "──────────────────────────────────────────────────────────────";

    srv.start();
    asyncLog.stop(); // 优雅关闭：等待后端线程将死区日志全部刷盘
    return 0;
}
