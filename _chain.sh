#!/usr/bin/env bash
# 自迭代：自对弈采集 → 续训 → 门控，循环。**不带参数**，旋钮就是下面这些裸常量。
#
# 链：每轮的采集与训练起点都是 CUR（第 1 轮 = NET）⇒ 自对弈数据由当前网自己产生、训练只是它的
# 续训，候选**无条件**成为下一轮的 CUR。门控只拿候选与历史最佳 BEST 配对比，只决定 BEST 这个
# 参照，**不**影响数据流（这才是"一个网络从头训到尾"）。
#
# ⚠️ 产物一律落 data/（不写 /tmp：重启即丢）；每轮的分片目录与权重都留着，磁盘不自动回收。
# ⚠️ 任一步失败当场退出（保留已完成轮），绝不静默跳过 —— 否则会拿旧权重接着跑，看着一切正常。
# ⚠️ 训练集 = **本轮**数据（回放缓冲与教师锚点已删；要拼接多轮得单独接，别顺手加回来）。
# ⚠️ 断点续跑看 `$LOGD/gate_history.txt` 末行（重启不从头来）。
set -uo pipefail

RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RDIR"

PY=/root/miniconda3/bin/python

# ── 旋钮（改这里；`./_start.sh` 会把 `STS_<旋钮>` 转发进来覆盖）──
NET=data/warm.pt    # 起步权重。**短名是刻意的**：训练产出的名字是整份配方（几百字符）
ROUNDS=0            # 0 = 一直迭代（停：`STS_SESSION=<会话名> bash _kill.sh`）
DEVICE=cuda         # 采集/训练/门控共用；⚠️ 采集必须单进程（多份 CUDA 上下文踩断过容器）
ROLLOUT_THREADS=1   # 前向 intra-op 线程（进产物名）；⚠️ 只在 CPU 前向有用（16 核给 12 ⇒ 20.7 场/min，
                    #   vs 线程=1 时 <13），cuda 上只会跟树线程抢 CPU
N_PER=32            # 采集：每项场数（总场数 = ENCS 表项数 × N_PER）
SIMS=3000           # 采集：MCTS 预算
GAMES=2016          # 采集：在飞的树数 = 前向批宽（**不是线程数**）。Trees 由下面的 SP_WORKERS 条
                    #   线程分担（每线程持 GAMES/SP_WORKERS 棵并肩：每轮各降 1 条路径凑成一批）。
                    #   ⚠️ 取最大值 = 一轮场数（63 × N_PER）⇒ 整轮一个块。实测（探针 sims=200，同批任务）：
                    #   cuda K=2016 W=14（每线程 144 棵）0.122 s/局 < 旧形状（2016 条线程）0.145；
                    #   cpu(12 线程) 反过来窄批更快（K=504 0.206），因为 intra-op 线程要和树线程抢同一个
                    #   16 核 cgroup。内存 ≈ 7.5 MB/棵 ⇒ 2016 棵 ≈ 15 GB（本机 503 GB）。
SP_WORKERS=14       # 采集：worker 线程数（`collect_selfplay --workers`）。⚠️ 别用"一棵树一条线程"
                    #   （实测 2016 条时 CPU 15.7 核全花在惊群/调度上，GPU 只有 4~11%）。取核数减 2
                    #   （前向服务线程 + 主线程交付各占一个）。
EPOCHS=2            # 训练：轮数
LR=3e-4             # 续训步长；1e-3 对已收敛的网过大（会把权重从教师分布拽走）
WORKERS=32          # 训练线程
LOSS_CHUNK=16384    # 训练：损失分块行数（进权重名）；⚠️ 内存 ∝ hidden：512 + 65536 ⇒ 单块 16 GiB
PI_WEIGHT=advantage # π 逐行权重：none 等权 / advantage = max(0, z−V̂) 归一；等权时"在输局里也照抄
                    #   教师"是负作用（实测两份数据各 −0.08 / −0.30pp）
