# Day 31 — WebSocket 协议支持（RFC 6455）

> **⚠️ 实验分支说明**　本日的内容是在 day30 生产发布之后的技术实验，未合入主代码树。
>
> 主仓库的可编译代码停留在 **day30** 状态；day31–day36 的可运行快照仅作为参考完整保留在 `HISTORY/day31` … `HISTORY/day36` 中。
>
> 这篇日志的所有代码路径与命令，默认均应补上 `HISTORY/day3X/` 前缀后阅读 / 运行。


> **今日目标**：在 Day 30 完成的 HTTP 框架之上，从零写一套自带 SHA-1/Base64、能完成 RFC 6455 握手、能编解码所有 5 种帧、能正确处理掩码 / 分片 / 控制帧 / 关闭握手的 WebSocket 协议层；最终接入 `Connection`，让上层业务通过 `WebSocketHandler` 4 个回调（onOpen/onMessage/onPing/onClose）就能写一个聊天室服务。
> **基于**：Day 30（GTest 迁移与项目收官）。**进入**：Phase 5「协议 / 协程 / 高性能 IO 实验区」。

---

## 0. 今日构建目标

WebSocket 不是一个"加个 if 就能用"的特性——它是 HTTP 之上的一层独立协议，状态机、帧格式、掩码、分片、关闭握手都有严格的 RFC 约束。今天你将按以下顺序逐步把它从空白搭起来，每一步都可以独立编译并写一个 GTest 用例验证。

**构建清单（按顺序）：**

1. **§2** — 自己写一个内置 SHA-1：64 字节分块、80 轮压缩、padding 规则
2. **§3** — 自己写一个 Base64 编码器：3→4 字节扩张 + 末尾补 `=`
3. **§4** — 实现 `computeAcceptKey()`：拼接魔数 GUID → SHA-1 → Base64
4. **§5** — 在 `WebSocket.h` 里设计帧结构：`WsOpcode` 枚举 + `WebSocketFrame` 结构体
5. **§6** — 写 `encodeFrame()` 与 `applyMask()`：把负载打包成符合 RFC 帧格式的字节流
6. **§7** — 写 `decodeFrame()`：处理 7-bit / 16-bit / 64-bit 三种长度形式 + 错误检测
7. **§8** — 写 `handleWebSocketData()`：完整的接收侧状态机，覆盖控制帧、分片、关闭握手
8. **§9** — 写 `WebSocketConnection` 外观类：暴露 `sendText/sendBinary/sendPing/sendClose` 给业务
9. **§10** — 用 21 个 GTest 用例钉死所有边界，最后接 wscat 跑一次端到端

**说明**：代码块前的「来自 `HISTORY/day31/...`」标注意为「将以下代码写入该文件的对应位置」，跟着每步动手输入即可。

---

## 1. 今天要解决的几个问题

### 1.1 Day 30 之后 HTTP 模型的真实瓶颈

Day 26-30 我们打磨出了一套相当完整的 HTTP/1.1 框架：路由表、中间件链、静态文件、ETag、Range、Gzip、CORS——基本是一个能跑生产业务的 Web 服务器。但 HTTP 的"请求-响应"骨架决定了它有个跨不过的坎：**服务端不能主动推送数据给客户端**。

想象三个具体业务：

- **聊天室**：用户 A 发消息后，必须把消息推给已连接的用户 B、C、D。Day 30 的 HTTP 做不到——B 必须不停发 `GET /messages?since=last_id` 查询。
- **股票行情**：行情每秒变 10 次，客户端要么轮询（浪费带宽 + 延迟最低也是轮询周期）要么用 SSE（只能服务端 → 客户端单向）。
- **协作编辑**：两个用户同时编辑同一文档，需要把对方的光标 / 增量编辑实时推给对方。HTTP 做这事要么开 30 秒 long polling（一次只能传一条消息），要么走 SSE（客户端发命令还得另开 HTTP 连接）。

三个场景的根本诉求一致：**在一条 TCP 连接上，让两端都能随时往对方发字节，而且开销要小**。

### 1.2 各方案的对比

| 方案 | 服务端→客户端 | 客户端→服务端 | 每条消息开销 | 适用 |
|------|----------|----------|----------|-----|
| 轮询 (Polling) | 受轮询周期限制 | 即时 | 1 个完整 HTTP 请求/响应 | 极少更新的场景 |
| 长轮询 (Long Polling) | 收到数据立即推 | 即时 | 1 完整 HTTP 请求/响应 | 普通推送，但每次后要重建连接 |
| SSE | 单向流式 | 必须另开 HTTP | 1 行 `data: ...\n\n` | 单向推送（行情、日志） |
| **WebSocket** | 即时 | 即时 | 2-14 字节帧头 + payload | 双向交互 |

WebSocket 的赢面在最后一列：一帧最小只有 2 字节头，比 HTTP 一行响应（至少几十字节）小一个数量级。**这是为什么聊天室、协作编辑、游戏几乎只用 WebSocket**。

### 1.3 解法：在现有 HTTP 框架旁边加一层协议

WebSocket 设计上很巧妙——它**借用 HTTP 完成握手**，握手成功后**抛弃 HTTP，切换到自己的二进制帧协议**：

```
客户端                                         服务端
   │                                              │
   │── GET /chat HTTP/1.1                        │
   │   Upgrade: websocket                        │
   │   Connection: Upgrade                       │
   │   Sec-WebSocket-Key: dGhlIHNhbXBsZQ==      │
   │   Sec-WebSocket-Version: 13                 │
   │─────────────────────────────────────────────▶│
   │                                              │   服务端：
   │                                              │   1. 检查 Upgrade/Connection
   │                                              │   2. 取 Sec-WebSocket-Key
   │                                              │   3. accept = base64(sha1(key + GUID))
   │                                              │
   │   HTTP/1.1 101 Switching Protocols          │
   │   Upgrade: websocket                        │
   │   Connection: Upgrade                       │
   │◀───── Sec-WebSocket-Accept: <accept>        │
   │                                              │
   ╪══════ 此后这条 TCP 连接走 WebSocket 帧 ══════╪
   │                                              │
   │── [Text frame: "hello"]──────────────────────▶│
   │◀────────────[Text frame: "hi back"]──────────│
   │── [Ping frame]──────────────────────────────▶│
   │◀────────────────────────────[Pong frame]─────│
   │── [Close frame: 1000]───────────────────────▶│
   │◀────────────────────[Close frame: 1000]──────│
   │── (TCP FIN)─────────────────────────────────▶│
```

这意味着我们要解决的核心是 4 件事：

1. **握手算密钥**：服务端必须算出一个**确定的** `Sec-WebSocket-Accept` 值，证明自己懂协议。算法 = `Base64(SHA1(客户端 key + 一个固定 GUID))`。
2. **帧编解码**：握手完成后，所有数据都包在 2-14 字节的二进制帧里，要会拼也要会拆。
3. **掩码**：浏览器发出的帧**必须**带 4 字节随机掩码（防代理缓存），服务端要会解掩码；服务端发出的帧**禁止**带掩码。
4. **关闭握手**：双方各发一个 Close 帧才能优雅关闭，不能裸 TCP FIN。

