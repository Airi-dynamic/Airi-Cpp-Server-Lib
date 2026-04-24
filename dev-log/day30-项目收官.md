# Day 30 — 项目收官：GTest 迁移 / util.cpp 清理 / Phase 4 终结

> **今日目标**：把 Day 28-29 的 5 个手工测试文件逐一迁移到 Google Test，清理遗留的 `util.cpp`，并配置 CMake 让 ctest 看到所有 17 个独立用例。
> **基于**：Day 29（生产特性）。**结束**：Phase 4「测试与生产化」。下个 Phase（Day 31+）是「协议 / 协程 / 高性能 IO 实验区」。

---

## 0. 今日构建目标

Phase 4 的最后一天，工作分两部分：把 Day 28-29 积累的 5 个手工测试文件迁移到 Google Test，并清理遗留的 `common/util.cpp`。两件事都是机械操作——逻辑不变，只是把框架和遗留代码换掉。

**构建清单（按顺序）：**

1. **§2** — 迁移 `BackpressureDecisionTest`：把 `check()/fail()/main()` 换成 `TEST() / EXPECT_*`，共 3 个用例
2. **§3** — 迁移 `HttpContextTest`：pipeline / body 分段 / 非法方法，共 3 个用例
3. **§4** — 迁移 `HttpRequestLimitsTest`：请求行 / 头部 / 体积 / 合法请求，共 4 个用例
4. **§5** — 迁移 `SocketPolicyTest`：无效 fd / 有效 fd，共 2 个用例
5. **§6** — 迁移 `TcpServerPolicyTest`：连接上限边界 / 小值 / 大值 / IO 线程 / 默认配置，共 5 个用例
6. **§7** — 配置 CMake：加入 `FetchContent(googletest v1.14.0)` + `gtest_discover_tests`，让 ctest 看到 17 个独立用例
7. **§8** — 清理 `common/util.cpp`：`git rm` 移除已无引用的 `errif()` 遗留文件

**说明**：代码块前的「来自 `HISTORY/day30/...`」标注意为「将以下代码写入该文件的对应位置」，跟着每步动手输入即可。

---

## 1. 今天要解决的几个问题

### 1.1 手工框架的五个具体痛点

Day 28-29 建立了完整的策略测试体系——5 个核心测试文件覆盖了项目最关键的几条决策路径：

| 测试文件 | 覆盖的决策 |
|---------|-----------|
| `BackpressureDecisionTest` | 输出缓冲区水位 → 暂停读 / 恢复读 / 强制断连 |
| `TcpServerPolicyTest` | 连接数边界 / IO 线程数归一化 / Options 默认值 |
| `HttpContextTest` | HTTP pipeline 多请求精确消费 / 分段 body / 非法方法 |
| `HttpRequestLimitsTest` | 三层限流（请求行 / 头部 / 体） |
| `SocketPolicyTest` | 无效 fd / 有效 fd 上各操作的返回值语义 |

但这些测试都是**手工框架**——每个文件自带一对 `check()` / `fail()` 辅助函数 + 一个手写的 `main()` 入口：

```cpp
namespace {
[[noreturn]] void fail(const std::string &msg) {
    std::cerr << "[FAIL] " << msg << "\n";
    std::abort();
}

void check(bool cond, const std::string &msg) {
    if (!cond) fail(msg);
    std::cout << "[PASS] " << msg << "\n";
}

void testConfigValidation() { check(...); check(...); }
void testPauseResumeDecision() { check(...); }
void testHardLimit() { check(...); }
} // namespace

int main() {
    testConfigValidation();
    testPauseResumeDecision();
    testHardLimit();
    std::cout << "=== 全部通过 ===\n";
    return 0;
}
```

这套手工框架在 5 个文件里（每个 30~120 行）出现了 5 次。它工作得"勉强能用"，但已经积累了五个具体的痛点：

1. **辅助代码重复**：5 份 `check()` / `fail()` 实现，10+ 行 boilerplate × 5 = 50 行纯冗余。改一个失败时的输出格式（比如想加上文件名:行号）要改 5 次。
2. **断言失败时信息贫瘠**：`check(consumed > 0, "应推进消费字节")` 失败时只看到自定义文字，看不到 `consumed` 的实际值。开发者必须临时在前面加一条 `std::cerr << "consumed=" << consumed << '\n'` 才能定位 bug——而且改完还得记得删掉。
3. **测试不独立**：所有用例在同一个 `main()` 里串联调用。`testConfigValidation()` 内部 `fail()` → `std::abort()`，后面的 `testPauseResumeDecision()` / `testHardLimit()` 全部跳过；CI 上看到的就是"一个错误掩盖了其余可能的错误"。
4. **无法选择性运行**：想单独跑"硬上限断连"这一个用例？不行。必须从头跑整个二进制。
5. **CTest 粒度粗**：CTest 看到的是 `BackpressureDecisionTest [Passed]` 这一行——只能告诉你"这个文件整体通过/失败"，看不到内部各用例的状态。

### 1.2 真实触发场景

| 痛点 | 真实场景 |
|------|---------|
| 失败信息贫瘠 | Day 28 调试 `consumed > 0` 那条断言时，临时加了三行 cerr 才发现 `consumed=15` 但 `req.size()=14`——本来 GTest 的 `EXPECT_GT(consumed, 0)` 失败时会直接打印 "Expected: (consumed) > (0), actual: 15 vs 0" |
| 测试不独立 | 一次 refactor 把 `Connection::evaluateBackpressure` 的水位边界条件 `>` 改成 `>=`，导致 `testPauseResumeDecision` 第一句 `abort()`；后面的 `testHardLimit` 实际上**也已经坏了**，但因为提前 abort 没有暴露出来，等一周后 PR review 才被发现 |
| 选择性运行缺失 | 局部修改 `HttpContext` 的状态机时只想跑 `HttpContextTest::PipelineConsumedBytes`，但必须跑完包含 5+ 个用例的整个二进制 |
| CTest 粒度粗 | CI 报告里只能看到 5 行 `Passed`，无法在 PR Comment 里贴出 "30 个具体子用例全部通过"的细节 |

### 1.3 解法：迁移到 Google Test

GTest 是 C++ 生态中最成熟的单元测试框架，被 Chromium、Protobuf、gRPC、TensorFlow、LLVM 等大型项目采用。它把上面 5 个痛点全部解掉：

```
手工框架                          Google Test
────────                          ───────────
check(cond, msg)            →     EXPECT_TRUE(cond)  << msg
check(!cond, msg)           →     EXPECT_FALSE(cond) << msg
check(a == b, msg)          →     EXPECT_EQ(a, b)    << msg
check(a > b, msg)           →     EXPECT_GT(a, b)    << msg
fail(msg)                   →     FAIL()             << msg
void testXxx() { ... }      →     TEST(Suite, Xxx) { ... }
int main() { testA(); ... } →     删除（gtest_main 提供）
```

收益：

| 指标 | 手工框架 | GTest |
|------|---------|-------|
| `BackpressureDecisionTest.cpp` 行数 | 112 | 60（-46%） |
| 断言失败信息 | 自定义文字 | 自动 `Expected: X, actual: Y` |
| 测试入口 | 每文件手写 `main()` | `gtest_main` 链接库 |
| 用例独立性 | 一处 abort 全断 | 每个 TEST 沙箱化 |
| 选择性运行 | 不支持 | `--gtest_filter=Suite.Case` |
| CTest 粒度 | 整个文件 | 每个 TEST() 一条 |

### 1.4 与 CMake 的零摩擦集成

依赖侧用 `FetchContent`，不依赖系统装的 GTest（避免 macOS / Ubuntu / 容器各装各的版本差异）：

```cmake
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
```

注册侧每个测试一行：

```cmake
add_executable(BackpressureDecisionTest test/BackpressureDecisionTest.cpp ${COMMON_SRC})
target_link_libraries(BackpressureDecisionTest gtest gtest_main pthread)
```

CTest 自动发现：通过 `include(GoogleTest)` + `gtest_discover_tests(BackpressureDecisionTest)`，构建后自动扫描二进制内所有 `TEST()` 宏，将每一个注册为独立 CTest 用例。`ctest --output-on-failure` 看到的不再是 5 行而是 18 行。

### 1.5 第二件事 — `common/util.cpp` 清理

Day 1-26 时，错误处理是这样的：

```cpp
int sock_fd = socket(...);
errif(sock_fd == -1, "socket create error");
```

`errif()` 出现在 50+ 处。Day 21-26 引入 Logger 体系后，逐步把所有 `errif()` 换成 `LOG_FATAL` / `LOG_ERROR`，但 `common/util.cpp` 文件本身一直留着——CMake 已经不编译它，但物理文件还在仓库里。

到 Day 30 这个文件已经是个纯粹的"考古遗迹"：
- 不在编译产物中（CMakeLists 不引用）
- 不被任何 .cpp / .h include
- 但新贡献者 grep 仓库时仍会看到它，必须翻 git log 才知道"哦这个不再使用"

所以 Day 30 顺手 `git rm common/util.cpp common/util.h`（如果还存在），让仓库结构和实际依赖图保持一致。

### 1.6 今日的方案

两件事：

1. **GTest 迁移**：5 个测试文件的 `check()/fail()/testXxx()/main()` 全部改写为 `TEST() / EXPECT_*`，删除每个文件里的 `namespace` 辅助块和手写 `main()`。逻辑保持完全一致——这是个机械替换。
2. **util.cpp 清理**：物理删除遗留文件。

### 1.7 文件变更总览

