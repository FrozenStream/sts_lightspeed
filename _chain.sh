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
# ⚠️ 每轮的采集产物（分片**目录**，体积 ∝ N_PER）与权重都留在 data/，磁盘不会自己回收。
#
# 旋钮集中在下面这一段，改这里即可。
set -uo pipefail

NET="${1:?用法: _chain.sh <起点权重> [轮数]}"
ROUNDS="${2:-0}"          # 0 = 一直迭代

RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RDIR"

PY=/root/miniconda3/bin/python

# ── 旋钮（**裸常量**：要改就改这里的字面量）──
# 外部覆盖不在这些行里做，而是集中在下面一个 `for` 循环（接 `STS_*`）。
N_PER=32                         # 采集：每项场数（总场数 = ENCS 表项数 × N_PER）
SIMS=3000                        # 采集：MCTS 预算（质量旋钮）
# 损失分块的**行数**。它是**配方旋钮**（进权重名）。
# ⚠️ 内存随 `hidden` **线性**放大：因子策略头的中间张量是 `[chunk, 手牌槽, 目标槽, hidden]`。
#    实测 `hidden=512` + `chunk=65536` ⇒ 单块 16 GiB ⇒ 当场 OOM（试了拉不动）。
#    16384 ≈ `hidden=512` 下与旧档（256/65536）相当的内存足迹。
LOSS_CHUNK=16384

# 采集：**一次调用并肩几棵树**（跨树合批的行数 = 前向宽度）。
# 形状（2026-10-09 起）：树内每轮只降 **1 条路径**（STS 的合法动作 mean 4.7 下，树内批会让降序
# 阶段的 Q 退化成常数 ⇒ 那批里只有第 1 条真在搜索）；宽度全部来自"同时有几棵树"。
# ⇒ 它**不改变任何一棵树的搜索语义**（实测：并肩 4 棵与 1 棵的 π 逐位相同，K=1024 也一样）。
# ⚠️ 驱动已并行化（K 条树线程各降各的 + 主线程独占前向、每轮一次屏障）⇒ 宽度直接兑现吞吐。
# 2026-10-10 远端实测（4080S / 16 核 / 单进程 / v3-train_9 / sims=200）：
#   宽度 16 → 8.5k sims/s ；32 → 15.9k ；64 → 27.5k ；128 → **43.6k** ；256 → **56.0k**（均宽 256）
#   ⇒ 拐点在 **256 行**（约 43~56k sims/s），256→512 不再涨（前向吞吐已到顶：均宽 256 时
#   37.7k 行/s ≈ GPU 单调用实测的 37.4k）。旧形状（C++ lockstep、树内批 64）同机 27.3k ⇒ **新形状快 2×**。
# ⚠️ **内存**：每棵树 ≈ 7.5 MB（K=512 实测峰值 RSS 3.8 GiB）⇒ `GAMES` 512 约 4 GB/进程。
# ⚠️ **单进程**（2026-10-13 起不靠 flag）：`sts.selfplay_run` 在**本进程**里跑完整段，没有
#    worker 池 ⇒ 不会出现"多进程各带一个 CUDA 上下文"（那两次容器被踩断的老问题）。
# ⚠️ 批宽 = `GAMES`（一棵树一条线程；树线程阻塞在自己的请求上、不占 CPU）⇒ **`GAMES` 给多大，
#    前向就吃多宽**。2026-10-12 的"一棵树一条线程"重写里把线程数写死成 `min(K, 16)` ⇒ 那段
#    时间里 `GAMES>16` 只是白占内存（2026-10-14 修回按 K 开线程，实测同一批 63 场 sims=8：
#    `games=1` 14s → `games=63` 1s）。
GAMES=256
# 推理后端：`cpp` = 前向在 C++ 侧（TorchScript 存档 + 进程内 `TorchEvaluator`；**唯一生产路径**）。
# 旧的两个后端已删（2026-10-09）：`eager`（每 worker 自建 net 就地 CPU 前向）与 `server`
# （共享推理服务）都只服务旧的 Python 树路径 —— 那条路已整段下沉到 C++。
INFER=cpp
EPOCHS=2                          # 训练：轮数（数量旋钮）
# 策略目标口径：1 = 用搜索的**完整访问分布** π 当软目标（`π**(1/temp)` 再归一化，AGZ 口径）；
# 0 = 旧口径硬 argmax one-hot（只教一个动作，扔掉其他动作的相对排序）。
SOFT_TARGET=1
SOFT_TEMP=1.0                     # 只在 SOFT_TARGET=1 时生效；<1 更尖、>1 更平
# π 损失的逐行权重口径（`train.py` 同名形参）：`none` 每行等权 / `advantage` = `max(0, z−V̂)` 归一。
# 等权时「在输局里也照抄教师」是**负作用**（实测两份数据各 −0.08 / −0.30pp）；加权消掉这份害
# ⇒ 相对等权 +0.42pp（32256 场配对，p = 0.0042 / 0.0025）。每轮效应 ≪ `GATE_MARGIN`。
PI_WEIGHT=advantage

