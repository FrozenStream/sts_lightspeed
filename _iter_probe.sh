#!/usr/bin/env bash
# **第 2 个 pass 探针**：判断固定数据上的 +0.93pp 是「一次性追赶」还是「可累积」。
#
# 背景：`ctrl_big`（固定 876k 行）从 `v3-train_4` 训 2 epoch = **+0.93pp**；链条每轮同样配方
# 却 16 轮零漂移（⑦-补7）。结构差异 = 固定数据 vs 自对弈滚动窗口。
# 若那 +0.93pp 是「重新贴合祖先数据（warm_v3 就是 v3-init 的训练集）」⇒ 再训一次应≈0；
# 若可累积 ⇒ 再来一个 +0.9pp。
#
# 用法：`_iter_probe.sh [起点权重] [lr=3e-4]`     门控场数用 env `GATE_N`（默认 512）
set -euo pipefail
cd "$(dirname "$0")"
PY=/root/miniconda3/bin/python
DATA="data/logs/ctrl_big.npz"
REF="$(ls -1 data/v3-train_4-e2-*.pt | head -1)"                              # 门控基准（起点）
P1="$(ls -1 data/ctrl_big-e2-lr0.0003-*pi_weightadvantage-loss_chunk*.pt | head -1)"  # 第 1 pass
INIT="${1:-$P1}"
LR="${2:-3e-4}"
GATE_N="${GATE_N:-512}"
OUT="data/logs/iterprobe_$(date +%m%d_%H%M)"
mkdir -p "$OUT"
echo "数据 = $DATA"; echo "基准 = ${REF##*/}"; echo "第1 pass = ${P1##*/}"
echo "第2 pass 起点 = ${INIT##*/}  lr=$LR  门控 $GATE_N 场/项  输出 = $OUT"
date +%T

LOG="$OUT/train_pass2.log"
"$PY" -u train.py train "$DATA" 2 \
    --lr "$LR" --wd 0.0001 --batch 256 --z-spec hp --soft-target --soft-temp 1.0 \
    --pi-weight advantage --pi-objective off --loss-chunk 16384 \
    --device cuda --threads 16 --record 0 --init "$INIT" >"$LOG" 2>&1
NEW="$("$PY" -c 'import sys;print([l.split()[1] for l in open(sys.argv[1],encoding="utf-8") if "已保存" in l][-1])' "$LOG")"
grep -aE "权重漂移|最终 train" "$LOG" | tail -2
echo "第2 pass = ${NEW##*/}"
date +%T

echo "===== 门控（$GATE_N 场/项 × 3 策略，裸网络）====="
"$PY" -u eval_encs.py "$GATE_N" "net:$REF,net:$P1,net:$NEW" 100000000 "$OUT/gate.json" \
    --workers 32 >"$OUT/gate.log" 2>&1
tail -3 "$OUT/gate.log"
date +%T

# 标签必须**各自唯一**：第 1/第 2 pass 的名字只差 `pi_objectiveoff` 与 `pi_weightadvantage-`
# 这两段 ⇒ 用 `pi_weightadvantage-loss_chunk`（仅第 1 pass 有）与 `pi_objectiveoff`（仅第 2 pass 有）。
echo "===== 配对（_pair_arms.py；不再内联重写）====="
"$PY" _pair_arms.py "$OUT/gate.json" "v3-train_4" \
    "pi_weightadvantage-loss_chunk" "pi_objectiveoff" | tee "$OUT/pair.txt"
echo "结果目录 = $OUT"