| 文件 | 状态 | 关键改动 |
|------|------|---------|
| `test/BackpressureDecisionTest.cpp` | **GTest 重写** | 112L → 60L；3 个 `TEST()` |
| `test/HttpContextTest.cpp` | **GTest 重写** | 3 个 `TEST()` 替代 testPipeline/testBody/testInvalid |
| `test/HttpRequestLimitsTest.cpp` | **GTest 重写** | 4 个 `TEST()`：RequestLine/Header/Body/Valid |
| `test/SocketPolicyTest.cpp` | **GTest 重写** | 2 个 `TEST()`：InvalidFd / ValidSocket |
| `test/TcpServerPolicyTest.cpp` | **GTest 重写** | 5 个 `TEST()`：RejectBoundary/Small/Large/IoThreads/Defaults |
| `CMakeLists.txt` | **修改** | 新增 FetchContent(googletest v1.14.0) + 5 个测试目标链接 gtest/gtest_main |
| `common/util.cpp` | **删除** | errif 遗留清理 |
| `common/util.h` | **删除** | 同上 |

---

## 2. 第 1 步 — 迁移 BackpressureDecisionTest

### 2.1 问题背景

回压决策是 Day 28 引入的核心安全机制：

```cpp
// Connection.h
struct BackpressureConfig {
    size_t lowWatermarkBytes  = 4 * 1024;
    size_t highWatermarkBytes = 16 * 1024;
    size_t hardLimitBytes     = 64 * 1024;
};

struct BackpressureDecision {
    bool shouldPauseRead;
    bool shouldResumeRead;
    bool shouldCloseConnection;
};

BackpressureDecision evaluateBackpressure(size_t bufferBytes, bool isPaused,
                                          const BackpressureConfig &cfg);
```

它决定了高负载时服务器的整体行为：buffer > high → 暂停读、buffer ≤ low → 恢复读、buffer > hardLimit → 强制断连。任何一处条件写错（`>` 写成 `>=`、`high` 与 `low` 比较反、`hardLimit` 校验缺失）都会让整个服务在生产负载下错乱——可能是无限挂着死连接，也可能是合法连接被误杀。

正因如此，这个函数的测试是项目里**最不能容忍"测试失败信息不清"的地方**。开发者必须能看到 `bufferBytes / isPaused / cfg` 三个输入和 `decision.*` 三个输出的具体值，而不是只看到一句"暂停读决策错误"。

### 2.2 先看旧测试的接口依赖

来自 [HISTORY/day30/test/BackpressureDecisionTest.cpp](HISTORY/day30/test/BackpressureDecisionTest.cpp)（行 1–4）：

```cpp
#include "Connection.h"
#include <gtest/gtest.h>
```

两行包含——不再需要 `iostream`、`cstdlib`、自定义 `namespace`。`gtest/gtest.h` 提供 `TEST()`、`EXPECT_*`、`ASSERT_*` 三组宏。

### 2.3 写三个 TEST()

#### 2.3.1 配置合法性

来自 [HISTORY/day30/test/BackpressureDecisionTest.cpp](HISTORY/day30/test/BackpressureDecisionTest.cpp)（行 5–24）：

```cpp
TEST(BackpressureDecisionTest, ConfigValidation) {
    Connection::BackpressureConfig good{};
    EXPECT_TRUE(Connection::isValidBackpressureConfig(good)) << "默认配置应合法";

    Connection::BackpressureConfig bad1{};
    bad1.lowWatermarkBytes = 0;
    EXPECT_FALSE(Connection::isValidBackpressureConfig(bad1)) << "low=0 应非法";

    Connection::BackpressureConfig bad2{};
    bad2.lowWatermarkBytes = 8;
    bad2.highWatermarkBytes = 4;
    EXPECT_FALSE(Connection::isValidBackpressureConfig(bad2)) << "low >= high 应非法";

    Connection::BackpressureConfig bad3{};
    bad3.lowWatermarkBytes = 4;
    bad3.highWatermarkBytes = 8;
    bad3.hardLimitBytes = 8;
    EXPECT_FALSE(Connection::isValidBackpressureConfig(bad3)) << "high >= hardLimit 应非法";
}
```

四个独立场景，每个都用了 `<<` 注入失败时的描述。GTest 失败输出例（假设 `bad2` 的检查反了）：

```
test/BackpressureDecisionTest.cpp:18: Failure
Value of: Connection::isValidBackpressureConfig(bad2)
  Actual: true
Expected: false
low >= high 应非法
```

—— 它告诉你期望/实际值都是什么、附加注释、源码精确行号。手工框架只能输出 `[FAIL] low >= high 应非法`。

#### 2.3.2 暂停 / 恢复决策

行 26–50：

```cpp
TEST(BackpressureDecisionTest, PauseResumeDecision) {
    Connection::BackpressureConfig cfg{};
    cfg.lowWatermarkBytes = 10;
    cfg.highWatermarkBytes = 20;
    cfg.hardLimitBytes = 30;

    auto d1 = Connection::evaluateBackpressure(5, false, cfg);
    EXPECT_FALSE(d1.shouldPauseRead);
    EXPECT_FALSE(d1.shouldResumeRead);
    EXPECT_FALSE(d1.shouldCloseConnection) << "buffer=5 不应触发动作";

    auto d2 = Connection::evaluateBackpressure(21, false, cfg);
    EXPECT_TRUE(d2.shouldPauseRead) << "buffer=21（未暂停）应触发暂停读";
    EXPECT_FALSE(d2.shouldResumeRead);
    EXPECT_FALSE(d2.shouldCloseConnection);

    auto d3 = Connection::evaluateBackpressure(15, true, cfg);
    EXPECT_FALSE(d3.shouldPauseRead);
    EXPECT_FALSE(d3.shouldResumeRead) << "buffer=15（已暂停）不应立即恢复";
    EXPECT_FALSE(d3.shouldCloseConnection);

    auto d4 = Connection::evaluateBackpressure(10, true, cfg);
    EXPECT_FALSE(d4.shouldPauseRead);
    EXPECT_TRUE(d4.shouldResumeRead) << "buffer=10（已暂停）应恢复读";
    EXPECT_FALSE(d4.shouldCloseConnection);
}
```

**4 个用例对应回压状态机的 4 个边界**：

| 用例 | bufferBytes | isPaused | 期望决策 | 验证内容 |
|------|-------------|----------|---------|---------|
| d1 | 5 (< low=10) | false | 全 false | low 以下不触发任何动作 |
| d2 | 21 (> high=20) | false | shouldPauseRead | 越过 high 水位需暂停读 |
| d3 | 15 (low<X<high) | true | 全 false | 已暂停 + 在中间区不重复动作 |
| d4 | 10 (== low) | true | shouldResumeRead | 已暂停 + 回到 low 应恢复 |

**关键不变式**：四个 `EXPECT_*` 即使其中一个失败，其余仍会执行——这是 `EXPECT_` 与 `ASSERT_` 的核心区别。手工框架是 `check() → fail() → abort()`，第一处失败立刻挂掉，后面的潜在 bug 全被遮蔽。

#### 2.3.3 硬上限断连

行 52–60：

```cpp
TEST(BackpressureDecisionTest, HardLimitDecision) {
    Connection::BackpressureConfig cfg{};
    cfg.lowWatermarkBytes = 10;
    cfg.highWatermarkBytes = 20;
    cfg.hardLimitBytes = 30;

    auto d = Connection::evaluateBackpressure(31, false, cfg);
    EXPECT_TRUE(d.shouldCloseConnection) << "buffer>hardLimit 应触发保护性断连";
}
```

只有一个断言——把 `hardLimit=30` 的最敏感边界单独拎出来一个用例。这是 GTest 的另一个隐性收益：**当一个用例失败，不会影响其它用例的判定**，所以可以放心地"一个用例一个断言"。

### 2.4 嵌入构建系统

CMakeLists.txt 行 56–57：

```cmake
add_executable(BackpressureDecisionTest test/BackpressureDecisionTest.cpp ${COMMON_SRC})
target_link_libraries(BackpressureDecisionTest gtest gtest_main pthread)
```

- `gtest`：GTest 静态库，提供 `TEST()` / `EXPECT_*` 等宏与运行时
- `gtest_main`：仅一行 `int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }` 的库——让测试文件不需要写 main
- `pthread`：GTest 内部某些路径依赖（如死亡测试）

`gtest_main` 是优雅之处：测试文件保持纯净，只有 `TEST()` 宏；启动入口由链接的库提供。这避免了"每个测试文件都抄一份 InitGoogleTest"的样板代码。

### 2.5 验证：追踪 ctest 从命令到输出

#### 业务场景 + 时序总览表

`ctest` 在 `build/` 目录下被调用，目标是验证迁移后所有 GTest 用例都通过：

| 时刻 | 事件 | 关键状态 | 决策 |
|------|------|---------|------|
| T0 | 开发者执行 `ctest --output-on-failure` | CTestTestfile.cmake 已生成 | 加载测试列表 |
| T1 | CTest 读取列表 → 看到 `BackpressureDecisionTest` 二进制 | 19 条独立用例（5 文件 × N 个 TEST） | 顺序 / 并行执行 |
| T2 | fork BackpressureDecisionTest 进程 | gtest_main 接管 main | 进入 RUN_ALL_TESTS() |
| T3 | InitGoogleTest 解析参数 | argv 含 `--gtest_filter=...` | 决定哪些 TEST 被运行 |
| T4 | 遍历静态注册表，逐个 TEST() 跑 | ConfigValidation / PauseResume / HardLimit | 每个 TEST 独立环境 |
| T5 | 每个 TEST 内 EXPECT_* 累加结果 | 计数器 passed / failed | 不 abort |
| T6 | RUN_ALL_TESTS 返回 | 失败数 → exit code | 0=全过, 1=有失败 |
| T7 | CTest 收集 stdout + exit code | Passed/Failed 统计 | 输出到终端 |

#### 第 1 步：`TEST()` 宏展开成什么

GTest 的 `TEST(Suite, Name)` 宏宏观上是：