# 学习率：续训（`--init`）用 3e-4 —— 每轮数据量上去后可以更激进；
# 1e-3 对已收敛的网步长仍过大，会把权重从教师分布拽走。
LR=3e-4
# 采集 / 训练线程 / 门控三处共用。`STS_WORKERS` 是 `sync.sh start` 唯一会带过环境变量的名字
# （见 sync.sh 头注），链条坐在这两边 —— 不接上就只能吃默认 32，在大机器上超订。
# ⚠️ **采集端必须 1**：自对弈现在是"单进程 + K 棵树并肩"（一次前向吃 K 行），GPU 上再多一个进程
#    就是多一个 CUDA 上下文 —— 那条路踩断过两次容器。训练/门控仍按核数用 32。
WORKERS=32
COLLECT_WORKERS=1
# 谱系标签：**进训练集名 ⇒ 进而进权重名**（`runtime.ckpt_name` 的第一段就是数据引用）。
# 用途：建网超参不进权重名（见 `aznet.py` 头注）⇒ 换容量时必须靠它把新旧权重分开，否则同名覆盖。
# 建议建新谱系时给一个，如 `LINEAGE=v2-` ⇒ 权重名变成 `v2-train_1-e2-…`。留空 = 沿用旧名字。
LINEAGE=


# AGZ 的探索与噪声（论文 Methods "Self-play training pipeline"）：
#   自对弈前 30 步 τ=1，之后 τ→0；根节点 P=(1-ε)p+εη，η~Dir(0.03)，ε=0.25。
#   ⚠️ α 与合法动作数相关：围棋每步 ~250 个动作，本引擎每帧远少于此，α 可能要放大。
TEMP_MOVES=30
# 根噪声：`DIR_TOTAL` 是 Dirichlet **总浓度**（KataGo 口径，围棋 0.03×361=10.83）——
# 每动作 α = 总浓度/合法动作数；本引擎每步只有几个动作，直接套围棋的每动作 0.03 会扰过头。
DIR_TOTAL=10.83
DIR_EPS=0.1                        # 噪声混合权重 ε（AGZ 对围棋用 0.25；我们动作少 ⇒ 调小些）

# 回放缓冲与教师锚点已删（2026-10-14）：训练集 = **本轮**自对弈数据。拼接多轮数据是另一个
# 训练语义（要做得单独接），别顺手加回来。
# ⚠️ 代价是实测过的：链每轮只加 ~2000 场，样本又小又自我强化 —— 同起点同口径下，单份 41k 行
#    **等权**每轮更新是 **−1.41pp**；挂 876k 行教师锚点才 **+1.00pp**（p≈0.0000，逐项 45/14）。
#    ⇒ 门控长期不升格时，先怀疑这里，而不是搜索旋钮。

# 门控：同一批 seed 上，候选与**历史最佳 best** 各在 encs 全表跑 GATE_N_PER 场/项（**裸网络**：
# 不给 `:sims`，直接量网络本身，没有搜索遮挡），领先 ≥ GATE_MARGIN 就把 best 换成候选。
# **门控只标记 best，不改变采集起点**（采集始终跟着 CUR）。
# ⚠️ 别拿"绝对胜率"比固定阈值（如 0.55）：那个数常年 0.9 以上，什么网都过。
# ⚠️ 场数下限由裕度反推：合计胜率差的标准差 ≈ √(p(1-p)/N) 的两倍（两臂各有噪声），
#   32 场/项（N=1024）时约 ±2pp > GATE_MARGIN ⇒ 判据落进噪声里，接受/拒绝近乎掷硬币。
GATE_N_PER=512
GATE_MARGIN=0.01
# 门控用**固定** seed 段（= `collect.EVAL_SEED0`）：每轮换 seed 的话，绝对胜率不可跳轮比
# （实测：同一个冻结权重在不同 seed 段上能差 1.9pp），曲线只能看配对差。
# 代价：这批棋盘被反复用于升格判据 ⇒ 判据本身对这批棋盘有轻微过拟合；真实泛化另抽一段 seed 验。
GATE_SEED0=100000000

# 轮次目录名：默认 = 当前时间；外部可用 `STS_STAMP` 指定（复用某个已存在的轮次目录/缓存）
STAMP="$(date +%m%d_%H%M)"