### 1.4 今日方案概览

为了**不引入任何第三方依赖**（OpenSSL 太重，protobuf 不相关），SHA-1 和 Base64 我们自己写。两个算法都是 100 行内的纯函数，写完就是一辈子的事。

整个 WebSocket 模块拆成 3 个文件：

| 文件 | 内容 | 行数 |
|-----|------|------|
| `include/http/WebSocket.h` | 类型定义（`WsOpcode`、`WebSocketFrame`、`WebSocketHandler`、`WebSocketConnectionContext`、`WebSocketConnection`）+ `ws::` 命名空间工具函数声明 | 153 |
| `common/http/WebSocket.cpp` | SHA-1 类、Base64 编码、握手密钥、帧编解码、`handleWebSocketData` 接收状态机 | 449 |
| `test/WebSocketTest.cpp` | 21 个 GTest 用例 | 351 |

### 1.5 今日文件变更全图

```
include/http/
└── WebSocket.h           [新增]

common/http/
└── WebSocket.cpp         [新增]

test/
└── WebSocketTest.cpp     [新增]

CMakeLists.txt            [修改：加入 WebSocketTest 目标]
```

下一节起，我们正式按建造顺序动手。

---

## 2. 第 1 步 — 写一个内置的 SHA-1

### 2.1 问题背景

WebSocket 握手要求服务端把客户端发来的 `Sec-WebSocket-Key`（一段 16 字节随机数据的 Base64）拼上一段固定的 GUID，做 SHA-1 哈希，再 Base64 一次，回传给客户端。**这是 RFC 强制的、不可绕过的步骤**——没有它，浏览器会拒绝完成握手。

为什么不直接 `#include <openssl/sha.h>`？三个理由：

1. OpenSSL 在 macOS 上要 Homebrew 装、在 Ubuntu 上路径不一致、在容器镜像里又得多装一层——构建复杂度爆炸。
2. 我们只需要 SHA-1 这一个算法，且只在握手时用一次（每个连接一次），完全不在乎性能。
3. SHA-1 的实现就 80 行——RFC 3174 把伪代码写得清清楚楚，自己写一遍也是教育意义。

### 2.2 SHA-1 的算法骨架

SHA-1 的本质是「把任意长度输入压缩成 160 位摘要」。它按 64 字节为一个 block 处理：

```
输入字节流 ──切成─▶ [64 字节 block 1][64 字节 block 2]...[最后不足 64 字节，要 padding]
                          │                  │
                          ▼                  ▼
                   processBlock(b1)   processBlock(b2)   ...
                          │                  │
                          └─── 累加更新 5 个 32-bit 状态 h[0..4] ───┘
                                              │
                                              ▼
                              digest = 拼接 h[0]||h[1]||h[2]||h[3]||h[4]  (20 字节)
```

`processBlock` 内部是 80 轮"压缩函数"，每轮做一次旋转 + 异或 + 加法。这一步纯粹的 bit twiddling，写起来机械但只要按 RFC 抄就不会错。

### 2.3 打开 WebSocket.cpp：定义 SHA1 类

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 12–40，初始化与 update）：

```cpp
namespace {

class SHA1 {
  public:
    SHA1() { reset(); }

    void update(const uint8_t *data, size_t len) {
        while (len > 0) {
            size_t space = 64 - bufLen_;
            size_t chunk = std::min(len, space);
            std::memcpy(buf_ + bufLen_, data, chunk);
            bufLen_ += chunk;
            data += chunk;
            len -= chunk;
            totalBits_ += chunk * 8;
            if (bufLen_ == 64) {
                processBlock(buf_);
                bufLen_ = 0;
            }
        }
    }

    void update(const std::string &s) {
        update(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }
    // digest() 见下文 ...
  private:
    uint32_t h_[5]{};
    uint8_t  buf_[64]{};
    size_t   bufLen_{0};
    uint64_t totalBits_{0};

    void reset() {
        h_[0] = 0x67452301;
        h_[1] = 0xEFCDAB89;
        h_[2] = 0x98BADCFE;
        h_[3] = 0x10325476;
        h_[4] = 0xC3D2E1F0;
        bufLen_ = 0;
        totalBits_ = 0;
    }
    // processBlock 见 §2.5
};
```

设计点：

- **匿名 namespace**：SHA1 是实现细节，不暴露给外部。
- **流式 update**：调用方可以多次喂数据，内部用 `buf_[64]` 攒满一个 block 才压缩，免去"必须一次性传全部数据"的约束。
- **5 个魔数初始向量** `h_[0..4]` = `0x67452301` 等等：RFC 3174 直接写死的，背景是 SHA-1 设计者（NSA）当年从某些"看起来无规律"的常量里挑的；我们抄就是。
- **`totalBits_`**：单独维护，因为 padding 阶段需要往末尾追加这个长度。

### 2.4 写 digest()：padding + 输出

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 42–63）：

```cpp
std::string digest() {
    uint64_t bits = totalBits_;

    // 填充：先追加 0x80，再补零到 bufLen_ ≡ 56 (mod 64)
    static const uint8_t padding[64] = {0x80};
    size_t padLen = (bufLen_ < 56) ? (56 - bufLen_) : (120 - bufLen_);
    update(padding, padLen);

    // 追加原始长度（大端 64-bit）—— 占 8 字节，刚好把 block 填满到 64
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i)
        lenBytes[i] = static_cast<uint8_t>((bits >> ((7 - i) * 8)) & 0xFF);
    update(lenBytes, 8);

    // 输出 20 字节摘要
    std::string result(20, '\0');
    for (int i = 0; i < 5; ++i) {
        result[i * 4 + 0] = static_cast<char>((h_[i] >> 24) & 0xFF);
        result[i * 4 + 1] = static_cast<char>((h_[i] >> 16) & 0xFF);
        result[i * 4 + 2] = static_cast<char>((h_[i] >>  8) & 0xFF);
        result[i * 4 + 3] = static_cast<char>((h_[i]      ) & 0xFF);
    }
    return result;
}
```

**为什么 padding 一定要这样**：SHA-1 强制最后一个 block 的最后 8 字节必须是"原始长度"。前面的 padding 规则保证了"原始长度"始终能装进最后 8 字节槽位：

- 若 `bufLen_ < 56`：先填 `(56 - bufLen_)` 字节的 `0x80 + 全 0`，再填 8 字节长度，刚好凑齐 64。
- 若 `bufLen_ >= 56`：先填 `(120 - bufLen_)` 字节，跨过当前 block 边界，到下一个 block 的第 56 字节，再填 8 字节长度。

这两条规则用同一个 `padLen` 表达式涵盖，是 RFC 3174 §4 的标准写法。

### 2.5 写 processBlock：80 轮压缩

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 79–122）：

