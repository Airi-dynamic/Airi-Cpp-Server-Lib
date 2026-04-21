// HTTP 文件服务器示例：演示如何在 day29 网络库 + 库级中间件之上搭建一个生产级
// HTTP 应用。所有 HTTP 标准行为（CORS / gzip / ETag / Range / 304 / 206 ...）
// 都已经下沉到 include/http/ 中的中间件与工具类，本文件只关注 **业务路由**。
//
// 应用层职责：
//   * 注册 CORS / gzip / 安全响应头中间件
//   * 提供首页、登录、文件管理 / 上传 / 下载 / 删除等业务路由
//   * 静态资源走 StaticFileHandler::serve()，自动获得 ETag/304/Range/sendfile
//
// 库层职责（include/http/）：
//   * CorsMiddleware     —— 跨域响应头 + 预检 204
//   * GzipMiddleware     —— Accept-Encoding 协商 + zlib 压缩
//   * StaticFileHandler  —— ETag / Last-Modified / 304 / Range / 416 / sendfile
//   * HttpServer         —— 中间件链 / 路由表 / 请求超时 / 大小限制
//   * Connection         —— sendfile 零拷贝、TLS（条件编译）
#include <SignalHandler.h>
#include <http/CorsMiddleware.h>
#include <http/GzipMiddleware.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
#include <http/StaticFileHandler.h>
#include <log/AsyncLogging.h>
#include <log/Logger.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// ── 配置 ──────────────────────────────────────────────────────────────────────
static const std::string kStaticDir = "static"; // 静态资源目录（相对 CWD）
static const std::string kFilesDir = "files";   // 用户文件目录（相对 CWD）

// ── 静态文件内存缓存 ──────────────────────────────────────────────────────────
// 服务器启动时预加载所有静态 HTML 文件到内存，在请求处理热路径中完全避免磁盘 I/O。
//
// 设计要点：
//   1. 在 main() 单线程阶段填充，之后只读 → 无需加锁，并发安全
//   2. key = 文件路径（相对 CWD），value = 文件内容字符串
//   3. 对内容会变化的动态页面（文件列表）不做缓存，仍按需生成
static std::unordered_map<std::string, std::string> g_staticCache;

static void preloadStaticCache(const std::string &dir) {
    DIR *dp = opendir(dir.c_str());
    if (!dp) {
        LOG_ERROR << "[static cache] cannot open dir: " << dir;
        return;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        std::string path = dir + "/" + name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            continue;
        g_staticCache[path] = {std::istreambuf_iterator<char>(ifs),
                               std::istreambuf_iterator<char>()};
        ++count;
    }
    closedir(dp);
    LOG_INFO << "[static cache] preloaded " << count << " files from " << dir;
}

// 读取文件到 string（mmap 快路径，不可用时降级 ifstream）。
// 返回 {found, data}：found=false 表示文件不存在。
static std::pair<bool, std::string> readFileSafe(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
        return {false, {}};

    struct stat st{};
    if (::fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return {false, {}};
    }

    if (st.st_size == 0) {
        ::close(fd);
        return {true, {}};
    }

    void *mapped = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        std::ifstream ifs(path, std::ios::in | std::ios::binary);
        if (!ifs)
            return {false, {}};
        return {true, {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()}};
    }

    std::string data(static_cast<const char *>(mapped), static_cast<size_t>(st.st_size));
    ::munmap(mapped, static_cast<size_t>(st.st_size));
    ::close(fd);
    return {true, std::move(data)};
}

// 优先从内存缓存读取，未命中时回落到磁盘。
static std::string readFile(const std::string &path) {
    auto it = g_staticCache.find(path);
    if (it != g_staticCache.end())
        return it->second;
    return readFileSafe(path).second;
}

// 文件名安全校验：禁止路径遍历（.. / 绝对路径）
static bool isSafeFilename(const std::string &name) {
    if (name.empty())
        return false;
    if (name.find("..") != std::string::npos)
        return false;
    if (name.find('/') != std::string::npos)
        return false;
    if (name.find('\0') != std::string::npos)
        return false;
    return true;
}

// 列出指定目录中的所有文件名（不含子目录）
static std::vector<std::string> listFiles(const std::string &dir) {
    std::vector<std::string> result;
    DIR *dp = opendir(dir.c_str());
    if (!dp)
        return result;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        struct stat st{};
        if (stat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            result.push_back(name);
    }
    closedir(dp);
    return result;
}

