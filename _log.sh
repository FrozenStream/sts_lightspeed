#!/usr/bin/env bash
# 看最新日志的尾部。
#
#   ./_log.sh [行数=40]
#
# 自足脚本：身份 / SSH 都在本文件里，不 source 任何公共库。

set -euo pipefail

# --- 远端实例身份：换机器只改这一段（或用 STS_REMOTE_HOST/PORT/DIR 覆盖）------
HOST="${STS_REMOTE_HOST:-root@connect.westc.seetacloud.com}"
PORT="${STS_REMOTE_PORT:-38825}"
DIR="${STS_REMOTE_DIR:-/root/autodl-tmp/sts_lightspeed}"



# BatchMode       需要交互时直接失败，而不是挂在那里等
# ConnectTimeout  建连超时（秒）
# ServerAlive*    保活：连续 3 次探测无回应即判定已断
SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=10
  -o ServerAliveInterval=15
  -o ServerAliveCountMax=3
  -o TCPKeepAlive=yes
)
SSH=(ssh "${SSH_OPTS[@]}" -p "$PORT" "$HOST")

# 读路径统一重试（写路径 push 不重试：幂等性由调用方保证）。
_ssh_retry() {
  local n=3 i rc out
  for ((i = 1; i <= n; i++)); do
    if out="$("${SSH[@]}" "$@" 2>/dev/null)"; then printf '%s' "$out"; return 0
    else rc=$?; echo "ssh_retry: failed $i/$n :? $rc" >&2
    fi
    ((i < n)) && sleep $((i * 2))
  done
  return $rc
}

# 返回最新 `run*.log` 的绝对路径。
_newest_log() {
  _ssh_retry "cd '$DIR' 2>/dev/null || exit 0
for f in run.log run.bg.log run.*.log; do [ -f \"\$f\" ] && stat -c '%Y $DIR/%n' \"\$f\"; done \
    | sort -rn | head -1 | cut -d' ' -f2-"
}

case "${1:-}" in -h|--help) sed -n '2,/^$/p' "$0"; exit 0 ;; esac

main() {
  local n="${1:-40}" lf
  lf="$(_newest_log)"
  [[ -n "$lf" ]] || { echo "[作业] 还没有任何日志（$DIR/run*.log 都不存在）"; return 0; }
  echo "[作业] 日志 = $lf"
  # 先 CR→LF 再 tail：进度条以 \r 原地刷新，直接读会把多次刷新粘成一行。
  "${SSH[@]}" "tail -n '$n' '$lf'" | tr '\r' '\n'
}

main "$@"
