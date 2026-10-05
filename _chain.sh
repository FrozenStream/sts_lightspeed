#!/usr/bin/env bash
# 自迭代驱动：自对弈采集 → 训练 → 评测，循环。
#
#   _chain.sh <起点权重> [轮数]
#
# `[轮数]` 省略或给 0 = **一直迭代**（用 `STS_SESSION=<名字> ./sync.sh kill` 停）。
#
# 采集与训练共用同一条连续链 `CUR`（第 1 轮用 <起点权重>）：每轮都是上一轮训出的网，
# 门控结果不影响它 —— 这才是「单网络从头训练到尾」。门控降级为**纯参照**：只记录候选与
# 历史最佳 `BEST` 的四项指标对比，不阻挡数据流。产物与日志一律落
# <repo>/data/（**不写 /tmp**：那里重启即丢）。任一步失败当场中止并保留已完成轮的产物，
# 不静默跳过 —— 否则会拿上一轮的权重继续跑，看起来一切正常。
#
# ⚠️ 每轮的 npz 与权重都会留在 data/（体积 ∝ N_PER），磁盘不会自己回收。
#
# 旋钮集中在下面这一段，改这里即可。
set -uo pipefail

NET="${1:?用法: _chain.sh <起点权重> [轮数]}"
ROUNDS="${2:-0}"          # 0 = 一直迭代

RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RDIR"

PY="${STS_PY:-/root/miniconda3/bin/python}"

N_PER="${N_PER:-32}"             # 采集：每项场数（总场数 = ENCS 表项数 × N_PER）
SIMS="${SIMS:-3000}"             # 采集：MCTS 预算（质量旋钮）
# 采集：MCTS 叶批大小 —— 同批 k 条路径一次前向（降 torch 派发开销）。
# **语义旋钮**（同批路径靠沿路径的 virtual loss 避开），换值必须重测下游胜率。
# 本地实测每 sim 成本（h256/d3）：批1 1168µs → 批8 366 → 批16 288 → 批32 226。
# 取 8：1→8 已吃掉 3.2×（总 5.2×），8→32 只多 1.6× 但 batch/sims 比值翻 4 倍。
MCTS_BATCH="${MCTS_BATCH:-8}"
EPOCHS="${EPOCHS:-2}"             # 训练：轮数（数量旋钮）
# 策略目标口径：1 = 用搜索的**完整访问分布** π 当软目标（`π**(1/temp)` 再归一化，AGZ 口径）；
# 0 = 旧口径硬 argmax one-hot（只教一个动作，扔掉其他动作的相对排序）。
SOFT_TARGET="${SOFT_TARGET:-1}"
SOFT_TEMP="${SOFT_TEMP:-1.0}"      # 只在 SOFT_TARGET=1 时生效；<1 更尖、>1 更平

# 学习率：续训（`--init`）用 3e-4 —— 每轮数据量上去（窗口 20 + 教师锚点）后可以更激进；
# 1e-3 对已收敛的网步长仍过大，会把权重从教师分布拽走。
LR="${LR:-3e-4}"
WORKERS="${WORKERS:-32}"


# AGZ 的探索与噪声（论文 Methods "Self-play training pipeline"）：
#   自对弈前 30 步 τ=1，之后 τ→0；根节点 P=(1-ε)p+εη，η~Dir(0.03)，ε=0.25。
#   ⚠️ α 与合法动作数相关：围棋每步 ~250 个动作，本引擎每帧远少于此，α 可能要放大。
TEMP_MOVES="${TEMP_MOVES:-30}"
# 根噪声：`DIR_TOTAL` 是 Dirichlet **总浓度**（KataGo 口径，围棋 0.03×361=10.83）——
# 每动作 α = 总浓度/合法动作数；本引擎每步只有几个动作，直接套围棋的每动作 0.03 会扰过头。
DIR_TOTAL="${DIR_TOTAL:-10.83}"
DIR_EPS="${DIR_EPS:-0.1}"           # 噪声混合权重 ε（AGZ 对围棋用 0.25；我们动作少 ⇒ 调小些）

# 回放缓冲：训练集 = 最近 WINDOW 轮自对弈 npz 的拼接（论文用“最近 500,000 局”）。
# 窗口小 ⇒ 数据少且相邻轮高度冗余，一轮内就过拟合（实测：train ce 降、val ce 涨）。
# 训完即删（`TRAINSET`），各轮 npz 留在 data/（体积 ∝ WINDOW × N_PER）。
WINDOW="${WINDOW:-20}"

