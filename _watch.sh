#!/usr/bin/env bash
# 自迭代看门狗：`_chain.sh` 每次退出即在 30 秒后重启，覆盖脚本崩溃与轮间空隙。
#
#   _watch.sh <起点权重>
#
# 旋钮与 `_chain.sh` 共用同名环境变量（STAMP / N_PER / SIMS / EPOCHS / WORKERS / WINDOW /
# GATE_GAMES / GATE_MARGIN / TEMP_MOVES / DIR_TOTAL / DIR_EPS），由调用方启动前导出，本脚本不改写。
# 未导出 STAMP 时由本脚本生成一次，随后固定不变，保证每次重启写同一个日志目录。
#
# 产物：每轮 npz 与权重落在 data/，重启不丢；`_chain.sh` 的输出追加到 $LOGD/watchdog.log。
set -uo pipefail

NET="${1:?用法: _watch.sh <起点权重>}"

RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RDIR"

STAMP="${STAMP:-$(date +%m%d_%H%M)}"
LOGD="data/logs/chain_${STAMP}"
mkdir -p "$LOGD"

while :; do
    ./_chain.sh "$NET" "${ROUNDS:-0}" 2>&1 | tee -a "$LOGD/watchdog.log"
    rc="${PIPESTATUS[0]}"
    printf '[watchdog] %s _chain.sh 退出 rc=%s，30 秒后重启\n' "$(date +%T)" "$rc" \
        | tee -a "$LOGD/watchdog.log"
    sleep 30
done
