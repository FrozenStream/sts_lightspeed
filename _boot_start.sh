#!/usr/bin/env bash
# 一次性引导：教师缓存 → 蒸馏起点权重 → 起跑自迭代看门狗（`_watch.sh`）。
#
#   _boot_start.sh [教师N_PER] [教师SIMS] [蒸馏EPOCHS]
#
# 起点必须是**当前引擎**产出的教师数据蒸馏出来的：旧权重与现在的建网配置不同，加载不了。
# 教师不走网络（`collect_encs.py` = C++ MCTS 逐项 argmax）⇒ 天然与当前口径一致。
# 幂等：分片已存在则跳过，缓存与权重按参数命名 ⇒ 中断后重跑即续。
#
# 旋钮分两组，全部显式传给下游（不依赖它们的默认值）：
#   `T_*`  教师缓存 + 蒸馏（只决定起点权重）
#   `SP_*` 自对弈链条（`_chain.sh` 每轮的采集/训练）+ 门控/温度/噪声
#
# 产物：`data/encdata_n<N>_s<S>.npz`、`data/boot_n<N>_s<S>_e<E>.pt`、`data/logs/boot_n<N>_s<S>/`。
set -uo pipefail

RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RDIR"

PY="${STS_PY:-/root/miniconda3/bin/python}"

# ---- ① 教师缓存 + 蒸馏（只决定起点权重） ----
T_N_PER="${1:-32}"      # 每项多少场
T_SIMS="${2:-2000}"     # 教师 MCTS 模拟数（纯 C++ 随机 rollout，很便宜）
T_EPOCHS="${3:-16}"     # 蒸馏轮数

# ---- ② 自对弈链条（原样传给 `_watch.sh` → `_chain.sh`） ----
SP_N_PER="${SP_N_PER:-32}"       # 每轮：每项场数
SP_SIMS="${SP_SIMS:-3000}"       # 每轮：MCTS 预算（实测 3.66 s/场 ⇒ 采集 ~2 h/轮，非线性的）
SP_EPOCHS="${SP_EPOCHS:-2}"      # 每轮：训练轮数
MCTS_BATCH="${MCTS_BATCH:-8}"    # 每轮采集：MCTS 叶批大小（>1 会改搜索语义）
SOFT_TARGET="${SOFT_TARGET:-1}"  # 策略目标：1 = 搜索的完整访问分布（模仿），0 = 硬 argmax
SOFT_TEMP="${SOFT_TEMP:-1.0}"    # 软目标温度（只在 SOFT_TARGET=1 时生效）
LR="${LR:-3e-4}"                  # 续训学习率（1e-3 对已收敛的网步长过大）
WINDOW="${WINDOW:-20}"            # 回放窗口（轮）
ANCHOR="${ANCHOR:-data/encdata_n${T_N_PER}_s${T_SIMS}.npz}"   # 教师锚点（每轮都拼进训练集）
GATE_N_PER="${GATE_N_PER:-128}"   # 门控：每项场数（裸网络，encs 全表）
GATE_MARGIN="${GATE_MARGIN:-0.01}" # 候选须领先多少才升 best
GATE_SEED0="${GATE_SEED0:-100000000}" # 门控**固定** seed 段（跳轮可比）；= `collect.EVAL_SEED0`
BASE_SIMS="${BASE_SIMS:-1000}"    # 教师基准的搜索预算（默认与采集的 SIMS 同）
TEMP_MOVES="${TEMP_MOVES:-30}"     # 前多少决策点用 τ=1
DIR_TOTAL="${DIR_TOTAL:-10.83}"     # 根噪声 Dirichlet **总浓度**（每动作 α = 总浓度/动作数）
DIR_EPS="${DIR_EPS:-0.1}"           # 根噪声混合权重 ε
WORKERS="${WORKERS:-32}"           # 两侧共用（物理核数）

CACHE="data/encdata_n${T_N_PER}_s${T_SIMS}.npz"
START="data/boot_n${T_N_PER}_s${T_SIMS}_e${T_EPOCHS}.pt"
LOGD="data/logs/boot_n${T_N_PER}_s${T_SIMS}"
mkdir -p "$LOGD"

say() { printf '%s %s\n' "$(date +%T)" "$*" | tee -a "$LOGD/boot.log"; }