```cpp
static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void processBlock(const uint8_t block[64]) {
    uint32_t w[80];
    // ① 把 64 字节切成 16 个 32-bit 字（大端读法）
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4    ]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) <<  8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    // ② 扩展到 80 个字（消息扩展）
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    // ③ 初始化工作变量
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

    // ④ 80 轮压缩
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f =  b ^ c ^ d;                  k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f =  b ^ c ^ d;                  k = 0xCA62C1D6; }
        uint32_t temp = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }

    // ⑤ 累加回 h_
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}
```

**这一段不需要理解**——它是 RFC 3174 §6.1 的字面翻译。20 + 20 + 20 + 20 = 80 轮，每 20 轮换一次 `f` 和 `k`。读者唯一需要确认的是：自己抄的代码与 RFC 完全一致；用 §10 的测试向量验证就行。

### 2.6 验证：追踪一次 SHA-1 计算

测试 `SHA1_ABC`（输入 `"abc"`，预期 16 进制摘要 `a9993e364706816aba3e25717850c26c9cd0d89d`）：

```
SHA1 ctx;
ctx.update("abc")             // 3 字节，bufLen_=3, totalBits_=24, 不满 64 不触发 processBlock
auto raw = ctx.digest();      // 进入 padding 路径
   ├─ bufLen_=3 < 56, padLen = 53
   ├─ update(padding, 53)     // 写入 0x80 + 52 个 0, bufLen_=56
   ├─ update(lenBytes, 8)     // 写入大端 24, bufLen_=64 → processBlock 触发
   │     processBlock 内部：
   │       w[0..15] 来自 "abc\x80\x00...0\x00\x00\x00\x18"
   │       w[16..79] 由 rotl(...) 扩展
   │       80 轮压缩后 h_[0..4] = {0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d}
   │     bufLen_ 重置 0
   └─ 输出 result = 20 字节，前 4 字节 = h_[0] 大端拼回去
```

`raw` 最终是二进制，转 hex 就是 RFC 标准向量。**测试向量对得上，等于证明了 SHA-1 实现正确**——这是密码学算法验证的金科玉律。

---

## 3. 第 2 步 — 写 Base64 编码

### 3.1 问题背景

握手响应 `Sec-WebSocket-Accept` 必须是 ASCII 文本（HTTP header 的限制），而 SHA-1 摘要是 20 字节二进制，里面有不可打印字符。Base64 是把任意字节流编码成 64 字符（A-Z a-z 0-9 + /）的最常用方案，编码后体积膨胀 33%。

我们只需要"编码"，不需要"解码"——客户端发的 `Sec-WebSocket-Key` 我们当**不透明字符串**用，原样拼到 SHA-1 输入里。

### 3.2 算法骨架

每 3 字节输入 → 24 bit → 切成 4 段 6 bit → 查表得 4 个字符。最后不足 3 字节的，补 `=` 标记：

```
输入 "Man"     = 0x4D 0x61 0x6E
              = 01001101 01100001 01101110
3 字节切 4 段 = 010011 010110 000101 101110
              = 19     22     5      46
查表得到      = T      W      F      u
输出          = "TWFu"

输入 "Ma"     = 0x4D 0x61 + 末尾补 0
3 字节切 4 段 = 010011 010110 0001|00 ──     (最后 6 bit 不完整→第 4 段算空)
              = 19     22     4
查表          = T      W      E      =
输出          = "TWE="

输入 "M"      → 输出 "TQ=="
```

### 3.3 写 base64Encode

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 169–188）：

```cpp
const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string &input) {
    std::string result;
    const auto *data = reinterpret_cast<const uint8_t *>(input.data());
    size_t len = input.size();
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len)
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len)
            n |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        result.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? kBase64Chars[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? kBase64Chars[n         & 0x3F] : '=');
    }
    return result;
}
```

**关键点**：

- 用 `uint32_t n` 把最多 3 字节装进同一个整数的高位，统一成同一套移位运算，避免分支爆炸。
- `result.reserve(((len + 2) / 3) * 4)`：精确预算 → 编码过程零次 realloc。
- `=` 补位的逻辑直接内联到三元运算符里——`i + 1 >= len` 说明第 2 字节缺，对应位置出 `=`；`i + 2 >= len` 同理。

### 3.4 写一个最小测试

来自 [HISTORY/day31/src/test/WebSocketTest.cpp](../HISTORY/day31/src/test/WebSocketTest.cpp)（节选）：

```cpp
TEST(WebSocketTest, Base64_Basic) {
    EXPECT_EQ(ws::base64Encode("Man"), "TWFu");
    EXPECT_EQ(ws::base64Encode("Ma"),  "TWE=");
    EXPECT_EQ(ws::base64Encode("M"),   "TQ==");
    EXPECT_EQ(ws::base64Encode(""),    "");
}
```

跑通这 4 条，可以认为编码器没错——再多的随机串也是同样路径。

---

## 4. 第 3 步 — 实现握手密钥计算与 Upgrade 检测

### 4.1 问题背景

RFC 6455 §4.2.2 定义了一个看似奇怪的步骤：

> The server MUST take the value of the `|Sec-WebSocket-Key|` field in the client's handshake and concatenate this with the GUID `258EAFA5-E914-47DA-95CA-C5AB0DC85B11` ... A SHA-1 hash, base64-encoded, of this concatenation is then returned in the server's handshake.

为什么要这么绕？因为 WebSocket 设计者想**确认服务端真的"懂" WebSocket 协议**——一个普通 HTTP 服务器收到带 `Upgrade: websocket` 的请求，可能误返回 200 OK + 一段 HTML。而这套 GUID + SHA-1 + Base64 的"认证操作"，普通 HTTP 服务器不可能凑巧做对。

### 4.2 写 computeAcceptKey

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 190–195）：

```cpp
std::string computeAcceptKey(const std::string &clientKey) {
    // RFC 6455 §1.3 规定的魔数 GUID
    static const std::string kMagicGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return base64Encode(sha1(clientKey + kMagicGUID));
}
```

三行实现，直接复用前两步写好的 `sha1` 和 `base64Encode`。`static const std::string kMagicGUID` 放函数内 → 第一次调用时初始化，后续调用零开销。

### 4.3 写 isUpgradeRequest 与 buildHandshakeResponse

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 269–290）：

```cpp
bool isUpgradeRequest(const std::string &upgradeHeader,
                      const std::string &connectionHeader) {
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    return toLower(upgradeHeader) == "websocket" &&
           toLower(connectionHeader).find("upgrade") != std::string::npos;
}

std::string buildHandshakeResponse(const std::string &acceptKey) {
    std::string resp;
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: " + acceptKey + "\r\n";
    resp += "\r\n";
    return resp;
}
```

**两个细节**：

1. **HTTP header 是大小写不敏感的**——浏览器可能发 `upgrade: WebSocket` 也可能发 `Upgrade: websocket`，必须用 `tolower` 归一化再比较。
2. **`Connection` 头部可能是 `Upgrade, keep-alive`**，所以判断"包含" `upgrade` 而不是相等。

