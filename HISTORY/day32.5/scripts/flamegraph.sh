#!/usr/bin/env bash
# flamegraph.sh
#
# 服务器 CPU 火焰图采样脚本。
#   macOS  : 使用系统自带 `sample` 工具（无需 root），输出文本调用栈
#            + 可选 stackcollapse-sample.pl + flamegraph.pl 渲染 SVG
#   Linux  : 使用 perf record / perf script + FlameGraph
#
# 依赖（可选，仅渲染 SVG 时需要）：
#   git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph
#   export FLAMEGRAPH_DIR=~/FlameGraph
#
# 用法：
#   ./scripts/flamegraph.sh                # 自动启动服务器、wrk 加压 15s、采样 10s
#   ./scripts/flamegraph.sh --pid 12345    # 对已运行的进程采样

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${MYCPPSERVER_BIND_PORT:-18888}"
OUT_DIR="${ROOT_DIR}/benchmark/flamegraph"
mkdir -p "${OUT_DIR}"

ATTACH_PID=""
if [[ "${1:-}" == "--pid" ]]; then
    ATTACH_PID="${2:-}"
fi

SERVER_PID=""
WRK_PID=""
cleanup() {
    [[ -n "${WRK_PID}" ]] && kill "${WRK_PID}" 2>/dev/null || true
    [[ -n "${SERVER_PID}" ]] && kill "${SERVER_PID}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

if [[ -z "${ATTACH_PID}" ]]; then
    echo "── starting server on 127.0.0.1:${PORT} ──"
    MYCPPSERVER_BIND_PORT="${PORT}" "${ROOT_DIR}/build/examples/http_server" > /tmp/airi_flame.log 2>&1 &
    SERVER_PID=$!
    READY=0
    for i in $(seq 1 16); do
        if curl -fs "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then READY=1; break; fi
        sleep 0.3
    done
    if [[ "${READY}" -ne 1 ]]; then
        echo "ERROR: server failed to start on port ${PORT}" >&2
        cat /tmp/airi_flame.log >&2 || true
        exit 1
    fi
    ATTACH_PID="${SERVER_PID}"

    echo "── starting wrk load (30s) ──"
    if command -v wrk >/dev/null 2>&1; then
        wrk -t4 -c100 -d30s "http://127.0.0.1:${PORT}/" > /tmp/airi_flame_wrk.log 2>&1 &
        WRK_PID=$!
        # 让 wrk 走过启动拖尾，再开始采样
        sleep 2
    else
        echo "WARN: wrk 未安装，将无 CPU 负载，火焰图可能为空" >&2
    fi
fi

UNAME="$(uname -s)"
TS="$(date +%Y%m%d_%H%M%S)"

if [[ "${UNAME}" == "Darwin" ]]; then
    STACK_TXT="${OUT_DIR}/sample_${TS}.txt"
    SAMPLE_SECONDS="${SAMPLE_SECONDS:-25}"
    echo "── macOS sample: ${SAMPLE_SECONDS}s @ pid=${ATTACH_PID} → ${STACK_TXT}"
    sample "${ATTACH_PID}" "${SAMPLE_SECONDS}" -file "${STACK_TXT}" >/dev/null
    echo "采样文本已保存：${STACK_TXT}"

    if [[ -n "${FLAMEGRAPH_DIR:-}" && -x "${FLAMEGRAPH_DIR}/stackcollapse-sample.awk" ]]; then
        SVG="${OUT_DIR}/flame_${TS}.svg"
        awk -f "${FLAMEGRAPH_DIR}/stackcollapse-sample.awk" "${STACK_TXT}" \
            | "${FLAMEGRAPH_DIR}/flamegraph.pl" --title "Airi-Cpp-Server-Lib" > "${SVG}"
        echo "火焰图 SVG：${SVG}"
    else
        echo "提示：设置 FLAMEGRAPH_DIR=~/FlameGraph 可自动渲染 SVG。"
    fi
elif [[ "${UNAME}" == "Linux" ]]; then
    if ! command -v perf >/dev/null 2>&1; then
        echo "ERROR: perf 未安装。Ubuntu: apt install linux-tools-common linux-tools-$(uname -r)" >&2
        exit 1
    fi
    PERF_DATA="${OUT_DIR}/perf_${TS}.data"
    echo "── perf record: 10s @ pid=${ATTACH_PID} → ${PERF_DATA}"
    perf record -F 99 -p "${ATTACH_PID}" -g --call-graph dwarf -o "${PERF_DATA}" -- sleep 10
    PERF_TXT="${OUT_DIR}/perf_${TS}.folded"
    perf script -i "${PERF_DATA}" > "${PERF_TXT}.raw"
    if [[ -n "${FLAMEGRAPH_DIR:-}" ]]; then
        "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" "${PERF_TXT}.raw" > "${PERF_TXT}"
        SVG="${OUT_DIR}/flame_${TS}.svg"
        "${FLAMEGRAPH_DIR}/flamegraph.pl" --title "Airi-Cpp-Server-Lib" "${PERF_TXT}" > "${SVG}"
        echo "火焰图 SVG：${SVG}"
    else
        echo "原始 perf script 已保存：${PERF_TXT}.raw （需要 FlameGraph 才能渲染 SVG）"
    fi
else
    echo "ERROR: 不支持的操作系统 ${UNAME}" >&2
    exit 1
fi
