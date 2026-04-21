#pragma once
#include <map>
#include <string>

// HttpResponse：构建并序列化 HTTP 响应报文。
// 用法：构造 → 设置状态码、头部、Body → 调用 serialize() 生成完整报文字符串。
class HttpResponse {
  public:
    enum class StatusCode {
        kUnknown = 0,
        k200OK = 200,
        k302Found = 302,          // 临时重定向（Location 头指定新地址）
        k400BadRequest = 400,
        k403Forbidden = 403,
        k404NotFound = 404,
        k500InternalServerError = 500,
    };

    explicit HttpResponse(bool closeConnection = false);

    // ── 状态行 ─────────────────────────────────────────────────────────────────
    void setStatus(StatusCode code, const std::string &message);
    void setStatusCode(StatusCode code) { statusCode_ = code; }
    void setStatusMessage(const std::string &msg) { statusMessage_ = msg; }

    // ── 响应头 ─────────────────────────────────────────────────────────────────
    void addHeader(const std::string &key, const std::string &value);
    void setContentType(const std::string &contentType); // 设置 Content-Type
    void setCloseConnection(bool close) { closeConnection_ = close; }
    bool closeConnection() const { return closeConnection_; }

    // ── 响应体 ─────────────────────────────────────────────────────────────────
    void setBody(const std::string &body);
    void setBody(std::string &&body);

    // 序列化为可发送的字节流（自动填充 Content-Length / Connection）
    std::string serialize() const;

  private:
    StatusCode statusCode_{StatusCode::kUnknown};
    std::string statusMessage_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool closeConnection_;
};
