#!/usr/bin/env bash
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

case "${1:-}" in -h|--help) sed -n '2,/^$/p' "$0"; exit 0 ;; esac

main() {
  local logf="${1:?用法: _wait.sh <日志名> [间隔秒=15] [最大轮数=960]}"
  local every="${2:-15}" rounds="${3:-960}" i out head body chain size=0 code

  printf '[守望] %s:%s/%s —— 静默等待完成（仅触发回调；人要直播用 attach）\n' \
         "$HOST" "$DIR" "$logf"

  for ((i = 1; i <= rounds; i++)); do
    if out="$(_ssh_retry "cd '$DIR' 2>/dev/null || exit 0
          stat -c %s '$logf' 2>/dev/null || echo 0
          tail -c +$((size + 1)) '$logf' 2>/dev/null || true
          printf '\n===CHAIN===\n'
          [ -f chain.log ] && tail -n 1 chain.log || true
          true")"; then
      head="${out%%$'\n'*}"
      if [[ "$head" =~ ^[0-9]+$ ]]; then
        size="$head"; body="${out#*$'\n'}"
      else
        body="$out"
      fi
      chain="${body#*===CHAIN===}"; body="${body%%===CHAIN===*}"
      # ⚠️ 故意不打印 `body`（静默触发器语义）；`|| true` 必须留在管道**末尾** ——
      #    `set -o pipefail` 下首轮无退出码时 grep 返回 1 会触发 errexit 静默退出。
      code="$(printf '%s' "$body" | tr '\r' '\n' | grep -aoE 'COMMAND_EXIT_CODE="[0-9]+"' | tail -1 | tr -dc 0-9 || true)"
      if [ -n "$code" ]; then
        printf '[守望] ✅ 完成：COMMAND_EXIT_CODE=%s\n' "$code"
        return 0
      fi
      if printf '%s' "$chain" | grep -q '⛔'; then
        printf '[守望] ⛔ 接力放弃（chain.log）——停止\n'
        return 0
      fi
    fi
    sleep "$every"
  done

  printf '[守望] ⏰ 到轮数上限，退出（作业可能仍在跑）\n'
  return 0
}

main "$@"
