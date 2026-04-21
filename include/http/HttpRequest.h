#pragma once
#include <map>
#include <string>

// HttpRequest：解析完成的 HTTP 请求报文。
// 由 HttpContext 状态机填充，用户通过只读接口访问。
class HttpRequest {
  public:
    enum class Method { kInvalid = 0, kGet, kPost, kHead, kPut, kDelete };
    enum class Version { kUnknown = 0, kHttp10, kHttp11 };

    HttpRequest();

    // ── 请求行 ─────────────────────────────────────────────────────────────────
    bool setMethod(const std::string &m); // 返回 false 表示未知方法
    Method method() const { return method_; }
    std::string methodString() const;

    void setVersion(const std::string &v);
    Version version() const { return version_; }
    std::string versionString() const;

    void setUrl(const std::string &url) { url_ = url; }
    const std::string &url() const { return url_; }

    // ── URL 查询参数（?key=value&key2=value2）─────────────────────────────────
    void addQueryParam(const std::string &key, const std::string &value);
    std::string queryParam(const std::string &key) const;
    const std::map<std::string, std::string> &queryParams() const { return queryParams_; }

    // ── 请求头（Header-Name: value）──────────────────────────────────────────
    void addHeader(const std::string &key, const std::string &value);
    std::string header(const std::string &key) const;
    const std::map<std::string, std::string> &headers() const { return headers_; }

    // ── 请求体 ─────────────────────────────────────────────────────────────────
    void setBody(const std::string &body) { body_ = body; }
    // 高效追加：避免 O(n²) 的 setBody(body() + chunk) 拼接，用于跨 TCP 分段累积
    void appendBody(const char *data, int len) { body_.append(data, len); }
    const std::string &body() const { return body_; }

    void reset(); // 清空所有字段，供 keep-alive 复用

  private:
    Method method_{Method::kInvalid};
    Version version_{Version::kUnknown};
    std::string url_;
    std::map<std::string, std::string> queryParams_;
    std::map<std::string, std::string> headers_;
    std::string body_;
};
