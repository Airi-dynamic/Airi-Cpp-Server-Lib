#pragma once
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>

// StaticFileHandler：核心库内置的静态文件服务工具
//
// 提供 HTTP/1.1 标准的内容协商能力：
//   - ETag (弱校验器 W/"size-mtime")
//   - Last-Modified
//   - If-None-Match → 304 Not Modified
//   - Range → 206 Partial Content / 416 Range Not Satisfiable
//   - Content-Disposition (可选，触发浏览器下载)
//   - 自动推断 Content-Type
//
// 结合 HttpResponse::setSendFile() + Connection::sendFile() 实现零拷贝文件传输。
//
// 用法：
//   server.addPrefixRoute(HttpRequest::Method::kGet, "/static/",
//       [](const HttpRequest& req, HttpResponse* resp) {
//           std::string path = "/var/www" + req.url();
//           StaticFileHandler::serve(req, resp, path);
//       });
//
//   // 带下载名 + 自定义 Cache-Control：
//   StaticFileHandler::serve(req, resp, path, {
//       .downloadName = "report.pdf",
//       .cacheControl = "private, max-age=0, must-revalidate",
//       .enableRange = true,
//   });
class StaticFileHandler {
  public:
    struct Options {
        std::string downloadName; // 非空时触发 Content-Disposition: attachment
        std::string cacheControl; // Cache-Control 头
        bool enableRange;         // 是否支持 Range 请求
        Options() : cacheControl("public, max-age=3600"), enableRange(true) {}
    };

    // 服务单个文件，自动处理 ETag / 304 / Range / 206
    // 返回 true 表示文件存在且已填写 resp，false 表示文件不存在
    static bool serve(const HttpRequest &req, HttpResponse *resp, const std::string &path,
                      const Options &opts = Options()) {
        // 安全检查：禁止路径遍历
        if (path.find("..") != std::string::npos) {
            resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
            resp->setBody("Invalid path\n");
            return true;
        }

        struct stat st{};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return false;

        const size_t fileSize = static_cast<size_t>(st.st_size);

        // 构建 ETag 和 Last-Modified
        const std::string etag = buildWeakEtag(st);
        const std::string lastModified = toHttpDate(st.st_mtime);

        resp->addHeader("ETag", etag);
        resp->addHeader("Last-Modified", lastModified);
        resp->addHeader("Cache-Control", opts.cacheControl);
        resp->addHeader("Accept-Ranges", "bytes");

        // Content-Type
        if (!opts.downloadName.empty()) {
            resp->addHeader("Content-Disposition",
                            "attachment; filename=\"" + opts.downloadName + "\"");
            resp->setContentTypeByFilename(opts.downloadName);
        } else {
            size_t slash = path.find_last_of('/');
            resp->setContentTypeByFilename(slash == std::string::npos ? path
                                                                      : path.substr(slash + 1));
        }

        // If-None-Match → 304 Not Modified
        if (req.header("If-None-Match") == etag) {
            resp->setStatus(HttpResponse::StatusCode::k304NotModified, "Not Modified");
            resp->setBody("");
            return true;
        }

        // Range 处理
        size_t start = 0;
        size_t length = fileSize;
        const std::string rangeHeader = req.header("Range");

        if (opts.enableRange && !rangeHeader.empty()) {
            if (!parseRange(rangeHeader, fileSize, start, length)) {
                resp->setStatus(HttpResponse::StatusCode::k416RangeNotSatisfiable,
                                "Range Not Satisfiable");
                resp->addHeader("Content-Range", "bytes */" + std::to_string(fileSize));
                resp->setBody("\n");
                return true;
            }
            resp->setStatus(HttpResponse::StatusCode::k206PartialContent, "Partial Content");
            resp->addHeader("Content-Range", "bytes " + std::to_string(start) + "-" +
                                                 std::to_string(start + length - 1) + "/" +
                                                 std::to_string(fileSize));
        } else {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
        }

        // 使用 sendFile 零拷贝路径
        resp->setSendFile(path, start, length);
        return true;
    }

  private:
    // 弱 ETag：基于文件大小 + mtime，格式 W/"size-mtime"
    static std::string buildWeakEtag(const struct stat &st) {
        return "W/\"" + std::to_string(static_cast<unsigned long long>(st.st_size)) + "-" +
               std::to_string(static_cast<unsigned long long>(st.st_mtime)) + "\"";
    }

    // RFC 7231 格式的 HTTP 日期
    static std::string toHttpDate(time_t t) {
        char buf[128];
        struct tm gmt{};
#ifdef _WIN32
        gmtime_s(&gmt, &t);
#else
        gmtime_r(&t, &gmt);
#endif
        strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
        return buf;
    }

    // 解析 Range 头：bytes=start-end 或 bytes=-suffix
    // 成功返回 true 并设置 start 和 length，失败返回 false（→ 416）
    static bool parseRange(const std::string &rangeHeader, size_t fileSize, size_t &start,
                           size_t &length) {
        if (rangeHeader.size() < 7 || rangeHeader.compare(0, 6, "bytes=") != 0)
            return false;
        if (fileSize == 0)
            return false;

        std::string spec = rangeHeader.substr(6);
        size_t dash = spec.find('-');
        if (dash == std::string::npos)
            return false;

        std::string left = spec.substr(0, dash);
        std::string right = spec.substr(dash + 1);

        if (left.empty()) {
            // bytes=-N（后缀范围）
            long suffix = std::strtol(right.c_str(), nullptr, 10);
            if (suffix <= 0)
                return false;
            size_t n = static_cast<size_t>(suffix);
            if (n > fileSize)
                n = fileSize;
            start = fileSize - n;
            length = n;
            return true;
        }

        long begin = std::strtol(left.c_str(), nullptr, 10);
        if (begin < 0 || static_cast<size_t>(begin) >= fileSize)
            return false;

        size_t s = static_cast<size_t>(begin);
        size_t e = fileSize - 1;
        if (!right.empty()) {
            long end = std::strtol(right.c_str(), nullptr, 10);
            if (end < begin)
                return false;
            e = static_cast<size_t>(end);
            if (e >= fileSize)
                e = fileSize - 1;
        }

        start = s;
        length = e - s + 1;
        return true;
    }
};
