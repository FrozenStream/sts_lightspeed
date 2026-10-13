#!/usr/bin/env bash
set -euo pipefail

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

# 需要监控的作业入口；`watch` 也算作业。⚠️ 采集器是 `collect_<名字>.py`（teacher/selfplay）。
_JOBS_RE='[p]ython .*(collect_[a-z]+|train|watch)\.py'

# 约束：① 并行作业全部列出（不得只报最新一个）；② 结束判据取退出码数值。
main() {
  "${SSH[@]}" "cd '$DIR' 2>/dev/null && {
echo '=== tmux 会话 ==='
tmux ls 2>/dev/null || echo '  （无）'
echo
echo '=== 窗格（dead=1 表示命令已退出；exit= 为 tmux 侧退出码） ==='
tmux list-panes -a -F '  #{session_name}  dead=#{pane_dead}  exit=#{pane_dead_status}  #{pane_current_command}' 2>/dev/null | head -12
echo
echo '=== 作业进程（etimes 秒 / %CPU） ==='
ps -eo pid,etimes,pcpu,args --sort=-etimes | grep -E '$_JOBS_RE' | grep -vE 'bash -c|script -q' | cut -c1-130 | head -12
echo
echo '=== 最近 5 个日志：静默 / 退出码 / 末行 ==='
for f in \$(ls -t -- *.log 2>/dev/null | head -5); do
  age=\$(( \$(date +%s) - \$(stat -c %Y -- \"\$f\") ))
  code=\$(grep -aoE 'COMMAND_EXIT_CODE=\"[0-9]+\"' \"\$f\" 2>/dev/null | tail -1 | tr -dc 0-9)
  if [ -n \"\$code\" ]; then st=\"退出码 \$code\"; else st='无退出码'; fi
  echo \"  \$f  静默 \${age}s  \$st  \$(tr '\r' '\n' < \"\$f\" | grep -vE '^[[:space:]]*\$' | tail -1 | cut -c1-80)\"
done; }"
}

main "$@"