# 教师锚点：每轮都拼进训练集（不占窗口、不会被逐出）。作用是别把蒸馏学到的先验忘掉；
# 窗口满时它只占 ~2k/42k 局 ≈ 5% 权重，不构成天花板。空串 = 关。
ANCHOR="${ANCHOR:-data/encdata_n32_s2000.npz}"

# 门控：同一批 seed 上，候选与**历史最佳 best** 各在 encs 全表跑 GATE_N_PER 场/项（**裸网络**：
# 不给 `:sims`，直接量网络本身，没有搜索遮挡），领先 ≥ GATE_MARGIN 就把 best 换成候选。
# **门控只标记 best，不改变采集起点**（采集始终跟着 CUR）。
# ⚠️ 别拿"绝对胜率"比固定阈值（如 0.55）：那个数常年 0.9 以上，什么网都过。
# ⚠️ 场数下限由裕度反推：合计胜率差的标准差 ≈ √(p(1-p)/N) 的两倍（两臂各有噪声），
#   32 场/项（N=1024）时约 ±2pp > GATE_MARGIN ⇒ 判据落进噪声里，接受/拒绝近乎掷硬币。
GATE_N_PER="${GATE_N_PER:-128}"
GATE_MARGIN="${GATE_MARGIN:-0.01}"
# 门控用**固定** seed 段（= `collect.EVAL_SEED0`）：每轮换 seed 的话，绝对胜率不可跳轮比
# （实测：同一个冻结权重在不同 seed 段上能差 1.9pp），曲线只能看配对差。
# 代价：这批棋盘被反复用于升格判据 ⇒ 判据本身对这批棋盘有轻微过拟合；真实泛化另抽一段 seed 验。
GATE_SEED0="${GATE_SEED0:-100000000}"

STAMP="${STAMP:-$(date +%m%d_%H%M)}"   # 可用外部 STAMP 指定（复用某个已存在的轮次目录/缓存）
LOGD="data/logs/chain_${STAMP}"
mkdir -p "$LOGD"

say() { printf '%s %s\n' "$(date +%T)" "$*"; }
die() { say "$*"; exit 1; }

say "自迭代启动：起点=$NET  轮数=$(if [ "$ROUNDS" -gt 0 ]; then echo "$ROUNDS"; else echo '不限（./sync.sh kill 停）'; fi)"
say "  采集 $N_PER 场/项 × MCTS($SIMS，批 $MCTS_BATCH)（ENCS 全表）  温度前 $TEMP_MOVES 决策点  根噪声 ε=$DIR_EPS 总浓度=$DIR_TOTAL"
say "  训练 $EPOCHS epoch  lr=$LR（回放窗口 $WINDOW 轮，教师锚点 ${ANCHOR:-关}）  目标=$(if [ "$SOFT_TARGET" = 1 ]; then echo "软分布 π**(1/$SOFT_TEMP)"; else echo 硬argmax; fi)  门控 $GATE_N_PER 场/项（裸网络）/ 候选须领先 $GATE_MARGIN"
say "  日志目录 $LOGD"

CUR="$NET"         # 连续训练链：每轮都是它的续训，且**自对弈数据也由它产生**
BEST="$NET"        # 只做参照：门控领先才升格，不再影响数据流
BUFFER=()          # 滑动窗口：最近 WINDOW 轮的 npz
[ -z "$ANCHOR" ] || [ -s "$ANCHOR" ] || die "教师锚点不存在：$ANCHOR"
r=0

