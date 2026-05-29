#!/usr/bin/env bash
# 用 macOS 自带的 xctrace 录制 .trace 文件，可直接用 Instruments.app 打开。
# 默认使用 Time Profiler 模板（CPU 火焰图视角）。
#
# ⚠️ xctrace attach 模式必须用 sudo 设置 kperf 采样优先级，
#    本脚本只对 `xctrace record` 一行调用 sudo，server/wrk 仍以当前用户身份运行
#    （避免 .trace / build 产物被写成 root 所有）。运行时会提示输入一次密码。
#
# 用法：
#   bash scripts/instruments_profile.sh                 # 30s wrk + 30s 录制 (Time Profiler)
#   TEMPLATE='System Trace' bash scripts/instruments_profile.sh
#   TEMPLATE=Allocations    bash scripts/instruments_profile.sh
#   DURATION=60 WRK_THREADS=8 WRK_CONNECTIONS=200 bash scripts/instruments_profile.sh
#
# 产物：
#   benchmark/instruments/airi_<template>_<时间戳>.trace      ← 双击即可在 Instruments 打开
#
set -euo pipefail

# ── 提前请求 sudo（一次性输入密码，后续 sudo 在 timeout 内免密）──
if ! sudo -v; then
  echo "✗ 需要 sudo 权限（xctrace attach 必须 root 才能设置 kperf 采样优先级）" >&2
  exit 1
fi
# 后台保活，避免 30s 录制中途 sudo timeout
( while true; do sudo -n true; sleep 30; done ) &
SUDO_KEEPALIVE_PID=$!

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TEMPLATE="${TEMPLATE:-Time Profiler}"
DURATION="${DURATION:-30}"          # 录制秒数
WRK_THREADS="${WRK_THREADS:-4}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-100}"
PORT="${MYCPPSERVER_BIND_PORT:-18888}"
URL_PATH="${URL_PATH:-/}"

OUT_DIR="$ROOT/benchmark/instruments"
mkdir -p "$OUT_DIR"

TS="$(date +%Y%m%d_%H%M%S)"
SAFE_TPL="$(echo "$TEMPLATE" | tr ' ' '_')"
TRACE_OUT="$OUT_DIR/airi_${SAFE_TPL}_${TS}.trace"
WRK_LOG="/tmp/airi_inst_wrk.log"

command -v xctrace >/dev/null || { echo "✗ xctrace 不可用（需要 Xcode 或 Command Line Tools）" >&2; exit 1; }
command -v wrk     >/dev/null || { echo "✗ wrk 不可用（brew install wrk）" >&2; exit 1; }

SERVER_BIN="$ROOT/build/examples/http_server"
if [[ ! -x "$SERVER_BIN" ]]; then
  echo "── 构建 http_server ──"
  cmake --build build --target http_server -j >/dev/null
fi

cleanup() {
  [[ -n "${SUDO_KEEPALIVE_PID:-}" ]] && kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true
  [[ -n "${SERVER_PID:-}"        ]] && kill "$SERVER_PID"        2>/dev/null || true
  [[ -n "${WRK_PID:-}"           ]] && kill "$WRK_PID"           2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ── 1. 拉起服务器（强制 export 端口给子进程）──
echo "── 启动 http_server (port=$PORT) ──"
MYCPPSERVER_BIND_PORT="$PORT" "$SERVER_BIN" >/tmp/airi_inst_server.log 2>&1 &
SERVER_PID=$!

# health check
READY=0
for _ in $(seq 1 20); do
  if curl -fs -o /dev/null "http://127.0.0.1:${PORT}/" 2>/dev/null; then
    READY=1; break
  fi
  sleep 0.2
done
[[ "$READY" -eq 1 ]] || { echo "✗ 服务器未在 4s 内就绪，见 /tmp/airi_inst_server.log" >&2; exit 1; }
echo "✓ 服务器就绪 (pid=$SERVER_PID)"

# ── 2. 拉起 wrk 持续加压（比录制窗口长 5s，确保覆盖整段录制）──
WRK_DURATION=$((DURATION + 5))
echo "── 启动 wrk: ${WRK_THREADS}t/${WRK_CONNECTIONS}c/${WRK_DURATION}s → ${URL_PATH} ──"
wrk -t"$WRK_THREADS" -c"$WRK_CONNECTIONS" -d"${WRK_DURATION}s" \
    "http://127.0.0.1:${PORT}${URL_PATH}" >"$WRK_LOG" 2>&1 &
WRK_PID=$!

# 等 wrk 真正进入稳态
sleep 2

# ── 3. xctrace 录制 ──
echo "── xctrace record: template='$TEMPLATE', duration=${DURATION}s ──"
echo "    输出：$TRACE_OUT"
sudo xctrace record \
  --template "$TEMPLATE" \
  --attach "$SERVER_PID" \
  --time-limit "${DURATION}s" \
  --output "$TRACE_OUT"

# 把 .trace 所有权还给当前用户（sudo 创建的目录默认属于 root）
sudo chown -R "$(id -u):$(id -g)" "$TRACE_OUT"

echo
echo "════════ 完成 ════════"
echo "Trace 文件：$TRACE_OUT"
echo
echo "wrk 摘要："
tail -n 12 "$WRK_LOG" | sed 's/^/    /'
echo
echo "在 Finder 中打开："
echo "    open '$TRACE_OUT'"
echo "或直接在 Instruments 中加载："
echo "    open -a Instruments '$TRACE_OUT'"
