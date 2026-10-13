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

main() {
  # `-tt` 强制分配 tty（tmux 需要）；接不上时 BatchMode 让它直接失败，而不是等着要输入。
  exec ssh -tt "${SSH_OPTS[@]}" -p "$PORT" "$HOST" "tmux attach -t '$SESSION'"
}

main "$@"
