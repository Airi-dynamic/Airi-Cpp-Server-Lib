#!/usr/bin/env bash
# bench.sh — 压测一键脚本
# 由 .vscode/tasks.json "bench:wrk+rss" 任务调用。
# 支持环境变量覆盖：MYCPPSERVER_BIND_PORT（默认 18888）

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${MYCPPSERVER_BIND_PORT:-18888}"

# ── 1. 启动服务器（后台）────────────────────────────────────────────────────
echo "── starting server on 127.0.0.1:${PORT} ──"
MYCPPSERVER_BIND_PORT="${PORT}" "${ROOT_DIR}/build/examples/http_server" > /tmp/airi_server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    echo
    echo "── stopping server (pid=${SERVER_PID}) ──"
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

# ── 2. 等待服务器就绪（最多 5 秒）─────────────────────────────────────────
echo "── waiting for server to be ready..."
for i in $(seq 1 16); do
    if curl -fs "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
        echo "   ready (attempt ${i})"
        break
    fi
    if [ "$i" -eq 16 ]; then
        echo "ERROR: server failed to start within 5s" >&2
        cat /tmp/airi_server.log >&2
        exit 1
    fi
    sleep 0.3
done

# ── 3. wrk 30s ─────────────────────────────────────────────────────────────
echo
echo "════════ [1/2] wrk 30s 压测  (4 线程 / 100 连接) ════════"
if ! command -v wrk >/dev/null 2>&1; then
    echo "ERROR: wrk 未安装。macOS: brew install wrk" >&2
    exit 1
fi
wrk -t4 -c100 -d30s --latency "http://127.0.0.1:${PORT}/"

# ── 4. RSS / 长连接规模测试 ─────────────────────────────────────────────────
echo
echo "════════ [2/2] RSS 长连接规模测试  (5000 conns) ════════"
"${ROOT_DIR}/build/tools/conn_scale_test" 127.0.0.1 "${PORT}" 5000 "${SERVER_PID}"

# ── 5. 打印服务器尾部日志 ───────────────────────────────────────────────────
echo
echo "── server stdout/stderr (last 20 lines) ──"
tail -n 20 /tmp/airi_server.log || true