### 4.4 验证：用 RFC 6455 测试向量

来自 [HISTORY/day31/src/test/WebSocketTest.cpp](../HISTORY/day31/src/test/WebSocketTest.cpp)（核心向量）：

```cpp
TEST(WebSocketTest, HandshakeAcceptKey_RFC6455) {
    // RFC 6455 §1.3 的测试向量：客户端 key 是 "dGhlIHNhbXBsZSBub25jZQ=="
    // 期望服务端响应 key 为 "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    std::string acceptKey = ws::computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
    EXPECT_EQ(acceptKey, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
```

这一个用例同时验证了 §2（SHA-1）+ §3（Base64）+ §4（拼接）三个步骤。RFC 把测试向量直接写在标准里，就是为了让"对得上 = 实现正确"成为可能。

---

## 5. 第 4 步 — 设计帧结构与操作码

### 5.1 问题背景

握手完成后，TCP 连接进入"WebSocket 模式"。从这一刻起，所有数据都包在帧里。RFC 6455 §5.2 定义的帧结构：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
+-+-+-+-+-------+-+-------------+-------------------------------+
|     Masking-key (if MASK=1)   |          Payload Data         |
+-------------------------------+-------------------------------+
```

最小帧 = 2 字节头 + 0 字节负载。最大帧 = 14 字节头（fin/op + 127 长度标记 + 8 字节真实长度 + 4 字节掩码） + 2^63 字节负载。

我们要把这个比特格局映射成 C++ 类型，让上层代码不必直接摸字节。

### 5.2 打开 WebSocket.h：定义 WsOpcode 枚举

来自 [HISTORY/day31/src/include/http/WebSocket.h](../HISTORY/day31/src/include/http/WebSocket.h)（行 33–43）：

```cpp
enum class WsOpcode : uint8_t {
    kContinuation = 0x0,
    kText         = 0x1,
    kBinary       = 0x2,
    // 0x3-0x7 保留（数据帧）
    kClose        = 0x8,
    kPing         = 0x9,
    kPong         = 0xA,
    // 0xB-0xF 保留（控制帧）
};
```

**关键设计**：

- `enum class` 强类型，避免 `int` 隐式转换误用。
- `: uint8_t` 显式底层类型，方便后续 `static_cast<uint8_t>(opcode)` 写入帧字节。
- 「控制帧」与「数据帧」用 `0x8` 这一位区分——`opcode >= 0x8` 必然是控制帧，这是 RFC 故意的位编排。

### 5.3 定义 WebSocketFrame 结构体

来自 [HISTORY/day31/src/include/http/WebSocket.h](../HISTORY/day31/src/include/http/WebSocket.h)（行 46–54）：

```cpp
struct WebSocketFrame {
    bool      fin{true};                   // FIN 位：是否最后一个分片
    WsOpcode  opcode{WsOpcode::kText};
    bool      masked{false};
    uint8_t   maskKey[4]{0, 0, 0, 0};
    std::string payload;                   // 已解掩码的负载

    bool isControl() const { return static_cast<uint8_t>(opcode) >= 0x8; }
};
```

`payload` 故意用 `std::string`（不是 `std::vector<char>`）：解码出来的内容如果是 Text 帧，业务往往直接当字符串用；如果是 Binary，`std::string` 也支持任意字节（`'\0'` 不打断）。

### 5.4 定义 WebSocketHandler 与连接上下文

来自 [HISTORY/day31/src/include/http/WebSocket.h](../HISTORY/day31/src/include/http/WebSocket.h)（行 70–93）：

```cpp
class WebSocketConnection;  // 前向声明

struct WebSocketHandler {
    std::function<void(WebSocketConnection &)> onOpen;
    std::function<void(WebSocketConnection &, const std::string &msg, bool isBinary)> onMessage;
    std::function<void(WebSocketConnection &, uint16_t code, const std::string &reason)> onClose;
    std::function<void(WebSocketConnection &, const std::string &payload)> onPing;
    std::function<void(WebSocketConnection &, const std::string &payload)> onPong;
};

struct WebSocketConnectionContext {
    enum class State { kOpen, kClosing, kClosed };

    WebSocketHandler handler;
    State state{State::kOpen};