# ── 外部覆盖 ──
# `sync.sh start` **只转发 `STS_*`**（见 sync.sh 的 `_fwd`），而上面的旋钮名不带前缀
# ⇒ 用这一层把 `STS_GAMES=16` 之类接进来。
# ⚠️ 空值不生效（`[ -n ]` 判据）—— “显式传空”只对上面赋空常量的那几个（`LINEAGE`）有意义。
for _k in N_PER SIMS GAMES COLLECT_WORKERS INFER EPOCHS LR WORKERS LINEAGE \
          LOSS_CHUNK GATE_N_PER GATE_MARGIN GATE_SEED0 TEMP_MOVES DIR_TOTAL DIR_EPS \
          SOFT_TARGET SOFT_TEMP PI_WEIGHT STAMP; do
    _s="STS_$_k"
    if [ -n "${!_s:-}" ]; then printf -v "$_k" '%s' "${!_s}"; fi
done
unset _k _s

LOGD="data/logs/chain_${STAMP}"
mkdir -p "$LOGD"

say() { printf '%s %s\n' "$(date +%T)" "$*"; }
die() { say "$*"; exit 1; }

say "自迭代启动：起点=$NET  轮数=$(if [ "$ROUNDS" -gt 0 ]; then echo "$ROUNDS"; else echo '不限（./sync.sh kill 停）'; fi)"
say "  采集 $N_PER 场/项 × MCTS($SIMS，单进程并肩 $GAMES 棵)（ENCS 全表）  温度前 $TEMP_MOVES 决策点  根噪声 ε=$DIR_EPS 总浓度=$DIR_TOTAL"
say "  训练 $EPOCHS epoch  lr=$LR  目标=$(if [ "$SOFT_TARGET" = 1 ]; then echo "软分布 π**(1/$SOFT_TEMP)"; else echo "硬argmax"; fi)  π损失权重=$PI_WEIGHT  门控 $GATE_N_PER 场/项（裸网络）/ 候选须领先 $GATE_MARGIN"
say "  日志目录 $LOGD"

CUR="$NET"         # 连续训练链：每轮都是它的续训，且**自对弈数据也由它产生**
BEST="$NET"        # 只做参照：门控领先才升格，不再影响数据流
r=0

# 断点续跑：看门狗重启不从头来。gate_history.txt 每轮追加
# `r=<轮>  cand=<权重>  cur=<权重>  best=<权重>`，取末行恢复轮号、best 与续训链。
if [ -s "$LOGD/gate_history.txt" ]; then
    LAST="$(tail -1 "$LOGD/gate_history.txt")"
    read -r r RBEST RCUR RCAND < <(printf '%s\n' "$LAST" | awk '
        {for (i = 1; i <= NF; i++) {split($i, a, "="); v[a[1]] = a[2]}}
        END {print v["r"], v["best"], v["cur"], v["cand"]}')
    case "$r" in ''|*[!0-9]*) die "断点续跑失败：末行轮号不是数字 ⇒ $(printf '%s' "$LAST")" ;; esac
    [ -n "$RCUR" ] || RCUR="$RCAND"        # 旧记录没有 cur= ⇒ 退回最后一个候选
    [ -f "$RBEST" ] || die "断点续跑失败：历史 best 权重不存在 ⇒ $RBEST"
    [ -f "$RCUR" ] || die "断点续跑失败：历史 cur 权重不存在 ⇒ $RCUR"
    BEST="$RBEST"
    CUR="$RCUR"
    say "断点续跑：已完成 $r 轮，从第 $((r + 1)) 轮继续，best=${BEST##*/}，续训=${CUR##*/}"
fi

