#!/bin/bash
# ──────────────────────────────────────────────────────────────────────
# demo_phase4.sh — Phase 4 功能演示与性能对比脚本
#
# 用法：
#   chmod +x demo/demo_phase4.sh
#   ./demo/demo_phase4.sh
#
# 前置条件：
#   1. 已在 build/ 下构建（cmake -B build && cmake --build build）
#   2. demo_server 已编译
#   3. curl 已安装
# ──────────────────────────────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
SERVER_PID=""

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m' # No Color
BOLD='\033[1m'

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

print_header() {
    echo ""
    echo -e "${BOLD}${CYAN}════════════════════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}${CYAN}  $1${NC}"
    echo -e "${BOLD}${CYAN}════════════════════════════════════════════════════════════════${NC}"
}

print_step() {
    echo -e "\n${GREEN}▶ $1${NC}"
}

print_cmd() {
    echo -e "  ${YELLOW}\$ $1${NC}"
}

# ── 构建 ──────────────────────────────────────────────────────────────
print_header "Phase 4 功能演示 — Airi-Cpp-Server-Lib"

print_step "构建项目..."
cd "$PROJECT_DIR"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMCPP_ENABLE_TESTING=ON > /dev/null 2>&1
cmake --build build --parallel > /dev/null 2>&1
echo "  构建完成 ✓"

# ── 启动 demo_server ──────────────────────────────────────────────────
print_step "启动 Phase 4 Demo Server (port 9090)..."
"$BUILD_DIR/demo_server" &
SERVER_PID=$!
sleep 1

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo -e "${RED}  服务器启动失败！${NC}"
    exit 1
fi
echo "  服务器已启动 (PID=$SERVER_PID) ✓"

# ── 功能演示 1：可观测性指标端点 ──────────────────────────────────────
print_header "1. 可观测性指标端点 /metrics"
print_cmd "curl -s http://127.0.0.1:9090/metrics"
echo ""
curl -s http://127.0.0.1:9090/metrics
echo ""

# ── 功能演示 2：结构化日志上下文 ──────────────────────────────────────
print_header "2. 结构化日志上下文"
echo "  每条请求日志自动携带 [req-ID METHOD /url] 前缀"
echo "  示例：2024-01-01 12:00:00.000000 T1 INFO  [req-1 GET /] handler called - HttpServer.cpp:42"
print_cmd "curl -s http://127.0.0.1:9090/"
curl -s http://127.0.0.1:9090/ > /dev/null
echo "  (查看服务器 stdout/日志文件可见结构化日志输出)"

# ── 功能演示 3：单调时钟 ──────────────────────────────────────────────
print_header "3. 单调时钟重构 (SteadyClock)"
echo "  请求延迟测量已从 gettimeofday() (wall clock) 迁移到"
echo "  std::chrono::steady_clock (monotonic clock)。"
echo ""
echo "  优势："
echo "    - 免疫 NTP 校时跳变（wall clock 可能回退导致延迟测量为负）"
echo "    - 免疫系统休眠/恢复造成的时间跳跃"
echo "    - 保证 elapsed = end - start ≥ 0（单调递增）"
echo ""
echo "  影响范围："
echo "    - HttpServer::onMessage() 中的请求超时检测"
echo "    - HttpServer::onRequest() 中的延迟统计上报"
echo "    - ServerMetrics 延迟分桶"

# ── 功能演示 4：限流中间件 ──────────────────────────────────────────
print_header "4. 限流中间件 (RateLimiter)"
print_step "正常请求："
print_cmd 'curl -s "http://127.0.0.1:9090/api/echo?msg=hello"'
curl -s "http://127.0.0.1:9090/api/echo?msg=hello"
echo ""

print_step "快速发送 150 请求以触发限流..."
SUCCESS=0
RATE_LIMITED=0
for i in $(seq 150); do
    CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:9090/api/echo?msg=test$i")
    if [ "$CODE" = "200" ]; then
        SUCCESS=$((SUCCESS + 1))
    elif [ "$CODE" = "429" ]; then
        RATE_LIMITED=$((RATE_LIMITED + 1))
    fi
done
echo "  结果：成功=$SUCCESS  被限流(429)=$RATE_LIMITED"

# ── 功能演示 5：鉴权中间件 ──────────────────────────────────────────
print_header "5. 鉴权中间件 (AuthMiddleware)"

print_step "无 token 访问受保护端点："
print_cmd "curl -s http://127.0.0.1:9090/api/admin/status"
curl -s http://127.0.0.1:9090/api/admin/status
echo ""

print_step "使用 Bearer Token 访问："
print_cmd 'curl -s -H "Authorization: Bearer demo-token-2024" http://127.0.0.1:9090/api/admin/status'
curl -s -H "Authorization: Bearer demo-token-2024" http://127.0.0.1:9090/api/admin/status
echo ""

print_step "使用 API Key 访问："
print_cmd 'curl -s -H "X-API-Key: demo-key-001" http://127.0.0.1:9090/api/admin/status'
curl -s -H "X-API-Key: demo-key-001" http://127.0.0.1:9090/api/admin/status
echo ""

# ── 功能演示 6：压测对比（使用内置 BenchmarkTest）──────────────────
print_header "6. 性能基准（BenchmarkTest 4线程 × 10秒）"
print_step "启动压测..."
if [ -f "$BUILD_DIR/BenchmarkTest" ]; then
    "$BUILD_DIR/BenchmarkTest" 127.0.0.1 9090 / 4 10 2>&1 || true
else
    echo "  BenchmarkTest 未编译，跳过"
fi

# ── 最终 metrics 快照 ────────────────────────────────────────────────
print_header "7. 最终 /metrics 快照"
print_cmd "curl -s http://127.0.0.1:9090/metrics"
echo ""
curl -s http://127.0.0.1:9090/metrics
echo ""

# ── 测试套件 ──────────────────────────────────────────────────────────
print_header "8. 全量测试套件"
print_step "运行 ctest..."
cd "$BUILD_DIR"
ctest --output-on-failure 2>&1

# ── 总结 ──────────────────────────────────────────────────────────────
print_header "Phase 4 演示完成"
echo ""
echo "  新增特性总览："
echo "    ✓ 可观测性指标端点 (/metrics, Prometheus text format)"
echo "    ✓ 结构化日志上下文 (LogContext: req-ID, METHOD, URL)"
echo "    ✓ 单调时钟重构 (gettimeofday → steady_clock)"
echo "    ✓ 令牌桶限流中间件 (RateLimiter, 429 Too Many Requests)"
echo "    ✓ Bearer Token / API Key 鉴权中间件 (AuthMiddleware)"
echo ""
echo "  测试覆盖：31 test cases (gtest/ctest)"
echo "  新增文件：6 headers + 3 test suites + 1 demo server"
echo ""

cleanup
echo -e "${GREEN}演示结束。${NC}"