    // 分片消息累积
    std::string fragmentBuffer;
    WsOpcode    fragmentOpcode{WsOpcode::kContinuation};
};
```

设计点：

- `WebSocketConnectionContext` 会被塞进 `Connection::context_`（Day 30 之前就是 `std::any` 容器，这里复用）。每条 TCP 连接存自己的"WebSocket 状态机状态"。
- `state` 是为了正确处理"双向关闭握手"：我方主动发 close 后进入 `kClosing`，等对方回 close 才进 `kClosed`。
- `fragmentBuffer/fragmentOpcode` 是为了支持分片：第一个非 FIN 数据帧把 opcode 记下，后续 continuation 帧把 payload 拼接，直到 FIN=1。

---

## 6. 第 5 步 — 写 encodeFrame 与 applyMask

### 6.1 问题背景

`encodeFrame` 是把"opcode + payload"打包成符合 RFC 帧格式的字节流。最大复杂度来自三种长度形式：

- payload ≤ 125 字节：直接放在 `Payload len` 7-bit 字段
- payload 126–65535 字节：`Payload len = 126`，再跟 2 字节大端长度
- payload ≥ 65536 字节：`Payload len = 127`，再跟 8 字节大端长度

服务端发出的帧**禁止**带掩码（MASK=0），所以最常用路径只走前 3 步。但我们仍要支持掩码——因为测试侧、未来要写客户端时都需要。

### 6.2 写 applyMask（掩码 = 解掩码）

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 197–200）：

```cpp
void applyMask(char *data, size_t len, const uint8_t maskKey[4]) {
    for (size_t i = 0; i < len; ++i)
        data[i] ^= static_cast<char>(maskKey[i % 4]);
}
```

**XOR 的对称性**：`(x ^ k) ^ k == x`，所以**同一个函数既可加掩码又可去掩码**。这是为什么 RFC 选 XOR 而不是 AES——零密钥协商、零额外状态。

`i % 4`：mask key 只有 4 字节，长 payload 循环复用。

### 6.3 写 encodeFrame

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 202–238）：

```cpp
std::string encodeFrame(WsOpcode opcode, const std::string &payload, bool mask,
                        const uint8_t maskKey[4]) {
    std::string frame;
    size_t payloadLen = payload.size();

    // 第 1 字节：FIN(1) | RSV(000) | opcode(4)
    frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

    // 第 2 字节：MASK(1) | payload length(7)
    uint8_t maskBit = mask ? 0x80 : 0x00;
    if (payloadLen <= 125) {
        frame.push_back(static_cast<char>(maskBit | static_cast<uint8_t>(payloadLen)));
    } else if (payloadLen <= 0xFFFF) {
        frame.push_back(static_cast<char>(maskBit | 126));
        frame.push_back(static_cast<char>((payloadLen >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payloadLen & 0xFF));
    } else {
        frame.push_back(static_cast<char>(maskBit | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((payloadLen >> (i * 8)) & 0xFF));
    }

    // 掩码密钥（4 字节）
    if (mask && maskKey) {
        frame.append(reinterpret_cast<const char *>(maskKey), 4);
    }

    // 负载
    if (mask && maskKey) {
        std::string masked = payload;
        applyMask(masked.data(), masked.size(), maskKey);
        frame.append(masked);
    } else {
        frame.append(payload);
    }

    return frame;
}
```

**值得注意的几个比特操作**：

- `0x80 | opcode` —— 高位 1 = FIN 帧（不分片）。如果将来要分片发送，这里要把 `0x80` 换成条件。
- `maskBit | 126` 和 `maskBit | 127` —— 把 MASK 位写进 `payload length` 字段的最高位。
- 8 字节长度大端写入：`(7 - i)` 循环把高字节先 push。

### 6.4 写一个 round-trip 测试

来自 [HISTORY/day31/src/test/WebSocketTest.cpp](../HISTORY/day31/src/test/WebSocketTest.cpp)（节选）：

```cpp
TEST(WebSocketTest, EncodeDecodeText_Unmasked) {
    std::string encoded = ws::encodeFrame(WsOpcode::kText, "hello", false, nullptr);
    // 帧应为：0x81 0x05 'h' 'e' 'l' 'l' 'o'
    ASSERT_EQ(encoded.size(), 7u);
    EXPECT_EQ(static_cast<uint8_t>(encoded[0]), 0x81);
    EXPECT_EQ(static_cast<uint8_t>(encoded[1]), 0x05);

    WebSocketFrame frame;
    size_t consumed = 0;
    auto result = ws::decodeFrame(encoded.data(), encoded.size(), frame, consumed);
    EXPECT_EQ(result, ws::DecodeResult::kComplete);
    EXPECT_EQ(consumed, 7u);
    EXPECT_TRUE(frame.fin);
    EXPECT_EQ(frame.opcode, WsOpcode::kText);
    EXPECT_EQ(frame.payload, "hello");
}
```

之所以一进 §6 就给一个测试，是为了让你现在就能 build + run，看到"绿条"再继续 §7 写 decodeFrame——这是测试驱动开发对一个长链路功能最舒服的节奏。

---

## 7. 第 6 步 — 写 decodeFrame

### 7.1 问题背景

`decodeFrame` 比 `encodeFrame` 难：**它必须处理"数据可能不够读完一帧"的情况**。TCP 是字节流，应用层 `Buffer` 可能只攒到了帧头的 1 字节、或攒到了"7 bit 长度说 payload 是 1000 字节，但实际只到了 500 字节"。这种情况不能报错，要返回 `kIncomplete`，让上层等下次 `read` 触发后再调用一次。

所以函数签名设计成：

```cpp
enum class DecodeResult { kComplete, kIncomplete, kError };

DecodeResult decodeFrame(const char *data, size_t len, WebSocketFrame &frame,
                         size_t &consumedBytes);
```

- `kComplete`：成功解出一帧，`consumedBytes` 告诉调用方"我吃了多少字节，请 retrieve 这么多"。
- `kIncomplete`：数据不够，`consumedBytes` 一定为 0。调用方什么都不要做，等更多数据。
- `kError`：协议错误（RSV 位错、长度溢出 63 位等），调用方应发 close 帧 + 关闭连接。

### 7.2 写实现：长度解析的三个 case

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 240–267）：

```cpp
DecodeResult decodeFrame(const char *data, size_t len, WebSocketFrame &frame,
                         size_t &consumedBytes) {
    consumedBytes = 0;
    if (len < 2)
        return DecodeResult::kIncomplete;

    const auto *bytes = reinterpret_cast<const uint8_t *>(data);

    // 第 1 字节
    frame.fin = (bytes[0] & 0x80) != 0;
    uint8_t rsv = (bytes[0] >> 4) & 0x07;
    if (rsv != 0)
        return DecodeResult::kError;          // 未协商扩展，RSV 必须为 0
    frame.opcode = static_cast<WsOpcode>(bytes[0] & 0x0F);

    // 第 2 字节：MASK + 7-bit 长度
    frame.masked = (bytes[1] & 0x80) != 0;
    uint64_t payloadLen = bytes[1] & 0x7F;

    size_t headerLen = 2;
    if (payloadLen == 126) {                  // 16-bit 扩展长度
        if (len < 4) return DecodeResult::kIncomplete;
        payloadLen = (static_cast<uint64_t>(bytes[2]) << 8) | bytes[3];
        headerLen = 4;
    } else if (payloadLen == 127) {           // 64-bit 扩展长度
        if (len < 10) return DecodeResult::kIncomplete;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i)
            payloadLen = (payloadLen << 8) | bytes[2 + i];
        headerLen = 10;
        // RFC 强制：64-bit 长度的最高位必须为 0
        if (payloadLen & (uint64_t(1) << 63))
            return DecodeResult::kError;
    }

    if (frame.masked) headerLen += 4;

    size_t totalLen = headerLen + static_cast<size_t>(payloadLen);
    if (len < totalLen) return DecodeResult::kIncomplete;

    // 读取掩码密钥
    if (frame.masked) {
        size_t maskOffset = headerLen - 4;
        std::memcpy(frame.maskKey, bytes + maskOffset, 4);
    }

    // 读取负载并解掩码
    frame.payload.assign(data + headerLen, static_cast<size_t>(payloadLen));
    if (frame.masked)
        applyMask(frame.payload.data(), frame.payload.size(), frame.maskKey);

    consumedBytes = totalLen;
    return DecodeResult::kComplete;
}
```

**几个隐藏陷阱**：

1. **每次访问字节前必须先检查 `len`**：先看 `len < 2`，再看 `len < 4`，再看 `len < 10`，最后看 `len < totalLen`。漏一个就是 OOB 读，AddressSanitizer 一上就抓。
2. **`payloadLen` 用 `uint64_t`**：因为 RFC 允许 63-bit 长度。但实际我们把它转 `size_t`——在 32-bit 平台会被截断；这不重要，因为没人会在 32-bit 平台跑 4GB 帧。
3. **`payloadLen & (uint64_t(1) << 63)` 检查**：RFC 6455 §5.2 强制最高位必须为 0（"The most significant bit MUST be 0"），违反就是协议错误。这条规则纯粹是为了未来扩展性——但我们必须实现。
4. **掩码 4 字节的位置**：`headerLen - 4` 而不是 `2 + extLenSize`，因为 `headerLen` 已经包含掩码空间。

### 7.3 验证：追踪一次 9 KB 文本帧解码

测试 `EncodeDecode_MediumPayload`（9000 字节文本，触发 16-bit 长度路径）：

```
encoded = encodeFrame(kText, payload9000, false, nullptr)
  │ frame[0] = 0x81           (FIN=1, opcode=Text)
  │ frame[1] = 126             (extended length flag)
  │ frame[2] = (9000 >> 8) & 0xFF = 0x23
  │ frame[3] =  9000 & 0xFF = 0x28
  │ frame[4..9003] = payload9000
  └ encoded.size() = 9004

decodeFrame(encoded.data(), 9004, frame, &consumed)
  │ len(9004) >= 2 ✓
  │ rsv = 0 ✓
  │ opcode = kText
  │ masked = false, payloadLen field = 126
  │ → 进 16-bit 路径，需要 len >= 4 ✓
  │   payloadLen = (0x23 << 8) | 0x28 = 9000
  │   headerLen = 4
  │ totalLen = 4 + 9000 = 9004 ✓ (== len)
  │ frame.payload.assign(encoded.data()+4, 9000)
  │ consumed = 9004
  └ return kComplete

调用方：buf->retrieve(9004)，状态机继续处理下一帧
```

### 7.4 拼包测试：MultipleFrames_InOneBuffer

`decodeFrame` 必须支持"一次 read 收到了多帧"的场景——非常常见，因为 TCP Nagle 会把短写合并：

```cpp
TEST(WebSocketTest, MultipleFrames_InOneBuffer) {
    std::string buffer;
    buffer += ws::encodeFrame(WsOpcode::kText, "frame1", false, nullptr);
    buffer += ws::encodeFrame(WsOpcode::kText, "frame2", false, nullptr);
    buffer += ws::encodeFrame(WsOpcode::kText, "frame3", false, nullptr);

    std::vector<std::string> got;
    size_t pos = 0;
    while (pos < buffer.size()) {
        WebSocketFrame f;
        size_t consumed = 0;
        auto r = ws::decodeFrame(buffer.data() + pos, buffer.size() - pos, f, consumed);
        ASSERT_EQ(r, ws::DecodeResult::kComplete);
        got.push_back(f.payload);
        pos += consumed;
    }
    EXPECT_EQ(got, (std::vector<std::string>{"frame1", "frame2", "frame3"}));
}
```

`pos += consumed` 这个 idiom 是字节流协议解码的标准写法——day28 的 `HttpContext::parse(consumedBytes)` 就是同一思路。

---

## 8. 第 7 步 — 写 handleWebSocketData 接收状态机

### 8.1 问题背景

到这里 6 个底层函数都齐了：SHA1、Base64、computeAcceptKey、isUpgradeRequest、encodeFrame、decodeFrame。我们要把它们组装成一个"完整的 WebSocket 接收循环"，在每次 `Connection::onMessage` 触发时调用，处理所有可能的帧类型。

涉及的所有"状态变化"：

| 触发 | 当前状态 | 行为 | 新状态 |
|-----|---------|------|------|
| 收到 Text/Binary 帧（FIN=1） | kOpen | 调 `onMessage(payload, isBinary)` | kOpen |
| 收到 Text/Binary 帧（FIN=0） | kOpen | 存进 `fragmentBuffer`、记 `fragmentOpcode` | kOpen |
| 收到 Continuation 帧（FIN=0） | kOpen + 有 fragment | 拼接到 `fragmentBuffer` | kOpen |
| 收到 Continuation 帧（FIN=1） | kOpen + 有 fragment | 拼接、调 `onMessage` 整体、清空 fragment | kOpen |
| 收到 Ping 帧 | kOpen | 调 `onPing` 或自动发 Pong | kOpen |
| 收到 Pong 帧 | kOpen | 调 `onPong` | kOpen |
| 收到 Close 帧 | kOpen | 回发 Close、调 `onClose`、`conn->close()` | kClosed |
| 收到 Close 帧 | kClosing（我方主动发起） | 调 `onClose`、`conn->close()` | kClosed |
| 收到分片 Continuation 但没有 fragment | kOpen | 协议错误，发 Close 1002 | kClosing |
| 收到新 Text/Binary 帧但 fragment 未结束 | kOpen | 协议错误，发 Close 1002 | kClosing |
| 收到分片的控制帧（ping/pong/close 且 FIN=0） | kOpen | 协议错误，发 Close 1002 | kClosing |
| `decodeFrame` 返回 kError | 任意 | 发 Close 1002，记日志 | kClosing |

12 个分支，全部需要测试覆盖。

### 8.2 写主循环骨架

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 300–325）：

```cpp
void handleWebSocketData(Connection *conn) {
    auto *ctx = conn->getContextAs<WebSocketConnectionContext>();
    if (!ctx || ctx->state == WebSocketConnectionContext::State::kClosed)
        return;

    Buffer *buf = conn->getInputBuffer();
    WebSocketConnection wsConn(conn);

    while (buf->readableBytes() > 0) {
        WebSocketFrame frame;
        size_t consumed = 0;
        auto result = decodeFrame(buf->peek(), buf->readableBytes(), frame, consumed);

        if (result == DecodeResult::kIncomplete) break;
        if (result == DecodeResult::kError) {
            LOG_WARN << "[WebSocket] 帧解析错误, fd=" << conn->getSocket()->getFd();
            wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError),
                             "Protocol error");
            ctx->state = WebSocketConnectionContext::State::kClosing;
            return;
        }
        buf->retrieve(consumed);

        // 控制帧不能分片 (RFC 6455 §5.4)
        if (frame.isControl() && !frame.fin) {
            wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError),
                             "Fragmented control frame");
            ctx->state = WebSocketConnectionContext::State::kClosing;
            return;
        }

        switch (frame.opcode) {
            // 8.3—8.6 各 case 逐一展开
        }
    }
}
```

这个循环的不变式：每次 while 进来，要么 buffer 空，要么至少能 decode 出一帧（或返回 incomplete）。`while (buf->readableBytes() > 0)` 而不是 `if`，是为了**一次 read 处理多帧**。

### 8.3 处理 Ping / Pong

```cpp
case WsOpcode::kPing:
    if (ctx->handler.onPing) {
        ctx->handler.onPing(wsConn, frame.payload);   // 用户自定义处理
    } else {
        wsConn.sendPong(frame.payload);               // 默认回应：原样回 Pong
    }
    break;

