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
PORT=22132
DIR=/root/autodl-tmp/sts_lightspeed

LOCAL_ONLY="STS_BGLOG STS_WORKERS STS_SESSION"        # 本地专用，不转发

# BatchMode 禁止所有需要用户交互的提示；一旦 SSH 需要你输入直接失败退出。
# ConnectTimeout 建立 TCP连接的超时时间
# ServerAliveInterval 连接建立后保活消息发送间隔
# ServerAliveCountMax 如果连续x次保活消息都没收到服务器回应，就认为连接已断。
_SETT=(-o BatchMode=yes
        -o ConnectTimeout=10 
        -o ServerAliveInterval=15
        -o ServerAliveCountMax=3
        -o TCPKeepAlive=yes)
SSH=(ssh "${_SETT[@]}" -p "$PORT" "$HOST")


# 受管文件 = 根目录 .py/.sh/构建文件 + `.agent/**/*.md` + C++ 目录。
# 排除：子模块（json/、pybind11/）、_old/、data/、build/、realtest/。
managed() {
  find . -maxdepth 1 -type f \( -name '*.py' -o -name '*.sh' -o -name 'CMakeLists.txt' \
       -o -name 'pyproject.toml' \) | sed 's|^\./||'
  find .agent -type f -name '*.md'
  find bindings include src apps -type f \( -name '*.cpp' -o -name '*.h' \)
}

local list=()

if (($#)); then list=("$@"); else mapfile -t list < <(managed); fi

printf '%s\n' "${list[@]}" | tar czf - -T -  | "${SSH[@]}" "mkdir -p '$DIR' && tar xzf - -C '$DIR'"
echo "[推送] ${#list[@]} 个文件 → $HOST:$DIR"