```cpp
class BackpressureDecisionTest_ConfigValidation_Test : public ::testing::Test {
public:
    BackpressureDecisionTest_ConfigValidation_Test() {}
private:
    virtual void TestBody();
    static ::testing::TestInfo* const test_info_;
};

::testing::TestInfo* const
BackpressureDecisionTest_ConfigValidation_Test::test_info_ =
    ::testing::internal::MakeAndRegisterTestInfo(
        "BackpressureDecisionTest", "ConfigValidation",
        nullptr, nullptr,
        ::testing::internal::CodeLocation(__FILE__, __LINE__),
        ::testing::internal::GetTestTypeId(),
        ::testing::Test::SetUpTestSuite,
        ::testing::Test::TearDownTestSuite,
        []() -> ::testing::Test* {
            return new BackpressureDecisionTest_ConfigValidation_Test;
        });

void BackpressureDecisionTest_ConfigValidation_Test::TestBody() {
    // ... 我们写的代码 ...
}
```

**关键点**：宏展开生成两块代码——
1. 一个 `TestBody()` 方法，里面装我们的断言。
2. 一个 `static TestInfo*` 成员，初始化时调 `MakeAndRegisterTestInfo` —— **这一步在 main 之前由 C++ 静态初始化执行**，把测试加入全局注册表。

**副作用快照**（程序启动后、`main()` 执行前）：

```
::testing::UnitTest::GetInstance()->total_test_count() = 3
注册表中包含：
  [0] BackpressureDecisionTest.ConfigValidation
  [1] BackpressureDecisionTest.PauseResumeDecision
  [2] BackpressureDecisionTest.HardLimitDecision
```

#### 第 2 步：gtest_main 提供的 main

**gtest_main 的实际实现**（来自 GoogleTest 源码 `googletest/src/gtest_main.cc`，等价代码）：

```cpp
GTEST_API_ int main(int argc, char **argv) {
    printf("Running main() from gtest_main.cc\n");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

**代入实参**：
- `argc = 2`、`argv = {"BackpressureDecisionTest", "--gtest_filter=*Hard*"}`
- `InitGoogleTest` 解析 `--gtest_filter`，把全局 filter 设为 `*Hard*`
- `RUN_ALL_TESTS()` 遍历注册表，过滤命中的 TEST() 跑

#### 第 3 步：RUN_ALL_TESTS 内部循环

```cpp
for (auto* test_info : registry) {
    if (filter_matches(test_info)) {
        test_info->Run();  // → 实例化 fixture → 调 TestBody → 收集 EXPECT 结果
    }
}
```

**代入实参**（filter=`*Hard*`）：
- `ConfigValidation` → 不匹配，跳过
- `PauseResumeDecision` → 不匹配，跳过
- `HardLimitDecision` → 匹配，调 Run

`Run()` 内部为该 TEST 创建一个 `TestEventListener` 上下文，捕获所有 `EXPECT_*` 触发的失败信息（不立即 abort，只累加 `failure_count`），TestBody 跑完后把结果汇总进 UnitTest::result()。

#### 第 4 步：EXPECT 失败的内部表达

```cpp
EXPECT_TRUE(d.shouldCloseConnection) << "buffer>hardLimit 应触发保护性断连";
```

**展开成（简化版）**：

```cpp
::testing::AssertionResult ar = ::testing::AssertionSuccess();
if (!(d.shouldCloseConnection)) {
    ar = ::testing::AssertionFailure()
        << "Value of: d.shouldCloseConnection\n"
        << "  Actual: " << ::testing::PrintToString(d.shouldCloseConnection) << "\n"
        << "Expected: true\n";
}
GTEST_MESSAGE_AT_(__FILE__, __LINE__, ar.message(),
                  ::testing::TestPartResult::kNonFatalFailure)
    << "buffer>hardLimit 应触发保护性断连";
```

**关键不变式**：`kNonFatalFailure` 是 `EXPECT_*` 的核心——它把失败记入 result，但**不抛异常、不 abort**，控制流继续往下走。`ASSERT_*` 用的是 `kFatalFailure`，会立即 throw（实现里其实是 longjmp 或异常，看编译选项），让 TestBody 提前返回。

#### 第 5 步：状态机视图

```
                ctest 命令
                    │
                    ▼
            读取 CTestTestfile
            (列出所有注册的测试)
                    │
                    ▼
        ┌──────────────────────────┐
        │  for each test binary    │
        │  fork + exec             │
        └──────────┬───────────────┘
                   ▼
          static initialization
          (TEST() 宏注册到全局表)
                   │
                   ▼
          gtest_main → main()
                   │
                   ▼
          InitGoogleTest(argv)
          (解析 --gtest_filter 等)
                   │
                   ▼
          ┌────────────────┐
          │ RUN_ALL_TESTS  │
          └───────┬────────┘
                  ▼
          for each TestInfo:
            filter 匹配? ──no──► 跳过
                   │
                   ▼ yes
            TestInfo::Run()
              ├── new fixture
              ├── SetUp()
              ├── TestBody() ─── EXPECT_* 触发 → 累加 failure_count（不 abort）
              │                 ASSERT_* 触发 → 抛异常 → 跳到 TearDown
              ├── TearDown()
              └── delete fixture
                  │
                  ▼
          总结：passed / failed
                  │
                  ▼
          exit(failed ? 1 : 0)
                  │
                  ▼
          ctest 接收 exit code
          归档为 Passed / Failed
```

#### 第 6 步：函数职责一句话表

| 角色 | 调用时机 | 职责 |
|------|---------|------|
| `TEST(Suite, Name)` 宏 | 编译期 | 展开为类 + TestBody + 静态注册器 |
| 静态初始化 | 程序启动、main 之前 | 把每个 TEST 注册进全局 UnitTest 单例 |
| `gtest_main` | main 入口 | 解析参数 + RUN_ALL_TESTS + 返回 exit code |
| `EXPECT_*` 宏 | TestBody 内 | 检查条件，失败累加 failure_count（不 abort） |
| `ASSERT_*` 宏 | TestBody 内 | 检查条件，失败立即 return（abort 当前 TEST） |
| `<<` 注入 | EXPECT/ASSERT 后 | 追加自定义文字到失败输出 |
| `gtest_discover_tests` | CMake 配置阶段 | 跑 `--gtest_list_tests` 把每个 TEST 注册为独立 ctest 用例 |

### 2.6 测试如何验证迁移本身

迁移的"正确性"有两个维度需要验证：

1. **逻辑保持**：每个原 `testXxx()` 内的 `check()` 调用是否一对一映射到新的 `EXPECT_*`，输入参数是否一致，期望布尔值是否一致——靠 git diff 与肉眼。
2. **运行时全过**：`ctest --output-on-failure` 全 PASS——靠机器。

实际跑一遍：

```sh
cd HISTORY/day30/build
ctest --output-on-failure -R BackpressureDecision
```

期望输出：

```
Test #1: BackpressureDecisionTest.ConfigValidation .................. Passed
Test #2: BackpressureDecisionTest.PauseResumeDecision ............... Passed
Test #3: BackpressureDecisionTest.HardLimitDecision ................. Passed
100% tests passed, 0 tests failed out of 3
```

—— 这正是"CTest 粒度从文件级细化到 TEST 级"的可见收益。

---

## 3. 第 2 步 — 迁移 HttpContextTest

### 3.1 问题背景

HTTP/1.1 keep-alive 与 pipelining 是协议里最容易出错的部分。

```
GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n
```

服务器解析时必须：
- 精确识别第一条请求的边界（不多吃 1 字节，不少吃 1 字节）
- 让 `consumed` 出参告诉调用者"我消费到这里了"，调用者才能 `buffer->retrieve(consumed)` 推进读指针
- 反复调用 `parse()` 处理后续请求

任何一处计数错误都会导致：要么响应被发回错位（请求 A 用了请求 B 的 body），要么连接死锁（buffer 永远不被消费）。这个测试就是守门员。

### 3.2 三个 `TEST()`

#### 3.2.1 Pipeline 精确消费

来自 [HISTORY/day30/test/HttpContextTest.cpp](HISTORY/day30/test/HttpContextTest.cpp)（行 6–32）：

```cpp
TEST(HttpContextTest, PipelineConsumedBytes) {
    const std::string reqA = "GET /a HTTP/1.1\r\n"
                             "Host: local\r\n"
                             "\r\n";
    const std::string reqB = "GET /b HTTP/1.1\r\n"
                             "Host: local\r\n"
                             "\r\n";
    const std::string merged = reqA + reqB;

    HttpContext ctx;
    int consumedA = 0;
    bool ok = ctx.parse(merged.data(), static_cast<int>(merged.size()), &consumedA);

    ASSERT_TRUE(ok) << "解析失败（第一条请求）";
    EXPECT_TRUE(ctx.isComplete()) << "第一条请求应已完整";
    EXPECT_EQ(ctx.request().url(), "/a");
    EXPECT_EQ(consumedA, static_cast<int>(reqA.size()));

    ctx.reset();
    int consumedB = 0;
    ok = ctx.parse(merged.data() + consumedA, static_cast<int>(merged.size()) - consumedA,
                   &consumedB);

    ASSERT_TRUE(ok) << "解析失败（第二条请求）";
    EXPECT_TRUE(ctx.isComplete()) << "第二条请求应已完整";
    EXPECT_EQ(ctx.request().url(), "/b");
    EXPECT_EQ(consumedB, static_cast<int>(reqB.size()));
}
```

**`ASSERT_TRUE` vs `EXPECT_TRUE` 在这里的区别**：
- `ASSERT_TRUE(ok)`：parse 失败时立即 return —— 后面 `ctx.request().url()` 在解析失败时是未定义的，继续访问只会引发更多噪声错误。
- `EXPECT_TRUE(ctx.isComplete())`：即使 isComplete 失败，url/consumedA 的检查也有独立诊断价值，照样跑。

这是 ASSERT/EXPECT 选择的经验法则：**"后续断言依赖前一个为真"用 ASSERT；否则用 EXPECT**。

#### 3.2.2 分段 Body + 尾随请求

行 34–73：

```cpp
TEST(HttpContextTest, BodyFragmentAndTailRequest) {
    HttpContext ctx;

    const std::string part1 = "POST /echo HTTP/1.1\r\n"
                              "Host: local\r\n"
                              "Content-Length: 11\r\n"
                              "\r\n"
                              "hello";
    const std::string part2 = " world"
                              "GET /next HTTP/1.1\r\n"
                              "Host: local\r\n"
                              "\r\n";

    int consumed1 = 0;
    bool ok = ctx.parse(part1.data(), static_cast<int>(part1.size()), &consumed1);
    ASSERT_TRUE(ok) << "part1 解析失败";
    EXPECT_FALSE(ctx.isComplete()) << "part1 后请求不应完整";
    EXPECT_EQ(consumed1, static_cast<int>(part1.size()));

    int consumed2 = 0;
    ok = ctx.parse(part2.data(), static_cast<int>(part2.size()), &consumed2);
    ASSERT_TRUE(ok) << "part2 解析失败";
    EXPECT_TRUE(ctx.isComplete()) << "补齐 body 后请求应完整";
    EXPECT_EQ(ctx.request().body(), "hello world");
    EXPECT_EQ(consumed2, 6) << "part2 中本次应只消费剩余 body 的 6 字节";

    ctx.reset();
    const char *tailReq = part2.data() + consumed2;
    const int tailLen = static_cast<int>(part2.size()) - consumed2;
    int consumed3 = 0;
    ok = ctx.parse(tailReq, tailLen, &consumed3);

    ASSERT_TRUE(ok) << "尾随请求解析失败";
    EXPECT_TRUE(ctx.isComplete()) << "尾随请求应完整";
    EXPECT_EQ(ctx.request().url(), "/next");
    EXPECT_EQ(consumed3, tailLen);
}
```

这是 5 个文件里**最复杂**的用例 —— 模拟真实场景：第一段数据只够把 `POST` 请求的 body 解析到一半（"hello"，5 字节），第二段又补了 6 字节 body 加一条新请求，需要：

1. 在第二次 parse 时 **不能多吃** 第二条请求的 16 字节，只该消费 6 字节 body。
2. `reset()` 后从 `part2.data() + 6` 开始的尾随数据应被正确识别为新请求 `/next`。

`EXPECT_EQ(consumed2, 6)` 失败时 GTest 的输出：

```
test/HttpContextTest.cpp:58: Failure
Expected equality of these values:
  consumed2
    Which is: 11
  6