// 将文件列表注入 fileserver.html 模板的 <!--filelist--> 占位符
static std::string buildFileServerPage() {
    std::string tpl = readFile(kStaticDir + "/fileserver.html");
    if (tpl.empty())
        return "<h1>Template not found</h1>";

    std::ostringstream rows;
    for (const auto &name : listFiles(kFilesDir)) {
        rows << "<tr>"
             << "<td class=\"filename\">" << name << "</td>"
             << "<td><div class=\"actions\">"
             << "<a href=\"/download/" << name << "\" class=\"btn btn-download\">⬇ 下载</a>"
             << "<a href=\"/delete/" << name << "\" class=\"btn btn-delete\">🗑 删除</a>"
             << "</div></td>"
             << "</tr>\n";
    }
    if (rows.str().empty()) {
        rows << "<tr><td colspan=\"2\" class=\"empty\">暂无文件，请上传</td></tr>\n";
    }

    const std::string placeholder = "<!--filelist-->";
    size_t pos = tpl.find(placeholder);
    if (pos != std::string::npos)
        tpl.replace(pos, placeholder.size(), rows.str());
    return tpl;
}

// ── 302 重定向辅助 ─────────────────────────────────────────────────────────────
static void redirect(HttpResponse *resp, const std::string &location) {
    resp->setStatus(HttpResponse::StatusCode::k302Found, "Found");
    resp->addHeader("Location", location);
    resp->setContentType("text/html; charset=utf-8");
    resp->setBody("<html><body>Redirecting to <a href=\"" + location + "\">" + location +
                  "</a></body></html>");
}

// ── 业务路由处理函数 ───────────────────────────────────────────────────────────
//
// 注意：CORS 预检 / gzip 压缩 / 安全响应头 / ETag-304-Range-sendfile 全部
// 已经在中间件链与 StaticFileHandler 中实现，这里只关注业务语义。
static void handleRequest(const HttpRequest &req, HttpResponse *resp) {
    const std::string &url = req.url();
    LOG_INFO << req.methodString() << " " << url;

    // ── GET 路由 ───────────────────────────────────────────────────────────────
    if (req.method() == HttpRequest::Method::kGet || req.method() == HttpRequest::Method::kHead) {

        // 首页
        if (url == "/") {
            std::string body = readFile(kStaticDir + "/index.html");
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");
            resp->setBody(std::move(body));
            return;
        }

        // 登录表单页
        if (url == "/login.html") {
            std::string body = readFile(kStaticDir + "/login.html");
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");
            resp->setBody(std::move(body));
            return;
        }

        // 文件管理页（动态生成文件列表，不缓存）
        if (url == "/fileserver") {
            std::string body = buildFileServerPage();
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");
            resp->setBody(std::move(body));
            return;
        }

        // 文件下载：GET /download/<filename>
        if (url.size() > 10 && url.compare(0, 10, "/download/") == 0) {
            std::string name = HttpRequest::urlDecode(url.substr(10));
            if (!isSafeFilename(name)) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setBody("Invalid filename\n");
                return;
            }
            const std::string path = kFilesDir + "/" + name;

            // 调用库级 StaticFileHandler，自动获得 ETag/304/Range/206/sendfile
            StaticFileHandler::Options opts;
            opts.downloadName = name;
            opts.cacheControl = "private, max-age=0, must-revalidate";
            opts.enableRange = true;
            if (!StaticFileHandler::serve(req, resp, path, opts)) {
                redirect(resp, "/fileserver");
            }
            return;
        }

        // 文件删除：GET /delete/<filename>（删完重定向）
        if (url.size() > 8 && url.compare(0, 8, "/delete/") == 0) {
            std::string name = HttpRequest::urlDecode(url.substr(8));
            if (isSafeFilename(name)) {
                std::string path = kFilesDir + "/" + name;
                if (remove(path.c_str()) == 0)
                    LOG_INFO << "Deleted file: " << name;
                else
                    LOG_WARN << "Delete failed: " << name;
            }
            redirect(resp, "/fileserver");
            return;
        }

        // 其他 GET：尝试作为静态资源服务
        {
            std::string stripped = HttpRequest::urlDecode(url.substr(1)); // 去掉前导 '/'，并解码
            if (!stripped.empty() && stripped.find("..") == std::string::npos) {
                std::string path = kStaticDir + "/" + stripped;
                StaticFileHandler::Options opts;
                opts.cacheControl = "public, max-age=60";
                opts.enableRange = true;
                if (StaticFileHandler::serve(req, resp, path, opts)) {
                    return;
                }
            }
        }

        // 404
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setContentType("text/plain");
        resp->setCloseConnection(true);
        resp->setBody("404 Not Found\n");
        return;
    }

    // ── POST 路由 ──────────────────────────────────────────────────────────────
    if (req.method() == HttpRequest::Method::kPost) {

        // POST /echo → 原样回显请求体（调试用）
        if (url == "/echo") {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/plain; charset=utf-8");
            resp->setBody(req.body());
            return;
        }

        // POST /login → 解析 application/x-www-form-urlencoded 表单
        if (url == "/login") {
            const std::string &body = req.body();
            auto extract = [&](const std::string &key) -> std::string {
                size_t pos = body.find(key + "=");
                if (pos == std::string::npos)
                    return "";
                pos += key.size() + 1;
                size_t end = body.find('&', pos);
                return body.substr(pos, end == std::string::npos ? end : end - pos);
            };
            std::string username = extract("username");
            std::string password = extract("password");

            LOG_INFO << "Login attempt: user=" << username;

            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/html; charset=utf-8");

            if (username == "admin" && password == "admin123") {
                resp->setBody(
                    "<html><body style='font-family:sans-serif;text-align:center;padding:60px'>"
                    "<h2 style='color:#48bb78'>✓ 登录成功</h2>"
                    "<p>欢迎，<strong>" +
                    username +
                    "</strong>！</p>"
                    "<p><a href='/'>返回首页</a></p>"
                    "</body></html>");
            } else {
                resp->setBody(
                    "<html><body style='font-family:sans-serif;text-align:center;padding:60px'>"
                    "<h2 style='color:#fc8181'>✗ 用户名或密码错误</h2>"
                    "<p><a href='/login.html'>重试</a> &nbsp;|&nbsp; <a href='/'>首页</a></p>"
                    "</body></html>");
            }
            return;
        }

        // POST /upload → multipart/form-data 文件上传
        if (url == "/upload") {
            HttpRequest::MultipartFile file;
            if (!req.parseMultipart(file) || file.filename.empty()) {
                LOG_WARN << "Upload failed: bad multipart body";
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setBody("Upload failed: invalid multipart data\n");
                return;
            }

            if (!isSafeFilename(file.filename)) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setBody("Invalid filename\n");
                return;
            }

            std::string savePath = kFilesDir + "/" + file.filename;
            std::ofstream ofs(savePath, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!ofs) {
                LOG_ERROR << "Cannot write file: " << savePath;
                resp->setStatus(HttpResponse::StatusCode::k500InternalServerError,
                                "Internal Server Error");
                resp->setBody("Failed to save file\n");
                return;
            }
            ofs.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
            ofs.close();

            LOG_INFO << "Uploaded: " << file.filename << " (" << file.data.size() << " bytes)";

            redirect(resp, "/fileserver");
            return;
        }

        // 未知 POST 路由
        resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
        resp->setBody("Not Found\n");
        return;
    }

    // 其他 HTTP 方法暂不支持（OPTIONS 已被 CorsMiddleware 拦截）
    resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
    resp->setBody("Method Not Supported\n");
}

// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<AsyncLogging> g_asyncLog;

void asyncOutputLog(const char *data, int len) { g_asyncLog->append(data, len); }

static std::string envOr(const char *key, const std::string &fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    return v;
}

static int envIntOr(const char *key, int fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    errno = 0;
    char *end = nullptr;
    long parsed = std::strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0')
        return fallback;
    if (parsed > std::numeric_limits<int>::max() || parsed < std::numeric_limits<int>::min())
        return fallback;
    return static_cast<int>(parsed);
}

static size_t envSizeOr(const char *key, size_t fallback) {
    const int parsed = envIntOr(key, static_cast<int>(fallback));
    if (parsed <= 0)
        return fallback;
    return static_cast<size_t>(parsed);
}

static uint16_t envPortOr(const char *key, uint16_t fallback) {
    int p = envIntOr(key, static_cast<int>(fallback));
    if (p <= 0 || p > 65535)
        return fallback;
    return static_cast<uint16_t>(p);
}

static bool envBoolOr(const char *key, bool fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    std::string s(v);
    if (s == "1" || s == "true" || s == "TRUE" || s == "on" || s == "ON")
        return true;
    if (s == "0" || s == "false" || s == "FALSE" || s == "off" || s == "OFF")
        return false;
    return fallback;
}

static double envDoubleOr(const char *key, double fallback) {
    const char *v = std::getenv(key);
    if (!v || v[0] == '\0')
        return fallback;
    errno = 0;
    char *end = nullptr;
    double parsed = std::strtod(v, &end);
    if (errno != 0 || end == v || *end != '\0')
        return fallback;
    if (parsed <= 0.0)
        return fallback;
    return parsed;
}