while :; do
    r=$((r + 1))
    if [ "$ROUNDS" -gt 0 ] && [ "$r" -gt "$ROUNDS" ]; then
        break
    fi
    say "===== 第 $r 轮（采集+续训=${CUR##*/}  参照 best=${BEST##*/}） ====="

    say "[$r] 采集"
    "$PY" -u collect_selfplay.py --n "$N_PER" --sims "$SIMS" --net-path "$CUR" \
        --cache-dir data --name "it${r}_${STAMP}" \
        --games "$GAMES" \
        --temp-moves "$TEMP_MOVES" --dirichlet-total "$DIR_TOTAL" --dirichlet-eps "$DIR_EPS" \
        >"$LOGD/collect_$r.log" 2>&1 || die "[$r] 采集失败 ⇒ 见 collect_$r.log"
    # 解析不依赖行首缩进：同一类消息在 `collect_selfplay.py` / `train.py` 里不止一处，
    # 格式（缩进）并不统一 —— 写死 `^  已保存` 会在单种子路径上永远匹配不到。
    DATA="$(grep -oE '产物目录 [^ ]+' "$LOGD/collect_$r.log" | tail -1 | cut -d' ' -f2)"
    [ -n "$DATA" ] || die "[$r] 解析不到产物目录（采集可能成功但日志格式变了）⇒ 见 collect_$r.log"
    say "[$r] 数据 $DATA"
    # 数据集弱于教师的告警（`collect_selfplay` 打在采集日志里）：抬到链条日志，免得埋在某轮里
    W="$(grep -aoE '⚠️.*' "$LOGD/collect_$r.log" 2>/dev/null | tail -1)"
    [ -n "$W" ] && say "[$r] $W"

    # 训练集 = 本轮数据（分片目录；回放缓冲已删，见文件头）
    TRAINSET="$DATA"

    say "[$r] 训练（续训起点 ${CUR##*/}）"
    # 配方全显式；建网超参不在这里（固定在 `aznet.py`）。
    # `seed` / `val_frac` 不是 flag ⇒ 走默认 0 / 0.2。
    SOFT_ARG=()                          # 目标口径：软分布 或 硬 argmax
    [ "$SOFT_TARGET" = 1 ] && SOFT_ARG=(--soft-target --soft-temp "$SOFT_TEMP")
    "$PY" -u train.py train "$TRAINSET" "$EPOCHS" --threads "$WORKERS" \
        --lr "$LR" --wd 0.0001 --batch 256 --z-spec hp ${SOFT_ARG[@]+"${SOFT_ARG[@]}"} \
        --pi-weight "$PI_WEIGHT" --loss-chunk "$LOSS_CHUNK" --device cuda \
        --record 1 --record-dir data/runs \
        --init "$CUR" \
        >"$LOGD/train_$r.log" 2>&1 || die "[$r] 训练失败 ⇒ 见 train_$r.log"
    CAND="$(grep -oE '已保存 [^ ]+' "$LOGD/train_$r.log" | tail -1 | cut -d' ' -f2)"
    [ -n "$CAND" ] || die "[$r] 解析不到权重路径（训练可能成功但日志格式变了）⇒ 见 train_$r.log"
    say "[$r] 候选权重 $CAND"
    # 权重漂移（候选 vs 续训起点）：门控差为零时，用它区分「没退化」与「根本没动」。
    DW="$(grep -oE '权重漂移 [0-9.eE+-]+' "$LOGD/train_$r.log" | tail -1 | cut -d' ' -f2)"
    say "[$r] 权重漂移 ${DW:-未报告}"
    CUR="$CAND"        # 无条件继承：门控只决定 best，不决定网络是否继续训练

    # ---- 门控：候选 vs 当前 best（同 seed 配对，裸网络，encs 同分布） ----
    # `eval_gate.py` 只当触发器：任务表 / 造局面 / 搜索 / 统计全在 C++（`sts.eval_run`），
    # **同一批 `(遭遇, 层, seed)`** 喂给两个策略 ⇒ 胜率差就是策略差。种子必须落在 held-out 段
    # （`collect.EVAL_SEED0` = 1e8）。
    say "[$r] 门控：$GATE_N_PER 场/项（裸网络，encs，固定 seed 段）"
    GSPEC_C="net:$CAND"
    GSPEC_B="net:$BEST"
    "$PY" -u eval_gate.py "$GATE_N_PER" "$GSPEC_C,$GSPEC_B" "$GATE_SEED0" \
        "$LOGD/gate_$r.json" --games "$GAMES" \
        >"$LOGD/gate_$r.log" 2>&1 || die "[$r] 门控失败 ⇒ 见 gate_$r.log"
    tail -40 "$LOGD/gate_$r.log" >>"$LOGD/summary.txt"
    # 两行读数 → `胜率差|候选|best`（小数给 `awk` 比大小，百分比给人看）。
    WIN="$("$PY" - "$LOGD/gate_$r.json" "$GSPEC_C" "$GSPEC_B" <<'PYX'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
r = {row["spec"]: row for row in d["rows"]}
c, b = r[sys.argv[2]]["win_rate"], r[sys.argv[3]]["win_rate"]
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
    # `CUR` 已在上面无条件置为候选（门控只决定 best，不决定网络是否继续训练）。
    printf 'r=%s  cand=%s  win_diff=%s  dw=%s  cur=%s  best=%s\n' "$r" "$CAND" "$DIFF" "${DW:--}" "$CUR" "$BEST" >>"$LOGD/gate_history.txt"
done

say "自迭代结束：共 $((r - 1)) 轮，日志在 $LOGD"