case WsOpcode::kPong:
    if (ctx->handler.onPong)
        ctx->handler.onPong(wsConn, frame.payload);
    break;
```

**默认行为差异**：

- Ping 必须回 Pong（RFC 强制）。我们提供"无回调时自动 Pong"的默认值，让最简业务（聊天室）不写一行 Ping 处理就能保活。
- Pong 没有强制回应，所以无回调时直接忽略。

### 8.4 处理 Close（双向握手）

```cpp
case WsOpcode::kClose: {
    uint16_t code = static_cast<uint16_t>(WsCloseCode::kNoStatus);
    std::string reason;
    if (frame.payload.size() >= 2) {
        code = (static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[0])) << 8) |
                static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[1]));
        reason = frame.payload.substr(2);
    }

    if (ctx->state == WebSocketConnectionContext::State::kClosing) {
        // 我方先发 close，对方回 close → 完成握手
        ctx->state = WebSocketConnectionContext::State::kClosed;
        if (ctx->handler.onClose) ctx->handler.onClose(wsConn, code, reason);
        conn->close();
    } else {
        // 对方主动发 close，我们必须回一个
        ctx->state = WebSocketConnectionContext::State::kClosing;
        wsConn.sendClose(code, reason);
        ctx->state = WebSocketConnectionContext::State::kClosed;
        if (ctx->handler.onClose) ctx->handler.onClose(wsConn, code, reason);
        conn->close();
    }
    return;
}
```

**关闭码格式**：Close 帧的 payload 前 2 字节是大端关闭码（如 1000=正常、1002=协议错误、1009=消息太大），后面是可选 UTF-8 reason。

**两支分支不要写错**：

- 如果是被动收到 close → 我们发回一个 close，然后 close TCP。
- 如果是主动发起的 close（之前已经发过、状态是 kClosing）→ 现在等到了对方的回应，可以直接 close TCP。

### 8.5 处理数据帧 + 分片

```cpp
case WsOpcode::kText:
case WsOpcode::kBinary: {
    if (!ctx->fragmentBuffer.empty()) {
        // 上一组分片还没结束，又来新数据帧 → 协议错
        wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError),
                         "New message before fragment complete");
        ctx->state = WebSocketConnectionContext::State::kClosing;
        return;
    }
    if (frame.fin) {
        // 整条消息一帧搞定
        bool isBinary = (frame.opcode == WsOpcode::kBinary);
        if (ctx->handler.onMessage)
            ctx->handler.onMessage(wsConn, frame.payload, isBinary);
    } else {
        // 分片开头：记下 opcode、把第一段 payload 存起来
        ctx->fragmentOpcode = frame.opcode;
        ctx->fragmentBuffer = std::move(frame.payload);
    }
    break;
}

