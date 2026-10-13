#!/usr/bin/env bash
set -euo pipefail

HOST="${STS_REMOTE_HOST:-root@connect.westc.seetacloud.com}"
PORT="${STS_REMOTE_PORT:-38825}"
DIR="${STS_REMOTE_DIR:-/root/autodl-tmp/sts_lightspeed}"

# 以仓库根为基准取受管清单 ⇒ 从任何 cwd 调用都对。
cd "$(dirname "${BASH_SOURCE[0]}")"


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

# 受管文件 = 根目录 .py/.sh/构建文件 + `.agent/**/*.md` + C++ 目录。
# 排除：子模块（json/、pybind11/）、_old/、data/、build/、realtest/。
managed() {
  find . -maxdepth 1 -type f \( -name '*.py' -o -name '*.sh' -o -name 'CMakeLists.txt' \
       -o -name 'pyproject.toml' \) | sed 's|^\./||'
  find .agent -type f -name '*.md'
  find bindings include src apps collect -type f \( -name '*.cpp' -o -name '*.h' \)
  find bindings/slaythespire_pkg -type f       # CMake 配置阶段要读的模板，漏推 = 配置失败
}

# 本地：路径 + md5
_fp_local() {
  local f
  while IFS= read -r f; do printf '%s %s\n' "$f" "$(md5sum "$f" | cut -d' ' -f1)"; done < <(managed | sort)
}

# 远端：同上。只哈希**存在**的文件 —— 缺失时 md5sum 的报错会跟"连不上"混淆。
_fp_remote() {
  local list
  # 换行→空格：该字符串整体作为一条远端命令提交，含换行会被拆成多条。
  list="$(managed | sort | tr '\n' ' ')"
  [[ -n "$list" ]] || return 0
  "${SSH[@]}" "cd '$DIR' 2>/dev/null || exit 3
    for f in $list; do [ -f \$f ] && md5sum \$f; done; true" | awk '{print $2, $1}' | sort
}

main() {
  # 有差异时 diff 退出码为 1 —— 那是本命令的**正常结果**，不是失败。
  diff <(_fp_local) <(_fp_remote) || true
  echo "[比对] 左侧 '<' 本地、右侧 '>' 远端；无输出 = 一致"
}

main "$@"
