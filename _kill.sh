#!/usr/bin/env bash
set -euo pipefail

HOST="${STS_REMOTE_HOST:-root@connect.westc.seetacloud.com}"
PORT="${STS_REMOTE_PORT:-38825}"

# `:?` 对 unset 与空值都报错（`set -u` 只管 unset；空值会漏过去 ⇒ `tmux -t ''` 静默建空名会话）。
SESSION="${STS_SESSION:?必须显式指定 STS_SESSION}"

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

main() {
  # ⚠️ 只 `tmux kill-session` 不会杀作业：`script`/python 子孙成孤儿继续跑（曾致双作业抢核）。
  #    所以先递归 TERM 整棵**进程树**，再（可选地）杀会话。
  local pane
  pane="$("${SSH[@]}" "tmux list-panes -t '$SESSION' -F '#{pane_pid}' 2>/dev/null | head -1")" || true
  if [[ -z "$pane" ]]; then
    echo "[tmux] 没有会话 '$SESSION'"; return 1
  fi

  # 递归 TERM 整棵子孙树：pane → script → python → workers。
  "${SSH[@]}" "kt() { local p=\$1 c; for c in \$(pgrep -P \$p 2>/dev/null); do kt \$c; done; kill -TERM \$p 2>/dev/null; }; kt $pane; sleep 1" 2>/dev/null || true
  # 进程树倒下后窗格即结束，tmux **通常已自行收掉会话** ⇒ 这句失败是正常路径，不当错误。
  "${SSH[@]}" "tmux kill-session -t '$SESSION' 2>/dev/null" || true

  echo "[tmux] 已终止会话 '$SESSION'（含作业进程树）"
}

main "$@"
