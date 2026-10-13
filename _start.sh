#!/usr/bin/env bash
set -euo pipefail

# `-h` 必须排在 `SESSION` 那行之前：那行对 unset/空值当场报错 ⇒ 只有先处理 -h 才问得到用法。
case "${1:-}" in -h|--help) sed -n '2,/^$/p' "$0"; exit 0 ;; esac

HOST="${STS_REMOTE_HOST:-root@connect.westc.seetacloud.com}"
PORT="${STS_REMOTE_PORT:-38825}"
DIR="${STS_REMOTE_DIR:-/root/autodl-tmp/sts_lightspeed}"

RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # 会话记录写在这个目录里

# --- 远端作业默认值 ----------------------------------------------------------
PY=/root/miniconda3/bin/python
# 远端作业的 worker 数（`runtime.workers()` 读 `STS_WORKERS`，见 `runtime.py`）。
# **空 = 按远端 cgroup 配额自动**：本机的物理核数不该带到大机器上。
WORKERS=16
BLOG="$DIR/run.bg.log"                # tmux 里 `script -f` 逐写落盘的主日志
SESSIONS="$RDIR/sync.sh.sessions"     # 启动记录（沿用旧文件名：历史在里面）
# tmux 会话名**无默认值**：`STS_SESSION` 没给（或给空）就在下面这行当场失败。
# ⚠️ 别让它落成空串：`tmux -s ''` 会**成功**建一个空名会话，然后第二次启动报
#    `duplicate session:`（冒号后是空的，看不出是谁），kill/attach 又都指不到 ⇒ 处处静默。
#    `:?` 对 unset 与空值都报错（`set -u` 只管 unset，空值会漏过去）。
SESSION="${STS_SESSION:?必须显式指定 STS_SESSION}"
# 本地专用，不转发到远端作业。
LOCAL_ONLY="STS_BGLOG STS_WORKERS STS_SESSION STS_REMOTE_HOST STS_REMOTE_PORT STS_REMOTE_DIR"



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

# 生成待转发的 STS_* 赋值片段，可直接拼进远端命令行。
_fwd() {
  local n o=""
  while IFS= read -r n; do
    case " $LOCAL_ONLY " in *" $n "*) continue ;; esac
    o+=" $n=$(printf '%q' "${!n}")"
  done < <(compgen -v STS_)
  printf '%s' "$o"
}

main() {
  local script runner
  # 脚本必填（防「没写目标就起训练」）。
  if [[ "${1:-}" == *.py || "${1:-}" == *.sh ]]; then script="$1"; shift
  else
    echo "[tmux] ❌ start 必须显式给出脚本（.py/.sh；默认 train.py 已取消）" >&2
    return 2
  fi

  if [[ "$script" == *.sh ]]; then runner="bash"; else runner="$PY -u"; fi

  # ⚠️ 参数会穿过三层 shell（ssh → tmux → `script -c`）：含 `;` 会被当命令分隔符截断乱跑。
  case "$*" in
    *';'*) echo "[tmux] ⚠️ 参数里含 ';' —— 远端三层 shell 会把它当命令分隔符截断命令；" \
                  "请改用逗号等安全分隔符（或用环境变量传参）" >&2 ;;
  esac

  # 会话内套 `script -q -e -f`：`-f` 逐写落盘，`-e` 把退出码写入日志尾部。
  # `remain-on-exit on` 让窗格在命令退出后保留 ⇒ `#{pane_dead_status}` 可作退出码的第二证据。
  "${SSH[@]}" "cd '$DIR' && tmux new-session -d -s '$SESSION' \"bash -lc \\\"STS_WORKERS='$WORKERS' STS_BGLOG='$BLOG'$(_fwd) \
script -q -e -f -c '$runner $script $*' '$BLOG'\\\"\" && tmux set-option -t '$SESSION' remain-on-exit on && tmux set-option -t '$SESSION' mouse on" \
    || { echo "[tmux] ⚠️ 启动失败：会话 '$SESSION' 已存在？（先 ./_kill.sh，或换 STS_SESSION）" >&2; return 1; }

  echo "[tmux] 已在远端会话 '$SESSION' 里启动：$script $*"
  printf '%s\t%s\t%s\t%s\n' "$(date +%F_%T)" "$SESSION" "$script $*" "$BLOG" >> "$SESSIONS"
  echo "        看进度：./_status.sh   或   STS_SESSION='$SESSION' ./_attach.sh"
}

main "$@"