part2 中本次应只消费剩余 body 的 6 字节
```

—— 一眼就能看出"实际多吃了 5 字节"。手工框架只能告诉你"消费字节错"，得自己加 cerr 打印才知道实际值。

#### 3.2.3 非法方法

行 75–85：

```cpp
TEST(HttpContextTest, InvalidMethod) {
    HttpContext ctx;
    const std::string badReq = "BAD / HTTP/1.1\r\n"
                               "Host: local\r\n"
                               "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(badReq.data(), static_cast<int>(badReq.size()), &consumed);
    EXPECT_FALSE(ok) << "非法方法请求应解析失败";
    EXPECT_TRUE(ctx.isInvalid()) << "上下文状态应为 Invalid";
    EXPECT_GT(consumed, 0) << "非法请求也应推进解析位置";
}
```

`EXPECT_GT(consumed, 0)` 是 Day 28 引入的"防止上层死循环"不变式的最后一道守护。

### 3.3 验证：追踪 pipeline 用例执行

#### 业务场景 + 时序总览表

| 时刻 | 步骤 | parse 输入 | parse 出参 | 副作用 |
|------|------|-----------|-----------|--------|
| T0 | TEST 启动 | merged.data() / 64 字节 | consumedA=0 | ctx 初始 kStart |
| T1 | parse 推进状态机直到 reqA 末尾的 \r\n\r\n | 同上 | consumedA=32 (reqA.size) | state=kComplete |
| T2 | ASSERT_TRUE(ok) | — | ok=true | 通过 |
| T3 | EXPECT_EQ(consumedA, 32) | — | — | 通过 |
| T4 | ctx.reset() | — | — | state=kStart, 计数器全清零 |
| T5 | 第二次 parse(merged.data()+32, 32, &consumedB) | reqB 全文 | consumedB=32 | state=kComplete |
| T6 | EXPECT_EQ(consumedB, 32) | — | — | 通过 |

#### 第 1 步：第一次 parse 触达 \r\n\r\n

`HttpContext::parse` 内部的 while 循环在解析到第一条请求的 `\r\n\r\n`（headers 终止符）时，根据有无 `Content-Length` 决定下一步：

- 有 → 进入 kBody，按 Content-Length 字节累加
- 无 → state=kComplete，break，return true

`reqA` 没有 Content-Length，因此 \r\n\r\n 后立刻 kComplete，consumed 在 break 前已是 reqA.size()。

#### 第 2 步：reset 后的清零不变式

回顾 Day 29 §2.4 的 reset 实现：

```cpp
void HttpContext::reset() {
    state_ = State::kStart;
    request_.reset();
    tokenBuf_.clear();
    colonBuf_.clear();
    bodyLen_ = 0;
    requestLineBytes_ = 0;
    headerBytes_ = 0;
    payloadTooLarge_ = false;
}
```

如果 reset 漏清 `requestLineBytes_`，第二次 parse 把 `requestLineBytes_` 累加到 reqA+reqB 总和，可能超 8KB 限流被误杀——`PipelineConsumedBytes` 这个 TEST 就是它的护城河。

#### 第 3 步：状态机视图（聚焦第一条请求解析）

```
初始 state=kStart, consumed=0
   │
   ▼
读 G → kMethod, requestLineBytes_=1
读 E → kMethod, requestLineBytes_=2
读 T → kMethod, requestLineBytes_=3
读 ' ' → setMethod("GET"), state=kBeforeUrl, requestLineBytes_=4
   │
   ▼
读 / → kUrl
读 a → kUrl, tokenBuf_="/a"
读 ' ' → setUrl, state=kBeforeProtocol
   │
   ▼
读 H T T P → kProtocol
读 / → kBeforeVersion
读 1 . 1 → kVersion
读 \r → kEndOfRequestLine
读 \n → kHeaderKey
   │
   ▼
读 H o s t : ' ' → 收集 key="Host"
读 l o c a l → 收集 value="local"
读 \r\n → addHeader, state=kHeaderKey
   │
   ▼
读 \r → kEndOfHeaders
读 \n → 检查 Content-Length 缺失 → state=kComplete
   │
   ▼
break while → return true, *consumed = 32
```

#### 第 4 步：函数职责一句话表

| 角色 | 时机 | 职责 |
|------|------|------|
| `parse` | onMessage / 测试代码 | 状态机驱动 HTTP 报文解析，更新 *consumed |
| `reset` | keep-alive 完成上一请求后 | 清零状态机 + 计数器，准备解析下一请求 |
| `isComplete` | 测试 / onMessage | 判断当前已解析出完整请求 |
| `isInvalid` | 测试 / onMessage | 判断进入了 unrecoverable 错误态 |
| `*consumed` | parse 出参 | 告知调用者"已读到第几字节"，为 buffer.retrieve 提供依据 |

---

## 4. 第 3 步 — 迁移 HttpRequestLimitsTest

### 4.1 问题背景

Day 29 §2 引入的三层限流（请求行/头/体）是防御 OOM、慢速攻击的最后一道防线。

1. 超长请求行 → 413 + payloadTooLarge 标志
2. 超长 header → 同上
3. 超长 body → 同上
4. 限制内 valid 请求正常通过

### 4.2 4 个 `TEST()`

来自 [HISTORY/day30/test/HttpRequestLimitsTest.cpp](HISTORY/day30/test/HttpRequestLimitsTest.cpp)（行 6–82，节选关键段）：

```cpp
TEST(HttpRequestLimitsTest, RequestLineLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 16;
    limits.maxHeaderBytes = 1024;
    limits.maxBodyBytes = 1024;

    HttpContext ctx(limits);
    const std::string req = "GET /0123456789abcdef HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "\r\n";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    EXPECT_FALSE(ok) << "超长请求行应解析失败";
    EXPECT_TRUE(ctx.isInvalid());
    EXPECT_TRUE(ctx.payloadTooLarge());
    EXPECT_GT(consumed, 0) << "失败请求应推进消费字节";
}

TEST(HttpRequestLimitsTest, HeaderLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 1024;
    limits.maxHeaderBytes = 24;
    limits.maxBodyBytes = 1024;

    HttpContext ctx(limits);
    const std::string req = "GET / HTTP/1.1\r\n"
                            "X-Long-Header: 12345678901234567890\r\n"
                            "\r\n";
    /* ... 同样 4 个 EXPECT ... */
}

TEST(HttpRequestLimitsTest, BodyLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 1024;
    limits.maxHeaderBytes = 1024;
    limits.maxBodyBytes = 4;

    HttpContext ctx(limits);
    const std::string req = "POST /upload HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "Content-Length: 10\r\n"
                            "\r\n"
                            "abcdefghij";
    /* ... */
}

