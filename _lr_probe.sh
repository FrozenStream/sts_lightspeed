#!/usr/bin/env bash
# **全量数据上的 lr 探针**：链条每轮 876k 行、lr=3e-4 ⇒ 实测每轮均值漂移 0（⑦-补7）。
# 本脚本只换一个量：`--lr`（3e-5）。若 +1.00pp 在这个小步长上**保持或变大** ⇒ 方向是好的、
# 步长在过冲 ⇒ 链条该降 lr；若归零 ⇒ 那个 +1.00pp 是"步长本身"带来的。
#
# 三个 spec 一次门控（同 seed 配对）：起点 `v3-train_4` / 已有 `ctrl_big@3e-4`（+1.00pp 那支）/ 新臂。
# ⚠️ 已有 `ctrl_big@3e-4` 权重名里**没有** `pi_objective`（它训在我加这个参数之前）
#    ⇒ 查找不能靠正则匹配 `pi_objectiveoff`，要按 `pi_weightadvantage` 找。
#
# 用法：`_lr_probe.sh [lr=3e-5]`     门控场数用 env `GATE_N`（默认 512）
set -euo pipefail
cd "$(dirname "$0")"
PY=/root/miniconda3/bin/python
DATA="data/logs/ctrl_big.npz"
INIT="$(ls -1 data/v3-train_4-e2-*.pt | head -1)"
BASE="$(ls -1 data/ctrl_big-e2-*-pi_weightadvantage-*.pt | head -1)"
LR="${1:-3e-5}"
GATE_N="${GATE_N:-512}"
OUT="data/logs/lrprobe_$(date +%m%d_%H%M)"
mkdir -p "$OUT"
[ -s "$DATA" ] || { echo "❌ 缺 $DATA"; exit 1; }
echo "数据 = $DATA（$(du -h "$DATA" | cut -f1)）"
echo "起点 = ${INIT##*/}"
echo "对照 = ${BASE##*/}（lr 3e-4，实测 +1.00pp）"
echo "新臂 = lr $LR   门控 $GATE_N 场/项   输出 = $OUT"

LOG="$OUT/train_off_lr$LR.log"
echo "===== 训练 off lr=$LR（全量 876k 行 × 2 epoch）====="
date +%T
"$PY" -u train.py train "$DATA" 2 \
    --lr "$LR" --wd 0.0001 --batch 256 --z-spec hp --soft-target --soft-temp 1.0 \
    --pi-weight advantage --pi-objective off --loss-chunk 16384 \
    --device cuda --threads 16 --record 0 --init "$INIT" >"$LOG" 2>&1
NEW="$("$PY" -c 'import sys;print([l.split()[1] for l in open(sys.argv[1],encoding="utf-8") if "已保存" in l][-1])' "$LOG")"
grep -aE "权重漂移|最终 train" "$LOG" | tail -2
echo "新臂 = ${NEW##*/}"
date +%T

echo "===== 门控（$GATE_N 场/项 × 3 策略，裸网络）====="
"$PY" -u eval_encs.py "$GATE_N" "net:$INIT,net:$BASE,net:$NEW" 100000000 "$OUT/gate.json" \
    --workers 32 >"$OUT/gate.log" 2>&1
tail -3 "$OUT/gate.log"
date +%T

echo "===== 配对（键 = enc_id/floor/seed）====="
"$PY" - "$OUT/gate.json" "$INIT" "$BASE" "$NEW" "$OUT/pair.txt" <<'PYX' | tee "$OUT/pair.txt"
import json, math, sys

path, ini, base, new, out = sys.argv[1:6]
d = json.load(open(path, encoding="utf-8"))
IW = 1
specs = d["specs"]
LAB = {ini: "起点 v3-train_4", base: "ctrl_big@3e-4 (+1.00pp 那支)", new: "ctrl_big@3e-5"}


def grab(s):
    """行布局 = (seed,) + Z_COLS ⇒ t_win 在第 1 列；键含项 ⇒ `seed` 跨项不唯一也能对上。"""
    return {(it["enc_id"], it["floor"], r[0]): int(r[IW])
            for it in d["items"] for r in it["per_spec"][s]["rows"]}


R = {s: grab(s) for s in specs}
found = {LAB.get(s, s.split("pi_objective")[-1][:12]): s for s in specs}


def pair(a, b, tag):
    ra, rb = R[a], R[b]
    n = len(ra)
    wa, wb = sum(ra.values()) / n, sum(rb.values()) / n
    aw = bw = 0
    per = {}
    for k in sorted(set(ra) & set(rb)):
        if ra[k] == rb[k]:
            continue
        if ra[k] > rb[k]:
            aw += 1
        else:
            bw += 1
        e = (k[0], k[1])
        per[e] = per.get(e, 0) + (1 if rb[k] > ra[k] else -1)
    m = aw + bw
    p = math.erfc(abs(aw - bw) / math.sqrt(m) / math.sqrt(2)) if m else 1.0
    pos = sum(1 for v in per.values() if v > 0)
    neg = sum(1 for v in per.values() if v < 0)
    print(f"  {tag:34s} {100 * wa:.2f}% → {100 * wb:.2f}%  B−A={100 * (wb - wa):+.2f}pp"
          f" ±{100 * math.sqrt(m) / n:.2f}  p={p:.3g}  逐项 好{pos}/差{neg}/平{len(per) - pos - neg}")


names = list(found)
get = lambda lab: found[[k for k in names if lab in k][0]]          # noqa: E731
print("  —— 相对起点 ——")
for lab in names:
    s = found[lab]
    if s != ini:
        pair(ini, s, f"起点 → {lab}")
print("  —— 两支互比（>0 = 小步长更好）——")
if base in specs and new in specs:
    pair(base, new, "3e-4 → 3e-5")
print(f"\n  （对象：{path}）")
PYX
echo "结果目录 = $OUT"