int main() {
    mkdir("log", 0755);

    g_asyncLog = std::make_unique<AsyncLogging>("log/http_server", 50 * 1024 * 1024, 3);
    g_asyncLog->start();

    Logger::setOutput(asyncOutputLog);
    Logger::setLogLevel(Logger::INFO);

    preloadStaticCache(kStaticDir);

    HttpServer::Options options;
    options.tcp.listenIp = envOr("MYCPPSERVER_BIND_IP", "127.0.0.1");
    options.tcp.listenPort = envPortOr("MYCPPSERVER_BIND_PORT", 8888);
    options.tcp.ioThreads = envIntOr("MYCPPSERVER_IO_THREADS", 0);
    options.tcp.maxConnections = envSizeOr("MYCPPSERVER_MAX_CONNECTIONS", 10000);
    options.tcp.tls.enabled = envBoolOr("MYCPPSERVER_TLS_ENABLE", false);
    options.tcp.tls.certFile = envOr("MYCPPSERVER_TLS_CERT_FILE", "");
    options.tcp.tls.keyFile = envOr("MYCPPSERVER_TLS_KEY_FILE", "");
    options.autoClose = envBoolOr("MYCPPSERVER_AUTO_CLOSE", false);
    options.idleTimeoutSec = envDoubleOr("MYCPPSERVER_IDLE_TIMEOUT_SEC", 60.0);
    options.requestTimeoutSec = envDoubleOr("MYCPPSERVER_REQUEST_TIMEOUT_SEC", 15.0);
    options.limits.maxRequestLineBytes = envIntOr("MYCPPSERVER_MAX_REQUEST_LINE_BYTES", 8 * 1024);
    options.limits.maxHeaderBytes = envIntOr("MYCPPSERVER_MAX_HEADER_BYTES", 32 * 1024);
    options.limits.maxBodyBytes = envIntOr("MYCPPSERVER_MAX_BODY_BYTES", 10 * 1024 * 1024);

    HttpServer srv(options);

    // ── 中间件链：所有 HTTP 标准行为都来自库级中间件 ───────────────────────────
    // 1) CORS：跨域响应头 + 预检请求自动 204
    CorsMiddleware cors;
    cors.allowOrigin("*")
        .allowMethods({"GET", "POST", "PUT", "DELETE", "OPTIONS", "HEAD"})
        .allowHeaders(
            {"Content-Type", "Authorization", "If-None-Match", "If-Modified-Since", "Range"})
        .maxAge(600);
    srv.use(cors.toMiddleware());

    // 2) gzip 压缩（可选，需编译时存在 zlib，由 CMake 自动检测）
    GzipMiddleware gzip;
    gzip.setMinSize(64);
    srv.use(gzip.toMiddleware());

    // 3) 通用安全响应头（轻量，不值得抽象成独立类）
    srv.use([](const HttpRequest &, HttpResponse *resp, const HttpServer::MiddlewareNext &next) {
        next();
        resp->addHeader("X-Content-Type-Options", "nosniff");
        resp->addHeader("Referrer-Policy", "no-referrer");
    });

    srv.setHttpCallback(handleRequest);

    Signal::signal(SIGINT, [&] {
        static std::atomic_flag fired = ATOMIC_FLAG_INIT;
        if (fired.test_and_set())
            return;
        std::cout << "\n[http_server] Shutting down.\n";
        LOG_INFO << "[http_server] Shutting down.";
        srv.stop();
        g_asyncLog->stop();
    });

    LOG_INFO << "[http_server] config: listen=" << options.tcp.listenIp << ":"
             << options.tcp.listenPort << " ioThreads=" << options.tcp.ioThreads
             << " maxConnections=" << options.tcp.maxConnections
             << " autoClose=" << (options.autoClose ? "true" : "false")
             << " idleTimeoutSec=" << options.idleTimeoutSec;
    LOG_INFO << "[http_server] Listening on http://" << options.tcp.listenIp << ":"
             << options.tcp.listenPort;
    LOG_INFO << "  Home:        http://" << options.tcp.listenIp << ":" << options.tcp.listenPort
             << "/";
    LOG_INFO << "  File server: http://" << options.tcp.listenIp << ":" << options.tcp.listenPort
             << "/fileserver";
    LOG_INFO << "  Login:       http://" << options.tcp.listenIp << ":" << options.tcp.listenPort
             << "/login.html";

    srv.start();
    return 0;
}