SOFT_TARGET=1       # 策略目标：1 = 完整访问分布 π**(1/SOFT_TEMP)（AGZ 口径）；0 = 硬 argmax one-hot
SOFT_TEMP=1.0       # 只在 SOFT_TARGET=1 时生效
TEMP_MOVES=30       # 自对弈前 N 个决策点 τ=1，之后 τ→0
DIR_TOTAL=10.83     # 根噪声 Dirichlet **总浓度**（每动作 α = 总浓度/合法动作数；围棋 0.03×361）
DIR_EPS=0.1         # 根噪声混合权重 ε（动作少 ⇒ 比围棋的 0.25 小）
GATE_N_PER=512      # 门控：每项场数（裸网络 = 不给 `:sims`，直接量网络本身）
GATE_MARGIN=0.01    # 门控：候选须领先的配对胜率差；⚠️ 别拿绝对胜率比固定阈值（常年 0.9+）；
                    #   场数下限由裕度反推（32 场/项 ≈ ±2pp > 阈值 ⇒ 判据落进噪声）
GATE_SEED0=100000000  # 门控固定 seed 段（= `collect.EVAL_SEED0`，held-out，训练数据绝不用）
STAMP="$(date +%m%d_%H%M)"  # 轮次目录名；`STS_STAMP` 可指定（复用已存在的轮次目录）

# `./_start.sh` 只转发 `STS_*`，而上面的旋钮名不带前缀 ⇒ 这一层接进来（空值 = 保持常量）。
for _k in N_PER SIMS GAMES SP_WORKERS ROLLOUT_THREADS DEVICE EPOCHS LR WORKERS LOSS_CHUNK PI_WEIGHT \
          SOFT_TARGET SOFT_TEMP TEMP_MOVES DIR_TOTAL DIR_EPS \
          GATE_N_PER GATE_MARGIN GATE_SEED0 STAMP; do
    _v="STS_$_k"
    [ -n "${!_v:-}" ] && printf -v "$_k" '%s' "${!_v}"
done
unset _k _v

SOFT_ARG=""; SOFT_KIND="硬 argmax"
if [ "$SOFT_TARGET" = 1 ]; then
    SOFT_ARG="--soft-target --soft-temp $SOFT_TEMP"; SOFT_KIND="软π**(1/$SOFT_TEMP)"
fi

LOGD="data/logs/chain_${STAMP}"
mkdir -p "$LOGD"
say() { printf '%s %s\n' "$(date +%T)" "$*"; }
die() { say "$*"; exit 1; }
# 取日志里 `<标签> <值>` 的值：值读到空格或全角/半角括号为止（值只会是路径或数字）。
pick() { awk -F'[[:space:]（）()]' -v k="$1" \
    '{for (i = 1; i < NF; i++) if ($i == k) v = $(i + 1)} END {print v}' "$2"; }

[ -f "$NET" ] || die "起步权重不存在：$NET（新实例上先蒸馏一个，或从别处拷成这个名字）"
say "起点 $NET｜设备 $DEVICE（前向 $ROLLOUT_THREADS 线程）｜采集 $N_PER 场/项 × MCTS($SIMS) × 并肩 $GAMES 棵 / $SP_WORKERS 线程"
say "训练 $EPOCHS ep lr=$LR 目标 $SOFT_KIND｜门控 $GATE_N_PER 场/项 裸网络｜日志 $LOGD"

CUR="$NET"      # 连续链：采集与训练都用它
BEST="$NET"     # 只做参照：门控领先才升格
r=0

# 断点续跑：末行 `r=<轮> cur=<权重> best=<权重>`
if [ -s "$LOGD/gate_history.txt" ]; then
    read -r _r _c _b < <(tail -1 "$LOGD/gate_history.txt")
    r="${_r#r=}"; CUR="${_c#cur=}"; BEST="${_b#best=}"
    case "$r" in ''|*[!0-9]*) die "断点续跑：末行读不出轮号 ⇒ $(tail -1 "$LOGD/gate_history.txt")" ;; esac
    [ -f "$CUR" ] && [ -f "$BEST" ] || die "断点续跑：权重不存在 ⇒ cur=$CUR best=$BEST"
    say "断点续跑：已完成 $r 轮，best=${BEST##*/} 续训=${CUR##*/}"
fi

