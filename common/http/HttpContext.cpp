#include "http/HttpContext.h"
#include <cctype>
#include <cstdlib>

HttpContext::HttpContext() = default;

void HttpContext::reset() {
    state_ = State::kStart;
    request_.reset();
    tokenBuf_.clear();
    colonBuf_.clear();
}

// ── 有限状态机核心 ──────────────────────────────────────────────────────────
// 以字符为单位驱动状态迁移。支持 TCP 粘包：多次调用 parse()，状态在调用间持续。
bool HttpContext::parse(const char *data, int len) {
    if (state_ == State::kInvalid || state_ == State::kComplete)
        return state_ == State::kComplete;

    const char *p = data;
    const char *end = data + len;

    while (p < end && state_ != State::kComplete && state_ != State::kInvalid) {
        char ch = *p;

        switch (state_) {
        // ── 跳过报文头部的空白行（某些代理会发空行） ─────────────────────────
        case State::kStart:
            if (ch == '\r' || ch == '\n' || std::isblank(ch)) {
                // 跳过
            } else if (std::isupper(ch)) {
                state_ = State::kMethod;
                tokenBuf_ += ch;
            } else {
                state_ = State::kInvalid;
            }
            break;

        // ── 方法名：纯大写字母，空格结束 ─────────────────────────────────────
        case State::kMethod:
            if (std::isupper(ch)) {
                tokenBuf_ += ch;
            } else if (std::isblank(ch)) {
                if (!request_.setMethod(tokenBuf_)) {
                    state_ = State::kInvalid;
                    break;
                }
                tokenBuf_.clear();
                state_ = State::kBeforeUrl;
            } else {
                state_ = State::kInvalid;
            }
            break;

        // ── 方法后的空格到 '/' ───────────────────────────────────────────────
        case State::kBeforeUrl:
            if (ch == '/') {
                tokenBuf_ += ch;
                state_ = State::kUrl;
            } else if (std::isblank(ch)) {
                // 忽略多余空格
            } else {
                state_ = State::kInvalid;
            }
            break;

        // ── 路径：遇到 '?' 转查询参数，遇到空格结束 ─────────────────────────
        case State::kUrl:
            if (ch == '?') {
                request_.setUrl(tokenBuf_);
                tokenBuf_.clear();
                state_ = State::kQueryKey;
            } else if (std::isblank(ch)) {
                request_.setUrl(tokenBuf_);
                tokenBuf_.clear();
                state_ = State::kBeforeProtocol;
            } else {
                tokenBuf_ += ch;
            }
            break;

        // ── URL 查询参数键（key=value&key2=value2）────────────────────────────
        case State::kQueryKey:
            if (ch == '=') {
                colonBuf_ = tokenBuf_; // 暂存 key
                tokenBuf_.clear();
                state_ = State::kQueryValue;
            } else if (std::isblank(ch) || ch == '\r' || ch == '\n') {
                state_ = State::kInvalid;
            } else {
                tokenBuf_ += ch;
            }
            break;

        case State::kQueryValue:
            if (ch == '&') {
                request_.addQueryParam(colonBuf_, tokenBuf_);
                colonBuf_.clear();
                tokenBuf_.clear();
                state_ = State::kQueryKey;
            } else if (std::isblank(ch)) {
                request_.addQueryParam(colonBuf_, tokenBuf_);
                colonBuf_.clear();
                tokenBuf_.clear();
                state_ = State::kBeforeProtocol;
            } else {
                tokenBuf_ += ch;
            }
            break;

        // ── 协议名（"HTTP"）──────────────────────────────────────────────────
        case State::kBeforeProtocol:
            if (std::isblank(ch)) {
                // 跳过空格
            } else if (std::isupper(ch)) {
                tokenBuf_ += ch;
                state_ = State::kProtocol;
            } else {
                state_ = State::kInvalid;
            }
            break;

        case State::kProtocol:
            if (ch == '/') {
                // "HTTP" 后跟 '/'，进入版本解析
                tokenBuf_.clear();
                state_ = State::kBeforeVersion;
            } else if (std::isupper(ch)) {
                tokenBuf_ += ch;
            } else {
                state_ = State::kInvalid;
            }
            break;

        // ── 版本号（"1.0" / "1.1"），\r 结束请求行 ───────────────────────────
        case State::kBeforeVersion:
            if (std::isdigit(ch)) {
                tokenBuf_ += ch;
                state_ = State::kVersion;
            } else {
                state_ = State::kInvalid;
            }
            break;

        case State::kVersion:
            if (ch == '\r') {
                request_.setVersion(tokenBuf_);
                tokenBuf_.clear();
                state_ = State::kHeaderKey;
                ++p; // 跳过 '\r'
                // 紧跟的 '\n' 在 kHeaderKey 的 ch=='\n' 分支中被跳过
                continue;
            } else if (std::isdigit(ch) || ch == '.') {
                tokenBuf_ += ch;
            } else {
                state_ = State::kInvalid;
            }
            break;

        // ── 请求头字段名（跳过 \n；空行 \r\n → 进入 body 判断）──────────────
        case State::kHeaderKey:
            if (ch == '\n') {
                // 跳过行末的 \n（上一行的 \r 已被消耗，这里消耗对应的 \n）
            } else if (ch == '\r') {
                // 遇到 \r 而 tokenBuf_ 为空 → 空行（headers 结束）
                if (tokenBuf_.empty()) {
                    ++p; // 跳过 '\r'
                    // 消耗空行的 '\n'，确保 p 指向 body 的第一个字节
                    if (p < end && *p == '\n') ++p;
                    std::string cl = request_.header("Content-Length");
                    if (!cl.empty() && std::atoi(cl.c_str()) > 0) {
                        state_ = State::kBody;
                    } else {
                        state_ = State::kComplete;
                    }
                    continue; // p 已手动推进，不再做末尾的 ++p
                } else {
                    state_ = State::kInvalid; // \r 出现在 key 中间，不合法
                }
            } else if (ch == ':') {
                colonBuf_ = tokenBuf_; // 暂存 header key
                tokenBuf_.clear();
                state_ = State::kHeaderValue;
            } else {
                tokenBuf_ += ch;
            }
            break;

        // ── 请求头字段值（跳过冒号后空格，\r 结束）──────────────────────────
        case State::kHeaderValue:
            if (tokenBuf_.empty() && std::isblank(ch)) {
                // 跳过 ": " 后的前导空格
            } else if (ch == '\r') {
                request_.addHeader(colonBuf_, tokenBuf_);
                colonBuf_.clear();
                tokenBuf_.clear();
                state_ = State::kHeaderKey; // 回到 kHeaderKey，等待下一行
            } else {
                tokenBuf_ += ch;
            }
            break;

        // ── 请求体：按 Content-Length 读取指定字节数 ──────────────────────────
        // 关键：使用 appendBody() 而非 setBody(body() + chunk)，避免 O(n²) 拷贝。
        // 对于大文件上传（multipart 体跨多个 TCP 段），这一点至关重要：
        //   旧代码：每次调用都生成全量 string 副本，138KB 文件 ≈ 9 次 × 平均 70KB = 630KB 无效分配
        //   新代码：body_.append() 直接扩容，均摊 O(1)
        case State::kBody: {
            // Content-Length 只需从 headers map 读取一次；bodyLen 在每次重入时重新计算但不昂贵
            const std::string &cl = request_.header("Content-Length");
            int bodyLen = cl.empty() ? 0 : std::atoi(cl.c_str());
            int alreadyRead = static_cast<int>(request_.body().size());
            int remaining = static_cast<int>(end - p);
            int toRead = std::min(bodyLen - alreadyRead, remaining);
            if (toRead > 0) {
                request_.appendBody(p, toRead); // O(1) 均摊，无副本
                p += toRead;
            } else {
                goto done; // 当前 TCP 段已读尽，等待下次 parse() 调用
            }
            if (static_cast<int>(request_.body().size()) >= bodyLen) {
                state_ = State::kComplete;
            }
            continue; // 已手动推进 p，跳过末尾的 ++p
        }

        default:
            state_ = State::kInvalid;
            break;
        }

        ++p;
    }

done:
    return state_ != State::kInvalid;
}