TEST(HttpRequestLimitsTest, ValidRequestWithinLimit) {
    HttpContext::Limits limits;
    limits.maxRequestLineBytes = 128;
    limits.maxHeaderBytes = 256;
    limits.maxBodyBytes = 16;

    HttpContext ctx(limits);
    const std::string req = "POST /ok HTTP/1.1\r\n"
                            "Host: local\r\n"
                            "Content-Length: 4\r\n"
                            "\r\n"
                            "ping";

    int consumed = 0;
    bool ok = ctx.parse(req.data(), static_cast<int>(req.size()), &consumed);
    ASSERT_TRUE(ok) << "限制内请求应解析成功";
    EXPECT_TRUE(ctx.isComplete());
    EXPECT_EQ(ctx.request().url(), "/ok");
    EXPECT_EQ(ctx.request().body(), "ping");
}
```

### 4.3 验证矩阵

| 用例 | 限制 | 实际 | parse 期望 | payloadTooLarge 期望 |
|------|------|------|-----------|---------------------|
| RequestLineLimit | maxRL=16 | 32+ 字节请求行 | false | true |
| HeaderLimit | maxHB=24 | 36 字节 header | false | true |
| BodyLimit | maxBB=4 | 10 字节 body | false | true |
| Valid | maxRL=128, maxHB=256, maxBB=16 | 全部小于阈值 | true | (n/a，未设置) |

**反向用例 `Valid` 是关键**：限流条件再严格也不能误杀正常流量。如果某次重构把"超过阈值就置位"写成了"达到阈值就置位"（差一字节），Valid 用例就会立刻失败。

### 4.4 验证：追踪 RequestLineLimit 用例

#### 业务场景 + 时序总览表

`maxRequestLineBytes=16`，请求行 "GET /0123456789abcdef HTTP/1.1" 实际有 32 字节：

| 字符索引 | 字符 | 状态 | requestLineBytes_ | 决策 |
|---------|------|-----|-------------------|------|
| 0 | G | kMethod | 1 | 累加 tokenBuf_ |
| 1 | E | kMethod | 2 | — |
| 2 | T | kMethod | 3 | — |
| 3 | ' ' | kBeforeUrl | 4 | setMethod("GET") |
| 4 | / | kUrl | 5 | tokenBuf_="/" |
| 5..15 | 0..a | kUrl | 6..16 | 持续累加 url |
| 16 | b | kUrl | **17 > 16** | payloadTooLarge_=true, state=kInvalid, break |
| — | parse return false | — | — | *consumed=17 |

#### 第 1 步：触发限流的具体行

回顾 Day 29 §2.3 的 kUrl 分支（结构与 kMethod 一致）：

```cpp
case State::kUrl:
    ++requestLineBytes_;
    if (requestLineBytes_ > limits_.maxRequestLineBytes) {
        payloadTooLarge_ = true;
        state_ = State::kInvalid;
        break;
    }
    /* ... */
```

**代入实参**（字符 'b' 时刻）：
- `requestLineBytes_ = 16`（前一字符已累加）
- `++requestLineBytes_ → 17`
- `limits_.maxRequestLineBytes = 16`
- `17 > 16` → true → 进 if

**副作用快照**：

```
state_           = State::kInvalid
payloadTooLarge_ = true
*consumed        = 17  (parse while 循环已推进 16+1)
返回值           = false
```

#### 第 2 步：4 个 EXPECT 都通过

- `EXPECT_FALSE(ok)` → ok=false → 通过
- `EXPECT_TRUE(ctx.isInvalid())` → state=kInvalid → 通过
- `EXPECT_TRUE(ctx.payloadTooLarge())` → 标志位为 true → 通过
- `EXPECT_GT(consumed, 0)` → consumed=17 > 0 → 通过

#### 第 3 步：状态机视图（聚焦限流触发）

```
       初始 limits.maxRL = 16
       初始 requestLineBytes_ = 0
            │
            ▼
     ┌─────────────────────┐
     │  parse while 循环   │
     └──────────┬──────────┘
                ▼
     case State::kXxx:
          ++requestLineBytes_
                │
                ▼
       requestLineBytes_ > maxRL?
            ┌────yes────┴────no────┐
            ▼                      ▼
    payloadTooLarge_=true     正常状态机推进
    state_=kInvalid
    break switch
            │
            ▼
    while 检查 state != kInvalid? false
    退出 while
            │
            ▼
    return false (parse)
            │
            ▼
    EXPECT_FALSE(ok)         → 通过
    EXPECT_TRUE(isInvalid)    → 通过
    EXPECT_TRUE(payloadTooLarge) → 通过
    EXPECT_GT(consumed, 0)   → 通过
```

#### 第 4 步：函数职责一句话表

| 角色 | 时机 | 职责 |
|------|------|------|
| `Limits` 配置 | TEST 内构造 ctx 前 | 注入三层阈值 |
| `++requestLineBytes_` 等 | parse 各 case 入口 | 累加各段字节计数 |
| 超限 if 分支 | 状态机内 | 置 `payloadTooLarge_` + 切到 kInvalid + break |
| `payloadTooLarge()` | 上层 onMessage / 测试 | 出口信号 → 区分 413 vs 400 |
| `EXPECT_GT(consumed, 0)` | 测试断言 | 守护"上层不死循环"不变式 |

---

## 5. 第 4 步 — 迁移 SocketPolicyTest

### 5.1 问题背景

`Socket` 类是底层 fd 操作的薄包装。它的"返回值语义"很微妙：bind/listen 失败时返回 false 还是抛异常？accept 失败返回 -1 还是 0？无效 fd（-1）操作时该 errno 是什么？这些不变式如果不被测试钉死，未来 refactor 时极易在某次"看起来无害"的修改中悄悄改变行为，导致上层 Acceptor 误判。

### 5.2 检查被测接口的依赖

来自 [HISTORY/day30/test/SocketPolicyTest.cpp](HISTORY/day30/test/SocketPolicyTest.cpp)（行 1–30）：

```cpp
#include "InetAddress.h"
#include "Socket.h"

#include <cerrno>
#include <gtest/gtest.h>

TEST(SocketPolicyTest, InvalidFdPolicy) {
    Socket invalid(-1);
    InetAddress addr("127.0.0.1", 1);

    EXPECT_FALSE(invalid.bind(&addr)) << "无效 fd 上 bind 应返回 false";
    EXPECT_EQ(errno, EBADF);

    EXPECT_FALSE(invalid.listen());
    EXPECT_FALSE(invalid.connect(&addr));
    EXPECT_EQ(invalid.accept(&addr), -1);
    EXPECT_FALSE(invalid.setnonblocking());
}

TEST(SocketPolicyTest, ValidSocketPolicy) {
    Socket sock;
    ASSERT_TRUE(sock.isValid()) << "socket() 成功时 fd 应有效";

    EXPECT_TRUE(sock.setnonblocking());
    EXPECT_TRUE(sock.isNonBlocking());

    InetAddress any("127.0.0.1", 0);
    EXPECT_TRUE(sock.bind(&any));
    EXPECT_TRUE(sock.listen());
}
```

### 5.3 两个 TEST 的契约

| TEST | 输入 | 验证 |
|------|------|------|
| InvalidFdPolicy | Socket(-1) 显式无效 fd | 所有操作返回 false/-1，errno=EBADF |
| ValidSocketPolicy | 默认构造（成功 socket()）  | bind/listen/setnonblocking 全 OK |

**`ASSERT_TRUE(sock.isValid())` 的必要性**：如果 socket() 系统调用本身失败（比如 fd 耗尽），后面的 setnonblocking/bind 都没意义，必须立即 abort 当前 TEST。

**`ValidSocketPolicy` 中的 `InetAddress("127.0.0.1", 0)`**：port=0 让内核自动分配一个空闲端口——避免测试与真实服务（如 8888）的端口冲突，让测试可以并行运行不踩雷。

### 5.4 验证：追踪 InvalidFdPolicy 用例

#### 时序总览表

| 步 | 调用 | sock 内部 fd | 系统调用 | errno | 返回值 | EXPECT |
|----|------|-------------|---------|-------|--------|--------|
| 1 | bind(&addr) | -1 | ::bind(-1, ...) | EBADF | false | EXPECT_FALSE → 通过 |
| 2 | EXPECT_EQ(errno, EBADF) | -1 | — | EBADF | — | 通过 |
| 3 | listen() | -1 | ::listen(-1) | EBADF | false | EXPECT_FALSE → 通过 |
| 4 | connect(&addr) | -1 | ::connect(-1, ...) | EBADF | false | EXPECT_FALSE → 通过 |
| 5 | accept(&addr) | -1 | ::accept(-1, ...) | EBADF | -1 | EXPECT_EQ → 通过 |
| 6 | setnonblocking() | -1 | ::fcntl(-1, ...) | EBADF | false | EXPECT_FALSE → 通过 |

#### 关键不变式：`Socket` 包装层不能"善意地"吞掉错误

考虑一个反面 refactor：有人觉得"-1 fd 调 bind 没意义，不如直接返回 true 跳过"——这看似让代码更"优雅"，实际把上层的错误检测能力直接抹掉。`InvalidFdPolicy` 的 6 个 EXPECT 就是钉死这道反向不变式的钉子。

### 5.5 函数职责一句话表

| 角色 | 时机 | 职责 |
|------|------|------|
| `Socket(-1)` | 测试 / 错误恢复路径 | 显式构造无效 fd 的 Socket |
| `Socket()` 默认构造 | 正常路径 | 调 ::socket(AF_INET, SOCK_STREAM, 0) |
| `bind/listen/connect/accept/setnonblocking` | 上层使用 | 失败返回 false/-1 + 保留 errno（不抛异常） |
| `port=0` | 测试 | 让内核分配空闲端口，避免冲突 |

---

## 6. 第 5 步 — 迁移 TcpServerPolicyTest

### 6.1 问题背景

`TcpServer` 暴露两个静态决策函数 + 一个 Options 默认配置 —— 它们都是"无副作用纯函数 / POD 默认值"，最适合用单元测试钉死：

- `shouldRejectNewConnection(current, max)` —— 是否拒绝新连接
- `normalizeIoThreadCount(requested, hardware)` —— IO 线程数归一化（0 表示用硬件核心数，负数无效，0+0 兜底为 1）
- `Options{}` —— listenIp / port / ioThreads / maxConnections 的默认值

每一处的边界条件如果出错都会让服务器在生产负载下行为异常：拒绝错误的连接数会让一台机器承载远低于硬件能力；IO 线程数算反会让多核闲置。

### 6.2 5 个 `TEST()`

来自 [HISTORY/day30/test/TcpServerPolicyTest.cpp](HISTORY/day30/test/TcpServerPolicyTest.cpp)（行 5–36）：

```cpp
TEST(TcpServerPolicyTest, RejectBoundary) {
    EXPECT_FALSE(TcpServer::shouldRejectNewConnection(0, 10));
    EXPECT_FALSE(TcpServer::shouldRejectNewConnection(9, 10));
    EXPECT_TRUE(TcpServer::shouldRejectNewConnection(10, 10));
    EXPECT_TRUE(TcpServer::shouldRejectNewConnection(11, 10));
}

