#include "http/HttpRequest.h"

#include <algorithm>
#include <cctype>

HttpRequest::HttpRequest() = default;

bool HttpRequest::setMethod(const std::string &m) {
    if (m == "GET")         { method_ = Method::kGet;    return true; }
    if (m == "POST")        { method_ = Method::kPost;   return true; }
    if (m == "HEAD")        { method_ = Method::kHead;   return true; }
    if (m == "PUT")         { method_ = Method::kPut;    return true; }
    if (m == "DELETE")      { method_ = Method::kDelete; return true; }
    if (m == "OPTIONS")     { method_ = Method::kOptions; return true; }
    method_ = Method::kInvalid;
    return false;
}

std::string HttpRequest::methodString() const {
    switch (method_) {
    case Method::kGet:    return "GET";
    case Method::kPost:   return "POST";
    case Method::kHead:   return "HEAD";
    case Method::kPut:    return "PUT";
    case Method::kDelete: return "DELETE";
    case Method::kOptions:return "OPTIONS";
    default:              return "INVALID";
    }
}

void HttpRequest::setVersion(const std::string &v) {
    if (v == "1.0")      version_ = Version::kHttp10;
    else if (v == "1.1") version_ = Version::kHttp11;
    else                 version_ = Version::kUnknown;
}

std::string HttpRequest::versionString() const {
    switch (version_) {
    case Version::kHttp10: return "HTTP/1.0";
    case Version::kHttp11: return "HTTP/1.1";
    default:               return "Unknown";
    }
}

void HttpRequest::addQueryParam(const std::string &key, const std::string &value) {
    queryParams_[key] = value;
}

std::string HttpRequest::queryParam(const std::string &key) const {
    auto it = queryParams_.find(key);
    return it != queryParams_.end() ? it->second : "";
}

void HttpRequest::addHeader(const std::string &key, const std::string &value) {
    headers_[normalizeHeaderKey(key)] = value;
}

std::string HttpRequest::header(const std::string &key) const {
    auto it = headers_.find(normalizeHeaderKey(key));
    return it != headers_.end() ? it->second : "";
}

std::string HttpRequest::normalizeHeaderKey(const std::string &key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (char c : key)
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return normalized;
}

void HttpRequest::reset() {
    method_      = Method::kInvalid;
    version_     = Version::kUnknown;
    url_.clear();
    queryParams_.clear();
    headers_.clear();
    body_.clear();
}

// ── 实用工具 ───────────────────────────────────────────────────────────────

#include <sstream>

std::string HttpRequest::urlDecode(const std::string &str) {
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%') {
            if (i + 2 < str.length()) {
                int hex;
                std::istringstream iss(str.substr(i + 1, 2));
                if (iss >> std::hex >> hex) {
                    result += static_cast<char>(hex);
                    i += 2;
                } else {
                    result += str[i];
                }
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

static std::string extractBoundary(const std::string &contentType) {
    size_t pos = contentType.find("boundary=");
    if (pos == std::string::npos) return "";
    pos += 9;
    if (pos < contentType.size() && contentType[pos] == '"') {
        ++pos;
        size_t end = contentType.find('"', pos);
        return contentType.substr(pos, end == std::string::npos ? end : end - pos);
    }
    size_t end = contentType.find(';', pos);
    std::string b = contentType.substr(pos, end == std::string::npos ? end : end - pos);
    while (!b.empty() && (b.back() == ' ' || b.back() == '\t' || b.back() == '\r'))
        b.pop_back();
    return b;
}

bool HttpRequest::parseMultipart(MultipartFile &out) const {
    std::string contentType = header("Content-Type");
    std::string boundary = extractBoundary(contentType);
    if (boundary.empty()) return false;
    
    const std::string delim = "--" + boundary;
    size_t partStart = body_.find(delim);
    if (partStart == std::string::npos) return false;
    partStart += delim.size();
    if (partStart + 2 <= body_.size() && body_.substr(partStart, 2) == "\r\n") partStart += 2;
    else return false;

    size_t headersEnd = body_.find("\r\n\r\n", partStart);
    if (headersEnd == std::string::npos) return false;

    std::string headers = body_.substr(partStart, headersEnd - partStart);
    size_t fnPos = headers.find("filename=\"");
    if (fnPos == std::string::npos) return false;
    fnPos += 10;
    size_t fnEnd = headers.find('"', fnPos);
    if (fnEnd == std::string::npos) return false;
    out.filename = headers.substr(fnPos, fnEnd - fnPos);

    size_t dataStart = headersEnd + 4;
    const std::string endMark = "\r\n" + delim;
    size_t dataEnd = body_.find(endMark, dataStart);
    if (dataEnd == std::string::npos) return false;

    out.data = body_.substr(dataStart, dataEnd - dataStart);
    return !out.filename.empty();
}
