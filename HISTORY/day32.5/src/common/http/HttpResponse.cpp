#include "http/HttpResponse.h"
#include <sstream>

HttpResponse::HttpResponse(bool closeConnection)
    : closeConnection_(closeConnection) {}

void HttpResponse::setStatus(StatusCode code, const std::string &message) {
    statusCode_ = code;
    statusMessage_ = message;
}

void HttpResponse::addHeader(const std::string &key, const std::string &value) {
    headers_[key] = value;
}

std::string HttpResponse::header(const std::string &key) const {
    auto it = headers_.find(key);
    return it == headers_.end() ? "" : it->second;
}

void HttpResponse::setContentType(const std::string &contentType) {
    addHeader("Content-Type", contentType);
}

void HttpResponse::setBody(const std::string &body) {
    clearSendFile();
    body_ = body;
}
void HttpResponse::setBody(std::string &&body) {
    clearSendFile();
    body_ = std::move(body);
}

void HttpResponse::setSendFile(const std::string &path, size_t offset, size_t count) {
    useSendFile_ = true;
    sendFilePath_ = path;
    sendFileOffset_ = offset;
    sendFileCount_ = count;
    body_.clear();
}

void HttpResponse::clearSendFile() {
    useSendFile_ = false;
    sendFilePath_.clear();
    sendFileOffset_ = 0;
    sendFileCount_ = 0;
}

// 序列化：组装状态行 + 固定头部 + 用户头部 + 空行 + body
std::string HttpResponse::serialize() const {
    std::string result;
    result.reserve(256 + body_.size());

    // 状态行
    result += "HTTP/1.1 ";
    result += std::to_string(static_cast<int>(statusCode_));
    result += ' ';
    result += statusMessage_;
    result += "\r\n";

    // Content-Length 始终包含，让客户端无需等待 TCP 关闭就能确定响应体结束
    result += "Content-Length: ";
    if (useSendFile_) {
        result += std::to_string(sendFileCount_);
    } else {
        result += std::to_string(body_.size());
    }
    result += "\r\n";

    // 连接控制头
    if (closeConnection_) {
        result += "Connection: close\r\n";
    } else {
        result += "Connection: keep-alive\r\n";
    }

    // 用户自定义头部
    for (const auto &kv : headers_) {
        result += kv.first;
        result += ": ";
        result += kv.second;
        result += "\r\n";
    }

    // 空行
    result += "\r\n";

    // Body
    if (!useSendFile_)
        result += body_;

    return result;
}

// ── 实用工具 ───────────────────────────────────────────────────────────────

void HttpResponse::setContentTypeByFilename(const std::string &filename) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) {
        setContentType("application/octet-stream");
        return;
    }
    std::string ext = filename.substr(dot + 1);
    if (ext == "html" || ext == "htm") {
        setContentType("text/html; charset=utf-8");
    } else if (ext == "css") {
        setContentType("text/css");
    } else if (ext == "js") {
        setContentType("application/javascript");
    } else if (ext == "json") {
        setContentType("application/json");
    } else if (ext == "png") {
        setContentType("image/png");
    } else if (ext == "jpg" || ext == "jpeg") {
        setContentType("image/jpeg");
    } else if (ext == "gif") {
        setContentType("image/gif");
    } else if (ext == "svg") {
        setContentType("image/svg+xml");
    } else if (ext == "txt") {
        setContentType("text/plain; charset=utf-8");
    } else if (ext == "pdf") {
        setContentType("application/pdf");
    } else {
        setContentType("application/octet-stream");
    }
}

void HttpResponse::setRedirect(const std::string &location) {
    setStatus(StatusCode::k302Found, "Found");
    addHeader("Location", location);
    setContentType("text/html; charset=utf-8");
    clearSendFile();
    setBody("<html><body>Redirecting to <a href=\"" + location + "\">" + location + "</a></body></html>");
}
