#!/usr/bin/env bash
# chain.sh —— 接力守望：等某个作业日志出现 `COMMAND_EXIT_CODE=0` 后，自动启动下一棒。
#
#   ./chain.sh <等谁完成的日志> <py 脚本> [args...]
#   例：./chain.sh job-train.log train.py train data/encdata_n100_s500.npz 8 data/x.pt
#
# 契约：
#   ① 完成判据 = 日志尾部 `COMMAND_EXIT_CODE` 的**数值**（137 = 外部 SIGKILL ≠ 完成）；
#   ② 前一棒**非 0**（或等待超时，默认 4h，`STS_CHAIN_MAX_ROUNDS`×30s 可调）⇒ 放弃接力并记录 —— 绝不带着坏缓存往下跑；
#   ③ 下一棒日志 = `job-chain-<脚本名>.log`（script 包装、尾部落退出码）；守望日志 = `chain.log`；
#   ④ 独立 tmux 会话承载（建议 `STS_SESSION=chain`）；取消 = `STS_SESSION=chain ./sync.sh kill`。
set -u
WLOG="${1:?用法: chain.sh <等谁完成的日志> <py脚本> [args...]}"; shift
(($#)) || { echo "[chain] 缺下一棒命令（py 脚本）" >&2; exit 2; }
cd "$(dirname "$0")"
PY=/root/miniconda3/bin/python
BLOG="job-chain-$(basename "$1" .py).log"
echo "[chain] 守望启动 $(date '+%F %T')：等 $WLOG ⇒ 然后 $PY -u $*（下一棒日志 $BLOG）" | tee -a chain.log
# ⚠️ 陈旧日志防护：WLOG 可能是**上一条同名链**留下的、已带 COMMAND_EXIT_CODE=0 的旧文件
# （下一棒会用 `script` 截断重写它）⇒ 只认守望开始**之后**被重写的日志（mtime 快照）。
MT0=$(stat -c %Y "$WLOG" 2>/dev/null || echo 0)
# 最长等 $STS_CHAIN_MAX_ROUNDS×30s（默认 480 = 4h）：作业卡死时不无限等。
for _ in $(seq 1 "${STS_CHAIN_MAX_ROUNDS:-480}"); do
  MT=$(stat -c %Y "$WLOG" 2>/dev/null || echo 0)
  if [ "$MT" -gt "$MT0" ] && grep -aq 'COMMAND_EXIT_CODE' "$WLOG" 2>/dev/null; then break; fi
  sleep 30
done
CODE=$(grep -aoE 'COMMAND_EXIT_CODE="[0-9]+"' "$WLOG" 2>/dev/null | tail -1 | tr -dc 0-9)
if [[ "${CODE:-x}" != 0 ]]; then
  echo "[chain] ⛔ $WLOG 未正常完成（code=${CODE:-无/等待超时}）⇒ 放弃接力 $(date '+%F %T')" | tee -a chain.log
  exit 1
fi
echo "[chain] ✅ $WLOG 完成（exit=0）⇒ 开下一棒：$*  $(date '+%F %T')" | tee -a chain.log
STS_WORKERS="${STS_WORKERS:-16}" STS_BGLOG="$BLOG" script -q -e -f -c "$PY -u $*" "$BLOG"
echo "[chain] 下一棒命令已退出（退出码见 $BLOG 尾部）$(date '+%F %T')" | tee -a chain.log