case WsOpcode::kContinuation: {
    if (ctx->fragmentBuffer.empty() &&
        ctx->fragmentOpcode == WsOpcode::kContinuation) {
        // 没有上下文却来 continuation → 协议错
        wsConn.sendClose(static_cast<uint16_t>(WsCloseCode::kProtocolError),
                         "Unexpected continuation frame");
        ctx->state = WebSocketConnectionContext::State::kClosing;
        return;
    }
    ctx->fragmentBuffer.append(frame.payload);
    if (frame.fin) {
        bool isBinary = (ctx->fragmentOpcode == WsOpcode::kBinary);
        if (ctx->handler.onMessage)
            ctx->handler.onMessage(wsConn, ctx->fragmentBuffer, isBinary);
        ctx->fragmentBuffer.clear();
        ctx->fragmentOpcode = WsOpcode::kContinuation;
    }
    break;
}
```

**分片流程实例**：客户端要发 30 KB 文本，但每帧最多 10 KB：

```
客户端：发 [Text  | FIN=0 | "前10KB"]
服务端：fragmentOpcode = Text, fragmentBuffer = "前10KB"

客户端：发 [Cont  | FIN=0 | "中10KB"]
服务端：fragmentBuffer.append("中10KB")  → 现在是 20 KB

客户端：发 [Cont  | FIN=1 | "末10KB"]
服务端：fragmentBuffer.append("末10KB")  → 现在是 30 KB
        触发 onMessage(wsConn, 30KB字符串, isBinary=false)
        fragmentBuffer.clear(), fragmentOpcode 复位
```

### 8.6 验证：追踪一次完整的关闭握手

服务端调 `wsConn.sendClose(1000, "bye")` 主动关闭：

```
1. sendClose(1000, "bye")
   └ payload = [0x03, 0xE8, 'b', 'y', 'e']     (1000=0x03E8 大端)
   └ encodeFrame(kClose, payload, false, nullptr)
   └ frame = 0x88 0x05 0x03 0xE8 'b' 'y' 'e'   (0x88 = FIN+Close)
   └ conn->send(frame)                         // 发出去
   └ ctx->state 还在 kOpen   ※注意：sendClose 自身不改 state

2. 业务代码紧接着应当：
   ctx->state = kClosing;                      // 显式置位

3. 等对方回 close 帧到达 → handleWebSocketData 进入 case kClose
   └ ctx->state == kClosing 分支
   └ ctx->state = kClosed
   └ onClose(wsConn, 1000, "...")              // 通知业务
   └ conn->close()                             // 关 TCP

4. 如果对方在合理时间内不回（比如 30 秒）—— 由 day29 的请求超时机制兜底关 TCP
```

**实际上**当前实现的 `WebSocketConnection::sendClose` 没把状态改成 `kClosing`，业务代码需要手动设置。Day 32 之后我们引入协程时会把这个 state machine 收进 connection 自身，让业务彻底不用管 state。

---

## 9. 第 8 步 — 写 WebSocketConnection 对外接口

### 9.1 问题背景

到这里所有逻辑都跑得通，但业务代码要发一条文本消息得这样写：

```cpp
std::string frame = ws::encodeFrame(WsOpcode::kText, "hello", false, nullptr);
conn->send(std::move(frame));
```

每次都要记得 `false, nullptr`、记得 `WsOpcode::kText`——既啰嗦又容易写错。我们封一层 `WebSocketConnection` 外观类，让业务只写：

```cpp
wsConn.sendText("hello");
```

### 9.2 写 WebSocketConnection

来自 [HISTORY/day31/src/common/http/WebSocket.cpp](../HISTORY/day31/src/common/http/WebSocket.cpp)（行 132–166）：

```cpp
WebSocketConnection::WebSocketConnection(Connection *conn) : conn_(conn) {}

void WebSocketConnection::sendText(const std::string &msg) {
    sendFrame(WsOpcode::kText, msg);
}

void WebSocketConnection::sendBinary(const std::string &data) {
    sendFrame(WsOpcode::kBinary, data);
}

void WebSocketConnection::sendPing(const std::string &payload) {
    sendFrame(WsOpcode::kPing, payload);
}

void WebSocketConnection::sendPong(const std::string &payload) {
    sendFrame(WsOpcode::kPong, payload);
}

void WebSocketConnection::sendClose(uint16_t code, const std::string &reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason);
    sendFrame(WsOpcode::kClose, payload);
}

