#!/usr/bin/env bash
# 重采一批**带 `a_act`** 的自对弈数据：策略梯度要用「实际执行的动作」，而 `data/` 里的旧
# 自对弈缓存在 `step < temp_moves` 时按 π 采样 ⇒ 只知道分布、不知道抽中哪个，无法还原。
#
# 配方与 `_chain.sh` 的**实际运行值**一致（it1..it17 缓存名 = `inferserver_server_devicecuda`；
# ⚠️ `_chain.sh` 里的常量 `INFER=eager` 是过时的，别抄常量、抄缓存名）。
# `--workers` 只影响速度，不进入缓存名（并行与串行逐位相同）。
#
# 用法：
#     _collect_probe.sh [N_PER] [SIMS] [输出名]
#     默认 = 64 3000 pgprobe，网取 data/v3-train_17-*.pt
set -euo pipefail
cd "$(dirname "$0")"
PY=/root/miniconda3/bin/python

N_PER="${1:-64}"
SIMS="${2:-3000}"
NAME="${3:-pgprobe}"
NET="${4:-$(ls -1 data/v3-train_17-*.pt | head -1)}"
LOG="data/logs/collect_${NAME}.log"

echo "网  = $NET"
echo "配  = N_PER=$N_PER SIMS=$SIMS games=16 temp_moves=30 dir=10.83/0.1"
echo "日志 = $LOG"
date +%T; date +%s

"$PY" -u collect_selfplay.py --n "$N_PER" --sims "$SIMS" --net-path "$NET" \
    --workers 16 --cache-dir data --name "$NAME" \
    --games 16 \
    --temp-moves 30 --dirichlet-total 10.83 --dirichlet-eps 0.1 \
    >"$LOG" 2>&1

date +%s; date +%T
grep -oE '已缓存到 [^ ]+' "$LOG" | tail -1
