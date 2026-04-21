#pragma once
#include <cstddef>
#include <map>
#include <string>

// HttpResponse：构建并序列化 HTTP 响应报文。
// 用法：构造 → 设置状态码、头部、Body → 调用 serialize() 生成完整报文字符串。
class HttpResponse {
  public:
    enum class StatusCode {
        kUnknown = 0,
        k204NoContent = 204,
        k206PartialContent = 206,
        k200OK = 200,
        k302Found = 302,          // 临时重定向（Location 头指定新地址）
        k304NotModified = 304,
        k400BadRequest = 400,
        k403Forbidden = 403,
        k404NotFound = 404,
        k408RequestTimeout = 408,
        k413PayloadTooLarge = 413,
        k416RangeNotSatisfiable = 416,
        k500InternalServerError = 500,
    };

    explicit HttpResponse(bool closeConnection = false);

    // ── 状态行 ─────────────────────────────────────────────────────────────────
    void setStatus(StatusCode code, const std::string &message);
    void setStatusCode(StatusCode code) { statusCode_ = code; }
    void setStatusMessage(const std::string &msg) { statusMessage_ = msg; }
    StatusCode statusCode() const { return statusCode_; }
    const std::string &statusMessage() const { return statusMessage_; }

    // ── 响应头 ─────────────────────────────────────────────────────────────────
    void addHeader(const std::string &key, const std::string &value);
    std::string header(const std::string &key) const;
    const std::map<std::string, std::string> &headers() const { return headers_; }
    void setContentType(const std::string &contentType); // 设置 Content-Type
    void setContentTypeByFilename(const std::string &filename); // 根据文件扩展名自动推断 MIME 设置 Content-Type
    
    // 实用动作：设置 302 重定向
    void setRedirect(const std::string &location);

    void setCloseConnection(bool close) { closeConnection_ = close; }
    bool closeConnection() const { return closeConnection_; }

    // ── 响应体 ─────────────────────────────────────────────────────────────────
    void setBody(const std::string &body);
    void setBody(std::string &&body);
    const std::string &body() const { return body_; }

    // 文件快路径描述：由 HttpServer 在发送响应头后调用 Connection::sendFile 完成正文传输。
    void setSendFile(const std::string &path, size_t offset, size_t count);
    bool hasSendFile() const { return useSendFile_; }
    const std::string &sendFilePath() const { return sendFilePath_; }
    size_t sendFileOffset() const { return sendFileOffset_; }
    size_t sendFileCount() const { return sendFileCount_; }
    void clearSendFile();

    // 序列化为可发送的字节流（自动填充 Content-Length / Connection）
    std::string serialize() const;

  private:
    StatusCode statusCode_{StatusCode::kUnknown};
    std::string statusMessage_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool closeConnection_;

    bool useSendFile_{false};
    std::string sendFilePath_;
    size_t sendFileOffset_{0};
    size_t sendFileCount_{0};
};
