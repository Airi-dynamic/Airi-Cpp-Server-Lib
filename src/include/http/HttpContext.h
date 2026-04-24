#pragma once
#include "http/HttpRequest.h"

// HttpContext：HTTP/1.x 请求报文的有限状态机解析器。
//
// 每个 TCP 连接对应一个 HttpContext 实例（存储于 Connection::context_）。
// 数据通过 parse() 逐步喂入；可以多次调用（TCP 粘包/分包场景），
// 状态在调用间保持，直到 isComplete() 返回 true。
//
// HTTP 请求报文结构：
//   METHOD URL[?key=val&...] HTTP/1.x\r\n
//   Header-Name: value\r\n
//   ...
//   \r\n                   ← 空行
//   [body...]
//
// 完整请求解析后，调用 reset() 准备下一个请求（HTTP/1.1 keep-alive）。

class HttpContext {
  public:
    struct Limits {
        int maxRequestLineBytes{8 * 1024};
        int maxHeaderBytes{32 * 1024};
        int maxBodyBytes{10 * 1024 * 1024};
    };

    enum class State {
        kInvalid,        // 解析出错，后续数据将被忽略
        kStart,          // 报文开始
        kMethod,         // 解析请求方法（GET/POST/...）
        kBeforeUrl,      // 方法结束到 '/' 之前的空格
        kUrl,            // 解析路径
        kQueryKey,       // URL 查询参数的键（?key=）
        kQueryValue,     // URL 查询参数的值（=value&...）
        kBeforeProtocol, // URL 结束后的空格
        kProtocol,       // "HTTP"
        kBeforeVersion,  // '/' 之后
        kVersion,        // "1.0" / "1.1"
        kHeaderKey,      // 请求头字段名
        kHeaderValue,    // 请求头字段值
        kBody,           // 请求体
        kComplete,       // 解析完成
    };

    HttpContext();
    explicit HttpContext(const Limits &limits);

    void setLimits(const Limits &limits) { limits_ = limits; }
    const Limits &limits() const { return limits_; }

    // 将 data[0..len) 喂入状态机，返回 false 表示报文格式非法。
    // 可多次调用；每次调用都从上次中断的状态继续。
    // consumedBytes 输出本次实际消费的字节数（用于上层保留同包中的后续请求字节）。
    bool parse(const char *data, int len, int *consumedBytes);

    // 兼容旧调用方：若不关心消费字节，可使用该重载。
    bool parse(const char *data, int len) {
        int ignored = 0;
        return parse(data, len, &ignored);
    }

    bool isComplete() const { return state_ == State::kComplete; }
    bool isInvalid() const { return state_ == State::kInvalid; }
    bool payloadTooLarge() const { return payloadTooLarge_; }

    // 取出已解析的请求对象（isComplete() 为 true 后才有意义）
    const HttpRequest &request() const { return request_; }

    // 重置解析状态，准备下一个请求（keep-alive）
    void reset();

  private:
    State state_{State::kStart};
    HttpRequest request_;

    // 指向当前 token 起点和冒号位置的偏移量
    // （因为每次 parse() 的 data 指针可能不同，改用累积字符串保存当前 token）
    std::string tokenBuf_; // 当前正在积累的 token（方法名/URL/头字段名/值等）
    std::string colonBuf_; // 冒号左侧（头字段名 / query key）

    // 性能优化：在 headers 结束时一次性读取 Content-Length 并缓存，
    // 避免 kBody 状态每次重入时都做 unordered_map 查找（对大文件分段上传效果明显）
    int bodyLen_{0};
    int requestLineBytes_{0};
    int headerBytes_{0};
    Limits limits_{};
    bool payloadTooLarge_{false};

    void saveToken(const std::string &tok); // 根据当前状态提交 token
};