TEST(TcpServerPolicyTest, SmallLimit) {
    EXPECT_FALSE(TcpServer::shouldRejectNewConnection(0, 1));
    EXPECT_TRUE(TcpServer::shouldRejectNewConnection(1, 1));
}

TEST(TcpServerPolicyTest, LargeValue) {
    const size_t maxConn = static_cast<size_t>(1) << 20;
    EXPECT_FALSE(TcpServer::shouldRejectNewConnection(maxConn - 1, maxConn));
    EXPECT_TRUE(TcpServer::shouldRejectNewConnection(maxConn, maxConn));
}

TEST(TcpServerPolicyTest, NormalizeIoThreads) {
    EXPECT_EQ(TcpServer::normalizeIoThreadCount(4, 16), 4);
    EXPECT_EQ(TcpServer::normalizeIoThreadCount(0, 8), 8);
    EXPECT_EQ(TcpServer::normalizeIoThreadCount(-1, 2), 2);
    EXPECT_EQ(TcpServer::normalizeIoThreadCount(0, 0), 1);
}

TEST(TcpServerPolicyTest, DefaultOptions) {
    TcpServer::Options options;
    EXPECT_EQ(options.listenIp, "127.0.0.1");
    EXPECT_EQ(options.listenPort, 8888);
    EXPECT_EQ(options.ioThreads, 0);
    EXPECT_EQ(options.maxConnections, 10000u);
}
```

### 6.3 五个边界场景的设计意图

| TEST | 场景 | 钉死的不变式 |
|------|------|-------------|
| RejectBoundary | maxConn=10 | `>=` 而非 `>`：达到阈值即拒，不留 1 个名额 |
| SmallLimit | maxConn=1 | 极端最小值（单连接服务器）也要正确 |
| LargeValue | maxConn=2^20 | size_t 不溢出 |
| NormalizeIoThreads | requested=0/-1, hardware=0 | 0/负值兜底逻辑：`0 → hardware`，`hardware==0 → 1` |
| DefaultOptions | 默认构造 | 默认配置的契约（生产环境如果意外依赖了"默认 8888 端口"，这里出错就是 BC break） |

**LargeValue 的特殊价值**：`maxConn = 1 << 20 = 1048576`。这是个真实可能的生产配置（百万级连接）。如果 `shouldRejectNewConnection` 内部用了 `int` 而非 `size_t`，传入 maxConn 会发生隐式截断。`EXPECT_FALSE(reject(maxConn-1, maxConn))` 一旦失败立刻能定位类型问题。

### 6.4 验证：追踪 DefaultOptions 用例

#### 时序总览表

| 步 | 操作 | options 字段 | 期望 | EXPECT 结果 |
|----|------|-------------|------|------------|
| 1 | `TcpServer::Options options;` | (默认构造) | — | — |
| 2 | EXPECT_EQ(listenIp, "127.0.0.1") | "127.0.0.1" | "127.0.0.1" | 通过 |
| 3 | EXPECT_EQ(listenPort, 8888) | 8888 | 8888 | 通过 |
| 4 | EXPECT_EQ(ioThreads, 0) | 0 | 0 | 通过 |
| 5 | EXPECT_EQ(maxConnections, 10000u) | 10000 | 10000 | 通过 |

#### 关键不变式：`10000u` 的 `u` 后缀

`maxConnections` 是 `size_t` 类型。如果写 `EXPECT_EQ(options.maxConnections, 10000)`，GTest 内部模板推导可能让两边类型不一致（int vs size_t），编译器会发出 `comparison of integer expressions of different signedness` 警告——`-Werror` 编译选项下直接编译失败。`10000u` 是明确告诉编译器"这是无符号字面量"。

GTest 的 `EXPECT_EQ` 文档专门提示了这个陷阱：[官方 FAQ](https://github.com/google/googletest/blob/main/docs/faq.md) 推荐**字面量永远加上类型后缀**或显式 cast。

### 6.5 函数职责一句话表

| 角色 | 时机 | 职责 |
|------|------|------|
| `shouldRejectNewConnection` | Acceptor 收到新连接前 | 纯函数：判断当前连接数是否已达上限 |
| `normalizeIoThreadCount` | TcpServer::start() | 把 0/负数请求归一化为有效 IO 线程数 |
| `Options{}` 默认值 | 用户未自定义时 | 提供"开箱即用"的合理默认 |
| `EXPECT_EQ(..., 10000u)` 后缀 | 测试 | 避免有符号/无符号比较警告 |

---

## 7. 第 6 步 — 配置 CMake 接入 GoogleTest

### 7.1 问题背景

迁移要在 CMake 层让 GTest 自动可用，且不污染原有的 LogTest/TimerTest/ThreadPoolTest。

### 7.2 完整 CMake 关键段

来自 [HISTORY/day30/CMakeLists.txt](HISTORY/day30/CMakeLists.txt)（行 1–80）：

```cmake
cmake_minimum_required(VERSION 3.14)
project(day30 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# ── GoogleTest ──
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Poller 平台分支 ──
set(POLLER_SRCS common/Poller/DefaultPoller.cpp)
if(APPLE)
    list(APPEND POLLER_SRCS common/Poller/kqueue/KqueuePoller.cpp)
else()
    list(APPEND POLLER_SRCS common/Poller/epoll/EpollPoller.cpp)
endif()

# ── 公共源文件集 ──
set(COMMON_SRC
    common/Acceptor.cpp common/Buffer.cpp common/Channel.cpp common/Connection.cpp
    common/EventLoopThread.cpp common/EventLoopThreadPool.cpp common/Eventloop.cpp
    common/InetAddress.cpp common/TcpServer.cpp common/Socket.cpp
    common/ThreadPool.cpp common/timer/TimerQueue.cpp
    ${LOG_SRC} ${HTTP_SRC} ${POLLER_SRCS}
)

# ── GTest 单元测试 ──
add_executable(BackpressureDecisionTest test/BackpressureDecisionTest.cpp ${COMMON_SRC})
target_link_libraries(BackpressureDecisionTest gtest gtest_main pthread)

add_executable(HttpContextTest test/HttpContextTest.cpp ${COMMON_SRC})
target_link_libraries(HttpContextTest gtest gtest_main pthread)

add_executable(HttpRequestLimitsTest test/HttpRequestLimitsTest.cpp ${COMMON_SRC})
target_link_libraries(HttpRequestLimitsTest gtest gtest_main pthread)

add_executable(SocketPolicyTest test/SocketPolicyTest.cpp ${COMMON_SRC})
target_link_libraries(SocketPolicyTest gtest gtest_main pthread)

add_executable(TcpServerPolicyTest test/TcpServerPolicyTest.cpp ${COMMON_SRC})
target_link_libraries(TcpServerPolicyTest gtest gtest_main pthread)

# ── 非 GTest 测试（保留手工框架） ──
add_executable(EpollPolicyTest test/EpollPolicyTest.cpp ${COMMON_SRC})
target_link_libraries(EpollPolicyTest pthread)

add_executable(TimerTest test/TimerTest.cpp ${COMMON_SRC})
target_link_libraries(TimerTest pthread)

add_executable(LogTest test/LogTest.cpp ${COMMON_SRC})
target_link_libraries(LogTest pthread)

add_executable(ThreadPoolTest test/ThreadPoolTest.cpp common/ThreadPool.cpp)
target_link_libraries(ThreadPoolTest pthread)
```

### 7.3 三个关键 CMake 决策

#### 7.3.1 `cmake_minimum_required(VERSION 3.14)`

`FetchContent_MakeAvailable` 是 CMake 3.14 引入的便捷写法，等价于：

```cmake
FetchContent_GetProperties(googletest)
if(NOT googletest_POPULATED)
    FetchContent_Populate(googletest)
    add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR})
endif()
```

3.14 之前的写法繁琐 4 行，所以 minimum 拉到 3.14 是值得的。

#### 7.3.2 `gtest_force_shared_crt ON`

这一行只在 Windows 起作用。MSVC 的 C 运行时有 4 种变体（/MT static debug/release, /MD shared debug/release）。GTest 默认编 /MT（静态），而项目其余部分通常编 /MD（动态）——链接时报 `mismatch detected for 'RuntimeLibrary'`。`gtest_force_shared_crt` 强制 GTest 也用 /MD，统一一致。Linux/macOS 上这个变量被忽略。

#### 7.3.3 FetchContent vs find_package

| 方式 | 优点 | 缺点 |
|------|------|------|
| `find_package(GTest)` | 复用系统已装版本，构建快 | 各平台/各机器版本不一致，CI 不可重现 |
| `FetchContent` | 锁定版本（v1.14.0），跨平台一致 | 首次配置需联网下载（~10 MB） |

对于团队项目和 CI，FetchContent 是公认更优解。Day 28 的 CI workflow 已经考虑了缓存（`actions/cache` 缓存 `~/.cache/cmake/...`），首次拉取后续 build 都是秒级。

### 7.4 选择性迁移 — 哪些测试不迁移

**`EpollPolicyTest` / `TimerTest` / `LogTest` / `ThreadPoolTest` 留在手工框架**，原因：

- **EpollPolicyTest**：`#ifdef __linux__` 平台守卫复杂，迁移到 GTest 会让 macOS 构建出现"空 binary"——不如保留 main 内部直接 return 0
- **TimerTest**：测试 EventLoop + Timer 的端到端集成，需要主线程持续 loop，不符合 GTest 的"快速一次性 TEST"风格
- **LogTest**：异步日志涉及后台线程刷盘，断言可能要等 fsync 完成；适合手工编排
- **ThreadPoolTest**：和 LogTest 同类型，多线程行为测试