# 断点续跑：看门狗重启不从头来。gate_history.txt 每轮追加
# `r=<轮>  cand=<权重>  win_diff=<差>  best=<权重>`，取末行恢复轮号与 best；
# 回放窗口按确定性命名 `data/it<N>_${STAMP}_*.npz` 回捞（同一 STAMP 才能续）。
if [ -s "$LOGD/gate_history.txt" ]; then
    LAST="$(tail -1 "$LOGD/gate_history.txt")"
    r="${LAST#r=}"
    r="${r%% *}"
    read -r r RBEST RCUR RCAND < <(printf '%s\n' "$LAST" | awk '
        {for (i = 1; i <= NF; i++) {split($i, a, "="); v[a[1]] = a[2]}}
        END {print v["r"], v["best"], v["cur"], v["cand"]}')
    case "$r" in ''|*[!0-9]*) die "断点续跑失败：末行轮号不是数字 ⇒ $(printf '%s' "$LAST")" ;; esac
    [ -n "$RCUR" ] || RCUR="$RCAND"        # 旧记录没有 cur= ⇒ 退回最后一个候选
    [ -f "$RBEST" ] || die "断点续跑失败：历史 best 权重不存在 ⇒ $RBEST"
    [ -f "$RCUR" ] || die "断点续跑失败：历史 cur 权重不存在 ⇒ $RCUR"
    BEST="$RBEST"
    CUR="$RCUR"
    i=$((r - WINDOW + 1))
    if [ "$i" -lt 1 ]; then i=1; fi
    while [ "$i" -le "$r" ]; do
        for f in data/it${i}_${STAMP}_*.npz; do
            if [ -f "$f" ]; then BUFFER+=("$f"); fi
        done
        i=$((i + 1))
    done
    [ "${#BUFFER[@]}" -gt 0 ] || die "断点续跑失败：找不到 it<N>_${STAMP}_*.npz 回放数据"
    say "断点续跑：已完成 $r 轮，从第 $((r + 1)) 轮继续，best=${BEST##*/}，续训=${CUR##*/}，回放 ${#BUFFER[@]} 份（窗口 $WINDOW）"
fi

while :; do
    r=$((r + 1))
    if [ "$ROUNDS" -gt 0 ] && [ "$r" -gt "$ROUNDS" ]; then
        break
    fi
    say "===== 第 $r 轮（采集+续训=${CUR##*/}  参照 best=${BEST##*/}） ====="

    say "[$r] 采集"
    "$PY" -u collect_selfplay.py "$N_PER" "$SIMS" "$CUR" \
        --workers "$WORKERS" --cache-dir data --name "it${r}_${STAMP}" \
        --mcts-batch "$MCTS_BATCH" \
        --temp-moves "$TEMP_MOVES" --dirichlet-total "$DIR_TOTAL" --dirichlet-eps "$DIR_EPS" \
        >"$LOGD/collect_$r.log" 2>&1 || die "[$r] 采集失败 ⇒ 见 collect_$r.log"
    # 解析不依赖行首缩进：同一类消息在 `collect_selfplay.py` / `train.py` 里不止一处，
    # 格式（缩进）并不统一 —— 写死 `^  已保存` 会在单种子路径上永远匹配不到。
    DATA="$(grep -oE '已缓存到 [^ ]+' "$LOGD/collect_$r.log" | tail -1 | cut -d' ' -f2)"
    [ -n "$DATA" ] || die "[$r] 解析不到缓存路径（采集可能成功但日志格式变了）⇒ 见 collect_$r.log"
    say "[$r] 数据 $DATA"
    # 数据集弱于教师的告警（`collect_selfplay` 打在采集日志里）：抬到链条日志，免得埋在某轮里
    W="$(grep -aoE '⚠️.*' "$LOGD/collect_$r.log" 2>/dev/null | tail -1)"
    [ -n "$W" ] && say "[$r] $W"

    # ---- 回放缓冲：滑窗拼接（`mix_caches` 会把后一份的 bid 错开） ----
    BUFFER+=("$DATA")
    while [ "${#BUFFER[@]}" -gt "$WINDOW" ]; do BUFFER=("${BUFFER[@]:1}"); done
    TRAINSET="$LOGD/train_$r.npz"
    "$PY" - "$TRAINSET" "${BUFFER[@]}" ${ANCHOR:+"$ANCHOR"} <<'PYX' || die "[$r] 缓冲拼接失败"
import sys
import numpy as np
from collect import mix_caches
out, *srcs = sys.argv[1:]
acc = dict(np.load(srcs[0], allow_pickle=False))
for s in srcs[1:]:
    acc = mix_caches(acc, dict(np.load(s, allow_pickle=False)), where=s + ":")
