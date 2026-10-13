#!/usr/bin/env bash
# 本地 → 远端（seetacloud）同步 + 作业控制。
#
#   ./sync.sh                  比较两端 md5（默认）
#   ./sync.sh push [文件...]   推送受管文件（默认全部；规则见 managed()）
#   ./sync.sh start <脚本.py> [参数...]   在远端 tmux 会话中启动
#   ./sync.sh attach | kill               接入 / 终止会话
#   ./sync.sh status           会话、窗格、作业进程、最近 5 个日志
#   ./sync.sh log [n]          最新日志尾部（默认 40 行）
#   ./sync.sh wait <日志名> [间隔秒] [轮数]    **静默触发器**：只等完成标记，退出码/⛔ 即退

set -euo pipefail

# 远端实例身份：换机器只改这一段。
HOST=root@connect.westc.seetacloud.com
PORT=38825
DIR=/root/autodl-tmp/sts_lightspeed
PY=/root/miniconda3/bin/python     
# 远端作业的 worker 数（`runtime.workers()` 读 `STS_WORKERS`，见 `runtime.py`）。
# **空 = 按远端 cgroup 配额自动**：本机的物理核数不该带到大机器上。
WORKERS=16
SESSION="${STS_SESSION:-}"
LOG="$DIR/run.log"
BLOG="$DIR/run.bg.log"

LOCAL_ONLY="STS_BGLOG STS_WORKERS STS_SESSION"        # 本地专用，不转发

# 会话名**无默认值**（2026-09-24 起）：attach / kill / start 缺 `STS_SESSION` 即响亮失败。
# ⚠️ 别给默认值 `''`：`tmux -s ''` 会**成功**建一个空名会话，然后第二次启动报
#    `duplicate session:`（冒号后是空的，看不出是谁），kill/attach 又都指不到 ⇒ 处处静默。
_need_session() {
  [[ -n "$SESSION" ]] && return 0
  echo "❌ 必须显式指定 STS_SESSION（attach/kill/start 已取消默认会话名）" >&2
  return 2
}

# ⚠️ **本脚本必须用 Git Bash 跑**（`C:\Program Files\Git\bin\bash.exe`），不要用 WSL 的 bash：
#    `AppData\...\WindowsApps\bash.exe` 是 WSL 的入口 ⇒ 那台 Linux 虚拟机有**它自己的** `$HOME`
#    （`/home/<user>`）与自己的 `ssh`/密钥 ⇒ `Permission denied (publickey)`，而同一台机器上
#    Windows 的 `ssh.exe`（终端里那个）却是通的。同一份脚本、两个"本地"，症状极像远端挂了。
# ⚠️ 私钥路径**显式给**，不靠 `$HOME`（Git Bash 的 `$HOME` = `C:\Users\<你>`，WSL 的不是）：
#    换机器只改 `STS_SSH_KEY` 即可，不会出现"终端能连、脚本不能连"。
KEY="${STS_SSH_KEY:-$HOME/.ssh/id_rsa}"
if [ ! -f "$KEY" ] && [ -f "/c/Users/${USER:-$(whoami)}/.ssh/id_rsa" ]; then
  KEY="/c/Users/${USER:-$(whoami)}/.ssh/id_rsa"
fi
[ -f "$KEY" ] || echo "sync.sh: ⚠️ 找不到私钥 '$KEY'（可用 STS_SSH_KEY 指定）" >&2

# BatchMode 禁止所有需要用户交互的提示；一旦 SSH 需要你输入直接失败退出。
# ConnectTimeout 建立 TCP连接的超时时间
# ServerAliveInterval 连接建立后保活消息发送间隔
# ServerAliveCountMax 如果连续x次保活消息都没收到服务器回应，就认为连接已断。
_SETT=(-o BatchMode=yes
        -o ConnectTimeout=10 
        -o ServerAliveInterval=15
        -o ServerAliveCountMax=3
        -o TCPKeepAlive=yes)
SSH=(ssh "${_SETT[@]}" -i "$KEY" -p "$PORT" "$HOST")

# 读路径统一重试；写路径（push）不重试（幂等性由调用方保证）。
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

# 生成待转发的 STS_* 赋值片段，可直接拼进远端命令行。
_fwd() {
  local n o=""
  while IFS= read -r n; do
    case " $LOCAL_ONLY " in *" $n "*) continue ;; esac
    o+=" $n=$(printf '%q' "${!n}")"
  done < <(compgen -v STS_)
  printf '%s' "$o"
}

# 受管文件 = 根目录 .py/.sh/构建文件 + `.agent/**/*.md` + C++ 目录。
# 排除：子模块（json/、pybind11/）、_old/、data/、build/、realtest/。
managed() {
  find . -maxdepth 1 -type f \( -name '*.py' -o -name '*.sh' -o -name 'CMakeLists.txt' \
       -o -name 'pyproject.toml' \) | sed 's|^\./||'
  find .agent -type f -name '*.md'
  find bindings include src apps collect -type f \( -name '*.cpp' -o -name '*.h' \)
  # ⚠️ 包的源码不是 .cpp/.h（`__init__.py` / `_slaythespire.pyi` 的模板）⇒ 上面那条抓不到；
  #    而 CMake 的 `file(GENERATE ... INPUT ...)` **配置阶段就要读它们** ⇒ 漏推 = 配置阶段失败
  #    （"Evaluation file ... cannot be read"，全新实例必踩）。
  find bindings/slaythespire_pkg -type f
}