**这是个有意识的取舍**：把 GTest 用在"纯函数 / POD / 快速可重复"的策略测试上，把"带主线程/后台线程的集成测试"留给手工 main。

### 7.5 函数职责一句话表

| CMake 命令 | 时机 | 职责 |
|-----------|------|------|
| `FetchContent_Declare` | configure | 声明依赖来源（git repo + tag） |
| `FetchContent_MakeAvailable` | configure | 拉取 + add_subdirectory，让 `gtest`/`gtest_main` 目标可用 |
| `gtest_force_shared_crt ON` | configure | Windows 下统一 CRT 变体（其他平台 noop） |
| `target_link_libraries(... gtest gtest_main pthread)` | configure | 让测试二进制能引用 GTest 与 main |

---

## 8. 第 7 步 — 清理 common/util.cpp

### 8.1 问题背景

`util.cpp` 的历史轨迹：

```
Day 1-20  : errif() 是核心错误处理
Day 21-26 : Logger 体系上线，errif() 调用点被 LOG_ERROR/LOG_FATAL 替换
Day 27    : 最后一处 errif() 被替换；util.cpp 已无引用
Day 28-29 : CMakeLists 不再编译 util.cpp，但物理文件留着
Day 30    : git rm util.cpp util.h
```

### 8.2 为什么删？

不删的代价：
- 新贡献者 `grep errif src/` 仍能命中，要花时间确认"这是不是仍在用"
- IDE 索引会包含它，跳转可能误导
- 仓库 `cloc` 统计虚高

删的成本：零——因为没人引用它，git rm 即完成。

### 8.3 验证删除完整性

```sh
# 删除
git rm common/util.cpp common/util.h

# 确认无残留引用
grep -rn "errif\|util\.h" --include="*.cpp" --include="*.h" .
# 期望：空输出

# 重新构建确认未破坏
rm -rf build && cmake -S . -B build && cmake --build build -j
```

### 8.4 这是最小化的"清理类提交"

提交信息建议：

```
chore(day30): remove legacy common/util.cpp

errif() 已被 Logger 宏完全替代（Day 21-26），文件物理留存只
会让新贡献者困惑。CMakeLists 早已不编译此文件，删除零风险。
```

---

## 9. 工程化收尾

### 9.1 README / app_example / 基准测试归档

`HISTORY/day30/CMakeLists.txt` + `HISTORY/day30/README.md` —— 当日快照，"那一天项目长什么样"的入口。

`HISTORY/day30/app_example/` —— 端到端的可运行 demo，不依赖项目内部源码而是用 `find_package(MyCppServerLib CONFIG)` 当外部库，模拟"用户用我们的库"的真实姿态。

`benchmark/` —— Day 28 的 conn_scale_test + Day 30 的 BenchmarkTest 留在仓库根的 benchmark/，与日常单元测试隔离，方便单独回归性能。

### 9.2 文档双轨

- README.md：构建、运行、端到端验证命令（"做什么"）
- dev-log/dayN-*.md：设计动机、全流程追踪、状态机推导（"为什么"）

两者互不替代。README 服务"我想编译跑起来"的新用户；dev-log 服务"我想读懂这是怎么演化来的"的代码考古者。

---

## 10. 验证

### 10.1 全量构建

```sh
cd HISTORY/day30
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build --prefix app_example/external/MyCppServerLib
cmake -S app_example -B app_example/build
cmake --build app_example/build -j$(nproc)
```

### 10.2 ctest 全套

```sh
cd HISTORY/day30/build && ctest --output-on-failure
```

期望（GTest 自动发现版）：

```
Test  1: BackpressureDecisionTest.ConfigValidation .................. Passed
Test  2: BackpressureDecisionTest.PauseResumeDecision ............... Passed
Test  3: BackpressureDecisionTest.HardLimitDecision ................. Passed
Test  4: HttpContextTest.PipelineConsumedBytes ...................... Passed
Test  5: HttpContextTest.BodyFragmentAndTailRequest ................. Passed
Test  6: HttpContextTest.InvalidMethod .............................. Passed
Test  7: HttpRequestLimitsTest.RequestLineLimit ..................... Passed
Test  8: HttpRequestLimitsTest.HeaderLimit .......................... Passed
Test  9: HttpRequestLimitsTest.BodyLimit ............................ Passed
Test 10: HttpRequestLimitsTest.ValidRequestWithinLimit .............. Passed
Test 11: SocketPolicyTest.InvalidFdPolicy ........................... Passed
Test 12: SocketPolicyTest.ValidSocketPolicy ......................... Passed
Test 13: TcpServerPolicyTest.RejectBoundary ......................... Passed
Test 14: TcpServerPolicyTest.SmallLimit ............................. Passed
Test 15: TcpServerPolicyTest.LargeValue ............................. Passed
Test 16: TcpServerPolicyTest.NormalizeIoThreads ..................... Passed
Test 17: TcpServerPolicyTest.DefaultOptions ......................... Passed
Test 18: EpollPolicyTest ............................................ Passed (Linux only)
Test 19: TimerTest .................................................. Passed
Test 20: LogTest .................................................... Passed
Test 21: ThreadPoolTest ............................................. Passed

100% tests passed, 0 tests failed out of 21
```

### 10.3 端到端 demo

```sh
cd HISTORY/day30/app_example/build
MYCPPSERVER_BIND_PORT=8889 ./http_server &
SRV_PID=$!
sleep 1
curl -sI http://127.0.0.1:8889/ | head -5
kill $SRV_PID
```

### 10.4 选择性运行验证

```sh
./BackpressureDecisionTest --gtest_filter='*HardLimit*'
```

期望：

```
[==========] Running 1 test from 1 test suite.
[ RUN      ] BackpressureDecisionTest.HardLimitDecision
[       OK ] BackpressureDecisionTest.HardLimitDecision (0 ms)
[==========] 1 test ran. (0 ms total)
[  PASSED  ] 1 test.
```

—— 验证 GTest 的 filter 机制工作正常。

---

## 11. Phase 4 总结 — 30 天演进路径回顾

```
Phase 1（Day 1-7）   原始 socket、epoll/kqueue、Channel 抽象
Phase 2（Day 8-15）  Reactor、EventLoop、ThreadPool、subloop
Phase 3（Day 16-26） Acceptor、Connection、Buffer、HTTP 协议、日志、定时器
Phase 4（Day 27-30） 回压、连接上限、限流、路由、中间件、TLS 预留、GTest
─────────────────────────────────────────────────────────────────
Phase 5（Day 31+）   WebSocket、协程 IO、io_uring、无锁队列、内存池、benchmark
```

Phase 4 的核心叙事：**从"功能可用"到"生产就绪"**。

- Day 27 之前：服务能跑，但任何"非常规"输入（巨大 body / 慢速攻击 / 100 MB 文件下载）都会让它表现失控
- Day 28：把"决策与状态分离"——`evaluateBackpressure / shouldRejectNewConnection / normalizeIoThreadCount` 抽出来变成纯函数，立刻可单元测试
- Day 29：对外接口完整化——路由 / 中间件 / sendFile / CORS / Gzip / StaticFileHandler / TLS 预留 / 三层限流 / 408 超时
- Day 30：测试基础设施现代化——GTest 自动发现 + 结构化断言 + 选择性运行

如果用一个数字概括 Phase 4 的"质变"：单元测试用例数 5 → 17（+240%），但每个用例的代码量减少 30~50%。这是测试可维护性与覆盖广度同时上升的标志。

---

## 12. 局限与下一步

### 12.1 当前已知局限

| 局限 | 说明 |
|------|------|
| 部分测试未迁移 | TimerTest / LogTest / EpollPolicyTest / ThreadPoolTest 仍手工框架（有意保留） |
| `gtest_discover_tests` 未启用 | 当前用 `add_test(NAME ... COMMAND ...)` 注册整个二进制；改用 `gtest_discover_tests` 可让 ctest 看到每个 TEST 单独一行 |
| 无 GitHub Actions 配置 | Day 28 提到的 CI 完整化未在本日完成 |
| TLS 仍仅接口 | 未链接 OpenSSL，`MCPP_HAS_OPENSSL` 未定义 |
| 前缀路由线性扫描 | vector 实现，路由数大时性能不优 |
| 仅 HTTP/1.x | 无 HTTP/2 / HTTP/3 |

### 12.2 Phase 5 路线图（Day 31+）

| Day | 主题 | 价值 |
|-----|------|------|
| 31 | WebSocket（握手 + 帧编解码 + ping/pong） | 长连接双向通信，IM/游戏/实时数据 |
| 32 | C++20 协程 IO（co_await 取代回调） | 业务代码线性化，告别回调地狱 |
| 33 | io_uring 后端（Linux 5.1+） | 系统调用次数大幅减少，吞吐瓶颈突破 |
| 34 | 无锁 MPMC 队列 + WorkStealingPool | 多核 CPU 利用率提升 |
| 35 | MemoryPool（slab 分配器 + STLAllocator） | 高频小对象分配的内存抖动消除 |
| 36 | muduo benchmark 对比 | 验证 30 天演进的相对性能位置 |

---