while :; do
    r=$((r + 1))
    [ "$ROUNDS" -gt 0 ] && [ "$r" -gt "$ROUNDS" ] && break
    say "===== 第 $r 轮（续训=${CUR##*/}  参照 best=${BEST##*/}） ====="

    "$PY" -u collect_selfplay.py --n "$N_PER" --sims "$SIMS" --net-path "$CUR" \
        --cache-dir data --name "it${r}_${STAMP}" --games "$GAMES" --workers "$SP_WORKERS" \
        --rollout-threads "$ROLLOUT_THREADS" --device "$DEVICE" \
        --temp-moves "$TEMP_MOVES" --dirichlet-total "$DIR_TOTAL" --dirichlet-eps "$DIR_EPS" \
        >"$LOGD/collect_$r.log" 2>&1 || die "[$r] 采集失败 ⇒ collect_$r.log"
    DATA="$(pick 产物目录 "$LOGD/collect_$r.log")"
    [ -n "$DATA" ] || die "[$r] 解析不到产物目录（采集可能成功但日志格式变了）⇒ collect_$r.log"
    W="$(grep -aoE '⚠️.*' "$LOGD/collect_$r.log" | tail -1)"   # 采集侧的告警抬到链条日志
    say "[$r] 数据 $DATA${W:+  $W}"

    "$PY" -u train.py train "$DATA" "$EPOCHS" --threads "$WORKERS" --lr "$LR" --wd 0.0001 \
        --batch 256 --z-spec hp $SOFT_ARG --pi-weight "$PI_WEIGHT" --loss-chunk "$LOSS_CHUNK" \
        --device "$DEVICE" --record 1 --record-dir data/runs --init "$CUR" \
        >"$LOGD/train_$r.log" 2>&1 || die "[$r] 训练失败 ⇒ train_$r.log"
    CAND="$(pick 已保存 "$LOGD/train_$r.log")"
    [ -n "$CAND" ] || die "[$r] 解析不到权重路径（训练可能成功但日志格式变了）⇒ train_$r.log"
    DW="$(pick 权重漂移 "$LOGD/train_$r.log")"   # 权重漂移（相对续训起点）
    CUR="$CAND"     # 无条件继承；门控只决定 best

    GSPEC_C="net:$CAND"; GSPEC_B="net:$BEST"
    "$PY" -u eval_gate.py "$GATE_N_PER" "$GSPEC_C,$GSPEC_B" "$GATE_SEED0" \
        "$LOGD/gate_$r.json" --games "$GAMES" --device "$DEVICE" \
        >"$LOGD/gate_$r.log" 2>&1 || die "[$r] 门控失败 ⇒ gate_$r.log"
    # 同一批 `(遭遇, 层, seed)` 喂给两个策略 ⇒ 胜率差就是策略差（任务表/造局面/搜索全在 C++）。
    read -r DIFF DCAND DBEST DDIFF < <("$PY" - "$LOGD/gate_$r.json" "$GSPEC_C" "$GSPEC_B" <<'PYX'
import json, sys
w = {row["spec"]: row["win_rate"] for row in json.load(open(sys.argv[1], encoding="utf-8"))["rows"]}
c, b = (w[s] for s in sys.argv[2:4])
print(f"{c - b:.6f} {c:.1%} {b:.1%} {c - b:+.1%}")   # 小数给 awk 比大小，百分比给人看
PYX
    ) || die "[$r] 门控结果解析失败 ⇒ gate_$r.json"

    if awk -v d="$DIFF" -v t="$GATE_MARGIN" 'BEGIN{exit !(d >= t)}'; then
        BEST="$CAND"; VERDICT="✅ 采纳（best 更新）"
    else
        VERDICT="❌ 未领先（阈值 $GATE_MARGIN，best 不变）"
    fi
    say "[$r] 候选 $DCAND vs best $DBEST  差 $DDIFF  $VERDICT"
    printf 'r=%s cur=%s best=%s\n' "$r" "$CUR" "$BEST" >>"$LOGD/gate_history.txt"
    printf '第 %s 轮  候选 %s vs best %s  差 %s  漂移 %s  %s\n' \
        "$r" "$DCAND" "$DBEST" "$DDIFF" "${DW:-未报告}" "$VERDICT" >>"$LOGD/summary.txt"
done

say "自迭代结束：共 $((r - 1)) 轮，日志在 $LOGD"