np.savez(out, **acc)
print(f"训练集 {out}：{len(srcs)} 份 / {len(np.unique(acc['bid']))} 场 / {len(acc['obs'])} 行")
PYX

    say "[$r] 训练（续训起点 ${CUR##*/}）"
    # 配方全显式；建网超参不在这里（固定在 `aznet.py`）。
    # `seed` / `val_frac` 不是 flag ⇒ 走默认 0 / 0.2。
    SOFT_ARG=()                          # 目标口径：软分布 或 硬 argmax
    [ "$SOFT_TARGET" = 1 ] && SOFT_ARG=(--soft-target --soft-temp "$SOFT_TEMP")
    "$PY" -u train.py train "$TRAINSET" "$EPOCHS" --threads "$WORKERS" \
        --lr "$LR" --wd 0.0001 --batch 256 --z-spec hp ${SOFT_ARG[@]+"${SOFT_ARG[@]}"} \
        --loss-chunk 65536 --device cpu --record 1 --record-dir data/runs \
        --init "$CUR" \
        >"$LOGD/train_$r.log" 2>&1 || die "[$r] 训练失败 ⇒ 见 train_$r.log"
    CAND="$(grep -oE '已保存 [^ ]+' "$LOGD/train_$r.log" | tail -1 | cut -d' ' -f2)"
    [ -n "$CAND" ] || die "[$r] 解析不到权重路径（训练可能成功但日志格式变了）⇒ 见 train_$r.log"
    say "[$r] 候选权重 $CAND"
    # 权重漂移（候选 vs 续训起点）：门控差为零时，用它区分「没退化」与「根本没动」。
    DW="$(grep -oE '权重漂移 [0-9.eE+-]+' "$LOGD/train_$r.log" | tail -1 | cut -d' ' -f2)"
    say "[$r] 权重漂移 ${DW:-未报告}"
    CUR="$CAND"        # 无条件继承：门控只决定 best，不决定网络是否继续训练
    rm -f "$TRAINSET"

    # ---- 门控：候选 vs 当前 best（同 seed 配对，裸网络，encs 同分布） ----
    # 用 `eval_encs.py`：每项就地造一场 = 与采集/训练**同分布**，且逐项可比；`eval_walk` 是整局
    # 漫游（分布不同、贵 ~50×）。种子必须落在 held-out 段（`collect.EVAL_SEED0` = 1e8）。
    say "[$r] 门控：$GATE_N_PER 场/项（裸网络，encs，固定 seed 段）"
    GSPEC_C="net:$CAND"
    GSPEC_B="net:$BEST"
    "$PY" -u eval_encs.py "$GATE_N_PER" "$GSPEC_C,$GSPEC_B" "$GATE_SEED0" \
        "$LOGD/gate_$r.json" --workers "$WORKERS" \
        >"$LOGD/gate_$r.log" 2>&1 || die "[$r] 门控失败 ⇒ 见 gate_$r.log"
    tail -40 "$LOGD/gate_$r.log" >>"$LOGD/summary.txt"
    # 同 seed 配对 ⇒ 两条胜率可直接比。取最高优先级的判据：胜率差。
    WIN="$("$PY" - "$LOGD/gate_$r.json" "$GSPEC_C" "$GSPEC_B" <<'PYX'
import json, sys
from eval_encs import summarize
d = json.load(open(sys.argv[1], encoding="utf-8"))
rows = {s: [r for it in d["items"] for r in it["per_spec"][s]["rows"]] for s in d["specs"]}
c, b = (summarize(rows[s])["win_rate"] for s in (sys.argv[2], sys.argv[3]))
# 第一段给 `awk` 比大小（小数），其余三段给人看（百分比）
print(f"{c - b:.6f}|{c:.1%}|{b:.1%}|{c - b:+.1%}")
PYX
)" || die "[$r] 门控结果解析失败 ⇒ 见 gate_$r.json"
    DIFF="${WIN%%|*}"
    ROW="$(echo "$WIN" | cut -d'|' -f2- | tr '|' ' ')"
    DCAND="$(echo "$ROW" | cut -d' ' -f1)"
    DBEST="$(echo "$ROW" | cut -d' ' -f2)"
    DDIFF="$(echo "$ROW" | cut -d' ' -f3)"
    MARGIN_PCT="$(awk -v t="$GATE_MARGIN" 'BEGIN{printf "%.1f%%", t * 100}')"
    say "[$r] 门控：候选 $DCAND vs best $DBEST  差 $DDIFF"

    # `awk` 用小数比大小（`DIFF`），人看的是百分比（`DDIFF`）。
    if awk -v d="$DIFF" -v t="$GATE_MARGIN" 'BEGIN{exit !(d >= t)}'; then
        say "[$r] ✅ 采纳（领先 $DDIFF ≥ $MARGIN_PCT）⇒ best 更新"
        BEST="$CAND"
    else
        say "[$r] ❌ 未领先（领先 $DDIFF < $MARGIN_PCT）⇒ best 保持不变（不影响采集）"
    fi
    printf 'r=%s  cand=%s  win_diff=%s  dw=%s  cur=%s  best=%s\n' "$r" "$CAND" "$DIFF" "${DW:--}" "$CUR" "$BEST" >>"$LOGD/gate_history.txt"
done

say "自迭代结束：共 $((r - 1)) 轮，日志在 $LOGD"
