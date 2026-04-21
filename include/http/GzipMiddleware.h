#pragma once
#include "http/HttpServer.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

#ifdef HAS_ZLIB
#include <zlib.h>
#endif

// GzipMiddleware：gzip 响应压缩中间件
//
// 在路由处理完成后，对满足条件的响应体进行 gzip 压缩：
//   1. 客户端 Accept-Encoding 包含 "gzip"
//   2. Content-Type 属于可压缩类型（text/*、application/json 等）
//   3. 响应体大小超过 minSize（默认 256 字节，避免对小报文做无效压缩）
//   4. 压缩后体积确实更小（否则保留原文）
//
// 编译依赖：
//   - 需要 zlib 库；构建系统在检测到 zlib 后会定义 HAS_ZLIB
//   - 未定义 HAS_ZLIB 时此中间件为 pass-through（不压缩，不影响功能）
//
// 用法：
//   GzipMiddleware gzip;
//   gzip.setMinSize(512);                // 可选，设置最小压缩阈值
//   gzip.setCompressionLevel(6);         // 可选，zlib 压缩级别 1-9
//   server.use(gzip.toMiddleware());
class GzipMiddleware {
  public:
    GzipMiddleware() = default;

    // 设置触发压缩的最小响应体大小（字节）
    GzipMiddleware &setMinSize(size_t bytes) {
        minSize_ = bytes;
        return *this;
    }

    // 设置压缩级别（1=最快 9=最高压缩率，默认 Z_DEFAULT_COMPRESSION）
    GzipMiddleware &setCompressionLevel(int level) {
        compressionLevel_ = level;
        return *this;
    }

    // 生成可注册到 HttpServer::use() 的中间件
    HttpServer::Middleware toMiddleware() {
        return [this](const HttpRequest &req, HttpResponse *resp,
                      const HttpServer::MiddlewareNext &next) {
            // 先执行后续中间件和路由，产生响应
            next();

            // sendFile 响应不做内存压缩
            if (resp->hasSendFile())
                return;

            // 检查客户端是否接受 gzip
            std::string acceptEncoding = req.header("Accept-Encoding");
            if (acceptEncoding.find("gzip") == std::string::npos)
                return;

            // 检查 Content-Type 是否可压缩
            std::string contentType = resp->header("Content-Type");
            if (!isCompressible(contentType))
                return;

            // 检查大小阈值
            const std::string &body = resp->body();
            if (body.size() < minSize_)
                return;

            // 已经压缩过的跳过
            if (!resp->header("Content-Encoding").empty())
                return;

#ifdef HAS_ZLIB
            std::string compressed;
            if (!compress(body, &compressed))
                return;

            // 压缩后更大则放弃
            if (compressed.size() >= body.size())
                return;

            resp->setBody(std::move(compressed));
            resp->addHeader("Content-Encoding", "gzip");
            resp->addHeader("Vary", "Accept-Encoding");
#endif
        };
    }

  private:
    static bool isCompressible(const std::string &contentType) {
        if (contentType.empty())
            return false;
        std::string ct = contentType;
        std::transform(ct.begin(), ct.end(), ct.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ct.find("text/") != std::string::npos ||
               ct.find("application/json") != std::string::npos ||
               ct.find("application/javascript") != std::string::npos ||
               ct.find("application/xml") != std::string::npos ||
               ct.find("image/svg+xml") != std::string::npos;
    }

#ifdef HAS_ZLIB
    bool compress(const std::string &input, std::string *output) const {
        if (!output || input.empty())
            return false;

        z_stream zs{};
        // windowBits = 15 + 16 → gzip 格式（而非 raw deflate）
        if (deflateInit2(&zs, compressionLevel_, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
            Z_OK) {
            return false;
        }

        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        zs.avail_in = static_cast<uInt>(input.size());

        std::string out;
        out.resize(compressBound(static_cast<uLong>(input.size())));
        zs.next_out = reinterpret_cast<Bytef *>(out.data());
        zs.avail_out = static_cast<uInt>(out.size());

        int rc = deflate(&zs, Z_FINISH);
        if (rc != Z_STREAM_END) {
            deflateEnd(&zs);
            return false;
        }

        out.resize(zs.total_out);
        deflateEnd(&zs);
        *output = std::move(out);
        return true;
    }
#endif

    size_t minSize_{256};
    int compressionLevel_{-1}; // Z_DEFAULT_COMPRESSION
};
