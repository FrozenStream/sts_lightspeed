#!/usr/bin/env bash
# 本地 → 远端推送：把受管文件打包发到远端目录。
#
#   ./_push.sh             推送全部受管文件（清单规则见下面的 managed()）
#   ./_push.sh <文件...>   只推送指定的这几个
set -euo pipefail

HOST="${STS_REMOTE_HOST:-root@connect.westc.seetacloud.com}"
PORT="${STS_REMOTE_PORT:-38825}"
DIR="${STS_REMOTE_DIR:-/root/autodl-tmp/sts_lightspeed}"

cd "$(dirname "${BASH_SOURCE[0]}")"

# BatchMode       需要交互（输密码、确认主机指纹）时直接失败，而不是挂在那里等
# ConnectTimeout  建连超时（秒）
# ServerAlive*    连接建立后的保活：连续 3 次探测无回应即判定已断，不再傻等
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
  # ⚠️ 包的源码不是 .cpp/.h（`__init__.py` / `_slaythespire.pyi` 的模板）⇒ 上面那条抓不到；
  #    而 CMake 的 `file(GENERATE ... INPUT ...)` **配置阶段就要读它们** ⇒ 漏推 = 配置阶段失败
  #    （"Evaluation file ... cannot be read"，全新实例必踩）。
  find bindings/slaythespire_pkg -type f
}

case "${1:-}" in -h|--help) sed -n '2,/^$/p' "$0"; exit 0 ;; esac

# 打包 → 一次 ssh 传到远端解包（比逐文件 scp 快，且不依赖远端有 rsync）。
main() {
  local -a files

  if (($# > 0)); then
    files=("$@")                      # 命令行给了文件：只推这些
  else
    mapfile -t files < <(managed)     # 否则推全部受管文件
  fi

  ((${#files[@]} > 0)) || { echo "[推送] 没有可推送的文件" >&2; return 1; }

  printf '%s\n' "${files[@]}" | tar czf - -T - \
    | "${SSH[@]}" "mkdir -p '$DIR' && tar xzf - -C '$DIR'"

  echo "[推送] ${#files[@]} 个文件 → $HOST:$DIR"
}

main "$@"