# --- check / push -------------------------------------------------------------
_fp_local() {
  local f
  while IFS= read -r f; do printf '%s %s\n' "$f" "$(md5sum "$f" | cut -d' ' -f1)"; done < <(managed | sort)
}

_fp_remote() {
  local list
  # 换行→空格：该字符串整体作为一条远端命令提交，含换行会被拆成多条。
  list="$(managed | sort | tr '\n' ' ')"
  [[ -n "$list" ]] || return 0
  # 仅哈希远端存在的文件：缺失时 md5sum 报错会与"连不上"混淆。
  "${SSH[@]}" "cd '$DIR' 2>/dev/null || exit 3
    for f in $list; do [ -f \$f ] && md5sum \$f; done; true" | awk '{print $2, $1}' | sort
}


cmd_push() {
  local list=()
  if (($#)); then list=("$@"); else mapfile -t list < <(managed); fi
  ((${#list[@]})) || { echo "[推送] 没有可推送的文件" >&2; return 1; }
  printf '%s\n' "${list[@]}" | tar czf - -T - \
    | "${SSH[@]}" "mkdir -p '$DIR' && tar xzf - -C '$DIR'"
  echo "[推送] ${#list[@]} 个文件 → $HOST:$DIR"
}


cmd_attach() {
  _need_session || return $?
  exec ssh -tt -o BatchMode=yes "${_SETT[@]}" -p "$PORT" "$HOST" "tmux attach -t '$SESSION'"
}
cmd_kill() {
  _need_session || return $?
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

cmd_start() {
  _need_session || return $?
  local script runner
  # 脚本必填（防「没写目标就起训练」）；校验放在 `cmd_push` 之前 —— 参数不合法时不推送。
  if [[ "${1:-}" == *.py || "${1:-}" == *.sh ]]; then script="$1"; shift
  else
    echo "[tmux] ❌ start 必须显式给出脚本（.py/.sh；默认 train.py 已取消）" >&2
    return 2
  fi
  cmd_push || return $?
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
    || { echo "[tmux] ⚠️ 启动失败：会话 '$SESSION' 已存在？（先 ./sync.sh kill，或换 STS_SESSION）" >&2; return 1; }
  echo "[tmux] 已在远端会话 '$SESSION' 里启动：$script $*"
  printf '%s\t%s\t%s\t%s\n' "$(date +%F_%T)" "$SESSION" "$script $*" "$BLOG" >> "$0.sessions"
  echo "        看进度：./sync.sh status   或   STS_SESSION='$SESSION' ./sync.sh attach"
}

# --- 看状态 -------------------------------------------------------------------
# 返回最新 `run*.log` 的绝对路径。
_newest_log() {
  _ssh_retry "cd '$DIR' 2>/dev/null || exit 0
for f in run.log run.bg.log run.*.log; do [ -f \"\$f\" ] && stat -c '%Y $DIR/%n' \"\$f\"; done \
    | sort -rn | head -1 | cut -d' ' -f2-"
}

cmd_log() {
  local n="${1:-40}" lf
  lf="$(_newest_log)"
  [[ -n "$lf" ]] || { echo "[作业] 还没有任何日志（$DIR/run*.log 都不存在）"; return 0; }
  echo "[作业] 日志 = $lf"
  # 先 CR→LF 再 tail：进度条以 \r 原地刷新，直接读会把多次刷新粘成一行。
  "${SSH[@]}" "tail -n '$n' '$lf'" | tr '\r' '\n'
}

# 需要监控的作业入口；`watch` 也算作业。⚠️ 采集器是 `collect_<名字>.py`（teacher/selfplay）。
_JOBS_RE='[p]ython .*(collect_[a-z]+|train|watch)\.py'

# --- 完成触发器（**静默**：只等完成标记，不转发任何内容）-----------------------
# ./sync.sh wait <日志名> [间隔秒=15] [最大轮数=960]
#   每轮新建 ssh 拉**上次之后的新字节**，只找标记不回显；读到 `COMMAND_EXIT_CODE`
#   或 `chain.log` 的「⛔」即退出（退出 = 完成回调，触发本地通知）。
#   人要看直播：`STS_SESSION=chain ./sync.sh attach`（pane 即任务本尊），或 log / status。
cmd_wait() {
  local logf="${1:?用法: wait <日志名> [间隔秒=15] [最大轮数=960]}"
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

# 输出：tmux 会话 / 窗格 / 作业进程 / 最近 5 个日志。
# 约束：① 并行作业全部列出（不得只报最新一个）；② 结束判据取退出码数值。
cmd_status() {
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

case "${1:-}" in
  push)   shift; cmd_push "$@" ;;
  start)  shift; cmd_start "$@" ;;
  attach) cmd_attach ;;
  kill)   cmd_kill ;;
  wait)   shift; cmd_wait "$@" ;;
  log)    shift; cmd_log "$@" ;;
  status) shift; cmd_status ;;
  usage|help|-h|--help) sed -n '2,/^$/p' "$0" ;;
  # 无参数 = 比较两端 md5（文档头第 4 行的默认行为；以前少了这一支 ⇒ 裸跑 `$1` unbound）。
  '')
    diff <(_fp_local) <(_fp_remote) || true
    echo "[比对] 左侧 '<' 本地、右侧 '>' 远端；无输出 = 一致" ;;
  *)      echo "未知子命令: $1" >&2; sed -n '2,/^$/p' "$0"; exit 2 ;;
esac