say "起点引导：教师 N_PER=$T_N_PER SIMS=$T_SIMS EPOCHS=$T_EPOCHS WORKERS=$WORKERS"
say "  链条每轮：N_PER=$SP_N_PER SIMS=$SP_SIMS（批 $MCTS_BATCH） EPOCHS=$SP_EPOCHS lr=$LR 窗口=$WINDOW（锚点 $ANCHOR）根噪声 ε=$DIR_EPS/总浓度 $DIR_TOTAL"
say "  门控 $GATE_N_PER 场/项（裸网络）/ 裕度 $GATE_MARGIN；温度前 $TEMP_MOVES 决策点；噪声 ε=$DIR_EPS 总浓度=$DIR_TOTAL"

if [ ! -s "$CACHE" ]; then
    say "① 采集教师缓存 $CACHE"
    # `--eps 0` = 教师 argmax（不加均匀探索）；`--seed0 0` = 种子段起点。
    # 不加 `--force` ⇒ 已有分片跳过 ⇒ 中断后重跑即续采。
    "$PY" -u collect_encs.py "$T_N_PER" "$T_SIMS" "$CACHE" \
        --workers "$WORKERS" --cache-dir data --eps 0.0 --seed0 0 \
        >"$LOGD/collect.log" 2>&1 || { say "① 采集失败 ⇒ 见 $LOGD/collect.log"; exit 1; }
fi
say "① 缓存就绪 $(du -h "$CACHE" | cut -f1)"

if [ ! -s "$START" ]; then
    say "② 从零蒸馏 ⇒ $START"
    # 建网超参固定在 `aznet.py`（传了会报 unrecognized arguments）；目标口径见 `SOFT_TARGET`；
    # `seed` / `val_frac` 不是 flag ⇒ 走默认 0 / 0.2。
    SOFT_ARG=()                        # 起点蒸馏就是**模仿** ⇒ 与链条同口径
    [ "$SOFT_TARGET" = 1 ] && SOFT_ARG=(--soft-target --soft-temp "$SOFT_TEMP")
    "$PY" -u train.py train "$CACHE" "$T_EPOCHS" "$START" \
        --lr 0.001 --wd 0.0001 --batch 256 --z-spec hp ${SOFT_ARG[@]+"${SOFT_ARG[@]}"} \
        --loss-chunk 65536 --device cpu --threads "$WORKERS" \
        --record 1 --record-dir data/runs \
        >"$LOGD/train.log" 2>&1 || { say "② 训练失败 ⇒ 见 $LOGD/train.log"; exit 1; }
fi
say "② 起点权重就绪 $START"

# ---- ③ 教师基准：固定 seed 段上跑一次，作“教师成绩”参照（幂等：已存在则跳过） ----
BASE="data/baseline/teacher_s${BASE_SIMS}.json"
if [ ! -s "$BASE" ]; then
    say "③ 教师基准 ⇒ $BASE（$GATE_N_PER 场/项 × 全表，seed0=$GATE_SEED0）"
    mkdir -p data/baseline
    # 教师不走网络（纯 C++ MCTS），与建网配置无关 ⇒ 换网络尺寸/容量不用重跑。
    "$PY" -u eval_encs.py "$GATE_N_PER" "teacher:$BASE_SIMS" "$GATE_SEED0" "$BASE" \
        --workers "$WORKERS" >"$LOGD/baseline.log" 2>&1 \
        || { say "③ 教师基准失败 ⇒ 见 $LOGD/baseline.log"; exit 1; }
fi
say "③ 教师基准就绪 $(du -h "$BASE" | cut -f1)"

STAMP="${STAMP:-$(date +%m%d_%H%M)}"
say "④ 起跑自迭代（STAMP=$STAMP，看门狗会在它退出后 30 秒重启）"
export STAMP
export N_PER="$SP_N_PER" SIMS="$SP_SIMS" EPOCHS="$SP_EPOCHS" LR WORKERS WINDOW MCTS_BATCH ANCHOR
# 起点蒸馏**已经**按这个口径训过 ⇒ 链条里必须接着用同一个，否则等于蒸馏白做
export SOFT_TARGET SOFT_TEMP
export GATE_N_PER GATE_MARGIN GATE_SEED0 TEMP_MOVES DIR_TOTAL DIR_EPS
exec ./_watch.sh "$START"