void WebSocketConnection::sendFrame(WsOpcode opcode, const std::string &payload) {
    // 服务端发出的帧不需要掩码
    std::string frame = ws::encodeFrame(opcode, payload, false, nullptr);
    conn_->send(std::move(frame));
}
```

**设计取舍**：

- `WebSocketConnection` 只持有 `Connection*` 裸指针——它生命周期短（在每次 `handleWebSocketData` 内构造、回调结束就销毁），不需要 share_ptr。
- 没把 `WebSocketConnection` 做成 `Connection` 的子类，是因为我们要支持"同一个 TCP 连接前半生跑 HTTP、后半生升级成 WebSocket"——子类做不到运行时切换协议，但 `Connection::context_` 里换一个 `std::any` 就行。
- `sendClose` 自己组装 2 字节 code + reason；业务方写 `sendClose(1000, "bye")` 就完事。

### 9.3 上层接入：HttpServer 升级握手（伪代码）

来自上层业务集成的典型用法（不在 day31 仓库内，但建议读者照写）：

```cpp
httpServer.setHttpCallback([&](const HttpRequest &req, HttpResponse *resp,
                               Connection *conn) {
    if (ws::isUpgradeRequest(req.getHeader("Upgrade"),
                             req.getHeader("Connection"))) {
        std::string clientKey = req.getHeader("Sec-WebSocket-Key");
        std::string accept    = ws::computeAcceptKey(clientKey);
        std::string handshake = ws::buildHandshakeResponse(accept);
        conn->send(handshake);

        WebSocketHandler handler;
        handler.onMessage = [](WebSocketConnection &c, const std::string &msg, bool) {
            c.sendText("echo: " + msg);
        };
        handler.onClose = [](WebSocketConnection &, uint16_t code, const std::string &) {
            LOG_INFO << "[ws] closed code=" << code;
        };
        conn->setContext(WebSocketConnectionContext{std::move(handler)});

        // 把 Connection 的 onMessage 改接到 ws::handleWebSocketData
        conn->setMessageCallback([](Connection *c){ ws::handleWebSocketData(c); });

        if (auto *ctx = conn->getContextAs<WebSocketConnectionContext>(); ctx->handler.onOpen) {
            WebSocketConnection wsc(conn);
            ctx->handler.onOpen(wsc);
        }
        return;
    }
    // 否则按普通 HTTP 处理
    ...
});
```

要点：

- `conn->setContext` 把 ctx 塞进 Connection 自带的 `std::any`。
- `conn->setMessageCallback` 把后续每次 read 的派发目标从 HTTP 解析切换到 ws 解析——这就是"同一连接两段生命"的实现机制。

---

## 10. 测试覆盖与验证

### 10.1 21 个 GTest 用例分布

来自 [HISTORY/day31/src/test/WebSocketTest.cpp](../HISTORY/day31/src/test/WebSocketTest.cpp)：

| # | 用例 | 覆盖什么 |
|---|------|---------|
| 1 | `HandshakeAcceptKey_RFC6455` | RFC 标准向量 → 整条 sha1+base64 链路 |
| 2 | `SHA1_Empty` | 空输入 → `da39a3ee...8709`（RFC 标准向量） |
| 3 | `SHA1_ABC` | `"abc"` → `a9993e36...d89d` |
| 4 | `SHA1_LongInput` | 1000 字节，触发多次 processBlock |
| 5 | `Base64_Basic` | "Man"/"Ma"/"M" 三种 padding |
| 6 | `Base64_Empty` | 空字符串 → 空字符串 |
| 7 | `MaskUnmask_Roundtrip` | XOR 对称性 |
| 8 | `EncodeDecodeText_Unmasked` | 7-bit 长度路径 |
| 9 | `EncodeDecodeBinary_Masked` | 7-bit + 掩码路径 |
| 10 | `EncodeDecode_MediumPayload` | 9000 字节，16-bit 长度路径 |
| 11 | `EncodeDecode_LargePayload` | 80000 字节，64-bit 长度路径 |
| 12 | `Decode_Incomplete_OneByte` | len=1 应返回 kIncomplete |
| 13 | `Decode_Incomplete_TruncatedExtLen` | 长度字段不完整 |
| 14 | `Decode_Incomplete_TruncatedPayload` | header 完整但 payload 缺 |
| 15 | `Decode_RSVBitsError` | RSV != 0 → kError |
| 16 | `Decode_LengthOverflowError` | 64-bit 长度最高位 1 → kError |
| 17 | `CloseFrame_Encode` | 关闭码 1000 + reason 编码 |
| 18 | `PingPong_Roundtrip` | Ping 帧编解码 |
| 19 | `Fragmentation_ManualReassembly` | 手动模拟 3 段分片 |
| 20 | `MultipleFrames_InOneBuffer` | 拼包 → 多次 decode |
| 21 | `Decode_ZeroLengthPayload` | payloadLen=0 边界 |

### 10.2 跑测试

```bash
$ cd build && ctest --output-on-failure -R WebSocket
Test project /Users/.../MyCppServerLib/build
    Start  53: WebSocketTest.HandshakeAcceptKey_RFC6455
1/21 Test #53: WebSocketTest.HandshakeAcceptKey_RFC6455 .......   Passed   0.01 sec
...
21/21 Test #73: WebSocketTest.Decode_ZeroLengthPayload ........   Passed   0.00 sec

100% tests passed, 0 tests failed out of 21
Total Test time (real) = 0.13 sec
```

### 10.3 端到端：用 wscat 验证

```bash
# 终端 A：跑服务（假设 echo 业务已接好）
$ ./demo_websocket_echo
[INFO] WebSocket echo server on :8080

# 终端 B：用 wscat（npm install -g wscat）
$ wscat -c ws://localhost:8080/echo
Connected (press CTRL+C to quit)
> hello
< echo: hello
> 你好世界
< echo: 你好世界
> [按 Ctrl+C]
Disconnected (code: 1006, reason: "")
```

### 10.4 验证：浏览器 DevTools 抓包

打开浏览器 DevTools → Network → WS 标签，点开连接，可以看到逐帧的 hex dump，对照 §6 §7 的格局逐字节核对。这是验证 WebSocket 实现最直观的手段。

---

## 11. 局限与下一步

### 11.1 当前局限

1. **未自动接入 HttpServer**：升级握手要业务自己写 `if (isUpgradeRequest)` 分支。生产框架（Boost.Beast / uWebSockets）会把这一步内置。
2. **不支持 wss://**：TLS 层留给后续 OpenSSL 集成时一起处理。
3. **没有 permessage-deflate 扩展**：高频文本通信压缩可省 70% 带宽，但需要 zlib 流式压缩 + Sec-WebSocket-Extensions 协商。
4. **不限制单帧最大尺寸**：理论上对方发 1 GB 帧会让 `frame.payload` 占 1 GB 内存。生产环境要在 §7 加 `if (payloadLen > MAX_FRAME_SIZE) return kError`。
5. **不强校验 UTF-8**：Text 帧 RFC 要求 payload 必须是合法 UTF-8，我们没校验。聊天室业务可能因此被恶意客户端注入坏字节。

### 11.2 下一步演进路线

- **Day 32**：C++20 协程 IO，把 `onMessage` 这种回调式接口改成 `co_await wsConn.recv()`。
- **Day 33**：io_uring 后端，让所有 send/recv 走完成队列。
- **Day 34**：无锁 MPMC 队列，解决多线程下广播聊天室消息时的锁竞争。
- **Day 35**：内存池，把 `frame.payload` 从 `std::string` 换成池化分配，热路径上去掉 new/delete。
- **Day 36**：与 muduo 横向基准，确认我们的 WebSocket QPS 不输生产级框架。