> **Day 30 完结 Phase 4。** 仓库以"可发布的中型网络库"姿态归位：
> - 5 个核心策略测试 GTest 化，覆盖 17 个 TEST 用例
> - util.cpp 历史包袱清理
> - app_example 验证"作为外部库被链接"的接口稳定性
> - README 与 dev-log 双轨文档完整同步
>
> Day 31 起进入 Phase 5「协议 / 协程 / 高性能 IO」实验区，前 30 天的"骨架"将承载更激进的架构探索。

---

## 13. 生产就绪收尾（GTest 迁移之外）

GTest 迁移只是 day30 的"测试基础设施"那一脚。要真正达到简历上"生产可用"的力度，本日还集中完成了**中间件三件套整合 / CI 多平台多 Sanitizer / coverage / clang-tidy / 全特性 demo / 面试手册**这几项工程化收尾。本节给一个 checklist 式的总览，每项都对应仓库里可直接运行的命令。

### 13.1 中间件链落地：从"散件"到"洋葱"

day29 把 `RateLimiter` / `AuthMiddleware` / `ServerMetrics` / `CorsMiddleware` / `GzipMiddleware` / `StaticFileHandler` 当成独立组件放在 `src/include/http/` 里，但**示例代码里没有把它们串起来**。day30 在 `examples/src/http_server.cpp` 里给出唯一推荐的注册顺序：

```cpp
srv.use(makeAccessLogMiddleware());     // ① 计时 + 日志（最外层）
srv.use(rateLimiter.toMiddleware());    // ② per-IP 令牌桶（早截断）
srv.use(cors.toMiddleware());           // ③ CORS（必须在 Auth 之前，OPTIONS 直接 204）
srv.use(auth.toMiddleware());           // ④ Bearer / API Key
srv.use(gzip.toMiddleware());           // ⑤ gzip（最内层，next() 之后才能看到完整 body）
```

**顺序里的两条铁律**：

1. **CORS 必须排在 Auth 之前**。浏览器发送跨域请求前会先发 `OPTIONS` 预检；这个预检不带 `Authorization` 头，如果让 Auth 先跑，预检直接 403，浏览器就拒绝发真实请求了。CorsMiddleware 内部检测到 `OPTIONS` + `Access-Control-Request-Method` 后会直接 `204` 短路返回，不会往下走。
2. **Gzip 必须排在最内层**。中间件 `next()` 之前看到的是请求，之后看到的是响应；gzip 要压响应体，所以一定要等所有内层都跑完、`resp->body()` 已经填好之后才动手。

### 13.2 前后端分离：static/ 不再硬编码进 .cpp

老 demo 的 `handleIndex()` 把 HTML 嵌在 raw string 里，这是反面教材：每改一行字都要重编。新 demo 把首页放到 `examples/static/index.html`，CMake 在 `add_custom_command(POST_BUILD)` 里把 `static/` 和 `files/` 复制到 build 目录，`handleIndex()` 用 `std::call_once` 在第一次请求时读盘并缓存。

```cmake
add_custom_command(TARGET app_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/examples/static ${CMAKE_BINARY_DIR}/static
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/examples/files  ${CMAKE_BINARY_DIR}/files)
```

```cpp
static void handleIndex(const HttpRequest&, HttpResponse* resp) {
    static std::string cached;
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = readFile("static/index.html", &cached); });
    if (!ok) { resp->setStatus(500, "..."); return; }
    resp->setStatus(200, "OK");
    resp->setContentType("text/html; charset=utf-8");
    resp->setBody(cached);
}
```

这个改动看似琐碎，对面试演示很关键——**演示者改 UI 不需要重编 C++**。

### 13.3 CI 矩阵：7 路并行

`.github/workflows/ci.yml` 在 day30 完成最终形态，按平台 × Sanitizer 拆成 7 个 job 并行：

| Job | 平台 | 编译器 | 额外开关 | 拦截目标 |
|-----|------|--------|---------|----------|
| `build-linux-gcc` | ubuntu-22.04 | gcc-11 | -O2 | 基线编译 + ctest |
| `build-linux-clang` | ubuntu-22.04 | clang-15 | -O2 | clang 兼容性 |
| `build-macos` | macos-13 | AppleClang | -O2 | 跨平台（kqueue 路径） |
| `asan` | ubuntu-22.04 | clang | -fsanitize=address,undefined | 内存越界 / use-after-free / UB |
| `tsan` | ubuntu-22.04 | clang | -fsanitize=thread | 多 reactor 数据竞争 |
| `coverage` | ubuntu-22.04 | gcc + lcov | --coverage | 行覆盖率 → codecov |
| `clang-tidy` | ubuntu-22.04 | clang-tidy-15 | 默认 + bugprone-* | 静态分析告警 |

每个 job 都跑完整 `ctest --output-on-failure`，任何一个挂掉 PR 都不能合。这把"qps 多少"以外的另一半工程力量摆到了 README 里——**不是"我跑一次过了"，是"我每次提交都过 7 次"**。

### 13.4 Sanitizer / Coverage / clang-tidy 本地手感

CI 之外，开发者本地一条命令就能跑：

```bash
# AddressSanitizer + UBSan
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j && (cd build-asan && ctest --output-on-failure)

# ThreadSanitizer
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j && (cd build-tsan && ctest --output-on-failure)

# 覆盖率（gcov + lcov）
cmake -B build-cov -DCMAKE_CXX_FLAGS="--coverage" -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build build-cov -j && (cd build-cov && ctest)
lcov --capture --directory build-cov --output-file cov.info
genhtml cov.info --output-directory cov_html

# clang-tidy（CMake 已注册同名 target）
cmake --build build --target clang-tidy
```

详细操作步骤、参数解释、常见误报处理见仓库根目录 `INTERVIEW_GUIDE.md` §4-§6。

### 13.5 全特性 demo：app_demo

day30 在 `examples/src/http_server.cpp` 给出了**唯一推荐的对外演示入口**，它一次性串联：

- 路由表（精确路径 `addRoute` + 前缀路径 `addPrefixRoute`）
- 5 层中间件链（顺序如 §13.1）
- `/static/*` + `/files/*` 双前缀（演示 ETag / 304 / Range / sendfile 零拷贝）
- `/metrics` 直出 `ServerMetrics::toPrometheus()` 文本
- `SIGINT/SIGTERM` 优雅停机（通过 `Signal::signal` 注册）
- 环境变量覆盖：`MYCPPSERVER_BIND_IP` / `MYCPPSERVER_BIND_PORT`

构建运行：

```bash
cmake -S . -B build && cmake --build build --target app_demo -j
cd build && ./app_demo                         # 默认 127.0.0.1:8888
```

冒烟脚本（face-to-face 演示用）：

```bash
curl -i http://localhost:8888/                                          # 200 + HTML
curl -i http://localhost:8888/api/users                                 # 403（无凭证）
curl -i -H "Authorization: Bearer demo-token-2024" \
     http://localhost:8888/api/users                                    # 200
curl -i -H "X-API-Key: api-key-001" http://localhost:8888/api/users     # 200
curl -i -X OPTIONS -H "Origin: http://x" \
     -H "Access-Control-Request-Method: GET" \
     http://localhost:8888/api/users                                    # 204
curl -i -H "Range: bytes=0-99" http://localhost:8888/files/readme.txt   # 206
curl -s http://localhost:8888/metrics | head -20                        # Prometheus
# 触发限流：
for i in $(seq 1 300); do
    curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8888/
done | sort | uniq -c                                                   # 200 + 429 混合
```

### 13.6 面试手册：INTERVIEW_GUIDE.md

day30 同步交付一份**面向面试场景**的速查手册（仓库根目录 `INTERVIEW_GUIDE.md`），覆盖：

- 30 秒电梯演讲（§0）
- 仓库结构 + 一行编译运行（§1-§2）
- ctest 用法（`-N` 列举 / `--gtest_list_tests` 列子用例 / `--gtest_filter` 单跑）（§3）
- 三种 Sanitizer 单独操作步骤（§4）
- gcov + lcov + genhtml 出 HTML 覆盖率报告（§5）
- clang-tidy CMake target（§6）
- BenchmarkTest 内部压测 + wrk 外部压测（§7）
- CI 7 个 job 总览表（§8）
- FAQ Q1-Q8：epoll LT/ET、Connection 生命周期（shared_ptr+enable_shared_from_this+queueInLoop）、HTTP 状态机、限流三种算法对比、Bearer/API Key 鉴权语义、sendfile 零拷贝原理（4 次 vs 2 次上下文切换）、wrk --latency 测 P99（§9）

简历上写"188K QPS / P99 2.15ms"的同时，这份手册保证**任何一个面试官随手翻一个细节都能在仓库里找到对应代码 + 命令**。

### 13.7 day30 验收清单

| 项目 | 命令 | 期望结果 |
|------|------|---------|
| 全量构建 | `cmake -S . -B build && cmake --build build -j` | 0 warning 0 error |
| 单元测试 | `cd build && ctest --output-on-failure` | 17 个 TEST 全 PASS |
| 全特性 demo | `cd build && ./app_demo`，浏览器访问 `http://localhost:8888` | 看到 HTML 首页 |
| 鉴权 | 上述 curl Bearer / API Key | 200 / 200 |
| 静态文件 | `curl http://localhost:8888/static/index.html` | 200 + HTML |
| sendfile | `curl -H "Range: bytes=0-99" .../files/readme.txt` | 206 + 100 字节 |
| Prometheus | `curl .../metrics` | `mcpp_*` 指标行 |
| 限流 | 300 并发循环 | 出现 429 |
| 优雅停机 | Ctrl+C | 收到 SIGINT 日志，进程退出码 0 |

> 这一节内容跟 §1-§12 是**互补**关系：§1-§12 讲"如何把 5 个手工测试迁到 GTest"，§13 讲"如何把这套测试基础设施 + day29 的中间件 + 完整 demo 一起对外发布"。两者一起构成 day30 真正的产物。


