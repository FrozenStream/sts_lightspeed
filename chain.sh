#!/usr/bin/env bash
# 自迭代：自对弈采集 → 续训 → 门控，循环。**不带参数**，旋钮就是下面这些裸常量。
#
# 链：每轮的采集与训练起点都是 CUR（第 1 轮 = NET）⇒ 自对弈数据由当前网自己产生、训练只是它的
# 续训，候选**无条件**成为下一轮的 CUR。门控只拿候选与历史最佳 BEST 配对比，只决定 BEST 这个
# 参照，**不**影响数据流（这才是"一个网络从头训到尾"）。
#
# ⚠️ 产物一律落 data/（不写 /tmp：重启即丢）；每轮的分片目录与权重都留着，磁盘不自动回收。
# ⚠️ 任一步失败当场退出（保留已完成轮），绝不静默跳过 —— 否则会拿旧权重接着跑，看着一切正常。
# ⚠️ 训练集 = **本轮 + 最近 `REPLAY_ROUNDS` 轮**（回放，权重按轮龄几何衰减；`REPLAY_ROUNDS=0`
#    则只训本轮）。教师锚点仍是删掉的，要挂得自己加 `--mix`。步数锚在本轮上（见 `collect.CollectMix`）。
# ⚠️ 开局种子段**每轮换一段**（`collect.round_seed0`：确定性、互不重叠、恒 < `EVAL_SEED0`）——
#    从前每轮都从 `start_seed=0` 起，等于每轮都在同一批开局上自对弈。
# ⚠️ 产物名**不带配方**（2026-10-15）：默认 = `data/spencs`（采集）/ `data/<数据目录名>.pt`（权重），
#    配方写进产物的 `_recipe.json` / 存档的 `recipe` 字段。链条**显式**给每轮的名字
#    （`data/sp_<STAMP>_r<轮>`）—— 不给就会每轮撞进同一个目录，靠配方校验报错。
# ⚠️ 断点续跑看 `$LOGD/gate_history.txt` 末行（重启不从头来）；回放台账在 `$LOGD/replay.txt`、
#    种子段台账在 `$LOGD/seed_history.txt`（**同一 `STS_STAMP` 才能续**，否则是新目录）。
set -uo pipefail

RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$RDIR"

PY=/root/miniconda3/bin/python

# ── 旋钮（改这里；`./_start.sh` 会把 `STS_<旋钮>` 转发进来覆盖）──
NET=data/warm.pt    # 起步权重。**短名是刻意的**：训练产出的名字是整份配方（几百字符）
ROUNDS=0            # 0 = 一直迭代（停：`STS_SESSION=<会话名> bash _kill.sh`）；也给个上限跑小实验
DEVICE=cuda         # 采集/训练/门控共用；⚠️ 采集必须单进程（多份 CUDA 上下文踩断过容器）
ROLLOUT_THREADS=1   # 前向 intra-op 线程（进产物名）；⚠️ 只在 CPU 前向有用（16 核给 12 ⇒ 20.7 场/min，
                    #   vs 线程=1 时 <13），cuda 上只会跟树线程抢 CPU
N_PER=32            # 采集：每项场数（**任务表** = `encs` 表项数 × N_PER = 63 × N_PER 场）
                    #   ⚠️ 它同时是**种子段的宽度**（第 i 场用 `start_seed + i`）⇒ 改它 = 改
                    #   `round_seed0` 的段划分（那一轮的开局分布跟着变）。
SIMS=3000           # 采集：MCTS 预算（**完整档**；见 SIMS_CHEAP / SIMS_FULL_FRAC）
SIMS_CHEAP=0        # 采集：**便宜档**预算（0 = 关 = 恒 SIMS）。KataGo 的 randomized playout cap：
                    #   多数决策点跑便宜档、少数跑完整档 ⇒ 同墙钟下"量大 + 少量深标签"，而不是
                    #   全表同一预算（后者标签量虽大，形状单一）。⚠️ 关着时逐位同旧行为（不动 rng）。
SIMS_FULL_FRAC=0.25 # 采集：走完整档（SIMS）的决策点比例（仅 SIMS_CHEAP > 0 时生效）
SP_POOL=512         # 采集：**池宽** = 在飞的场数（跑完一场立刻补一场）。一棵树同时只挂 1 个在飞
                    #   前向请求 ⇒ **批宽 ≤ 池宽**。实测（受控探针：同任务表 2016 场 / sims=200 / workers=14 /
                    #   cuda）：池宽 512 ⇒ **202 s / 均批 79.3**；1024 ⇒ 214 s / 83.6；整表全开 2016 ⇒ 209 s /
                    #   84.2（旧形状）⇒ **512 最好且内存最小**（≈ 3 MB/棵 ⇒ 1.5 GB）。⚠️ 池宽**不是线程数**
                    #   （见 SP_WORKERS），也**不是任务表大小**（那个由 N_PER 定）。
SP_WORKERS=14       # 采集：worker 线程数（`collect_selfplay --workers`）。⚠️ 别用"一棵树一条线程"
                    #   （实测 2016 条时 CPU 15.7 核全花在惊群/调度上，GPU 只有 4~11%）。取核数减 2
                    #   （前向服务线程 + 主线程交付各占一个）。树不绑定线程：推一步就回池，谁抢到谁继续。
SP_BATCH_WAIT=0     # 采集：**攒批**（µs，`collect_selfplay --batch-wait-us`）。⚠️ 只在池子塌了
                    #   （在飞场数远小于 SP_POOL）时才该开：那时等一等能把批从十几行拉到 60+（实测
                    #   543→316 s）。池子正常时它是**白等**（池宽 512 等 6 ms → 235 s vs 不等 202 s）。
                    #   池子塌的根因见 `collect/selfplay.cpp` 的 `selfplayPooled`（树槽/交付槽要分开）。
SP_BATCH_TARGET=0   # 采集：攒批的"提前发车"阈值（0 = 不设限；攒够这个行数就不等剩余的了）。
SP_CPU_ROWS=10      # 采集：**尾段（窄批）走 CPU** 的分界（`--cpu-rows`；0 = 关）。一次前向的耗时
                    #   **几乎与批宽无关**（实测 GPU 批 1 → 0.98 ms、批 256 → 1.41 ms = kernel 派发
                    #   受限）⇒ 只剩几棵树在飞时每行贵 ~200×：实测一整轮 2411 s 里约 **1877 s** 花在
                    #   最后 16 场的 1~2 行调用上（GPU 只 40%、14 条 worker 87% 在睡）。CPU 批 1 只要
                    #   0.40 ms 且**多会话能横向扩**（1/4/8 会话 = 2201/7164/10083 行/s）⇒ 窄批交给它。
                    #   实测交叉点 ≈ 批 6（单会话）、4 会话下 ≈ 10 行。
SP_CPU_SESSIONS=4   # 采集：尾段 CPU 会话数（每个 = 一条服务线程 + 一份独立 ONNX 会话）。
EPOCHS=2            # 训练：轮数
LR=3e-4             # 续训步长；1e-3 对已收敛的网过大（会把权重从教师分布拽走）
WORKERS=32          # 训练线程
LOSS_CHUNK=16384    # 训练：损失分块行数（进权重名）；⚠️ 内存 ∝ hidden：512 + 65536 ⇒ 单块 16 GiB
PI_WEIGHT=none      # π 逐行权重：none 等权（**基线**）/ advantage = max(0, z−V̂) 归一。
                    #   ⚠️ `advantage` 在败局上常被整批滤掉（`z=-1` ⇒ 权重 0），难局的策略梯度
                    #   直接消失；等权的代价是"输局里也照抄教师"（历史实测两份数据各 −0.08/−0.30pp）
                    #   ⇒ 两种口径都有已知代价，**先跑等权基线**，再单独试温和权重（别一次改两件事）
REPLAY_ROUNDS=3     # 回放：训练集里挂**最近几轮**的自对弈产物（0 = 只训本轮）。动机：只训本轮
                    #   ⇒ 网络在本轮那批开局上过拟合、旧状态淡出（历史实测小份等权每轮 −1.41pp）。
                    #   ⚠️ 一轮的**优化步数不变**（行数锚在本轮上，见 `collect.CollectMix`）⇒ 挂
                    #   回放 ≠ 偷偷多训几步（否则"回放"与"更多训练量"分不开）
REPLAY_DECAY=0.6    # 回放：权重按轮龄**几何衰减**（最近一轮 = 本轮×0.6，再往前 ×0.6 递增）
                    #   ⇒ 3 轮 ≈ 50.9% / 30.5% / 18.5%（用户要的 50/30/20 口径）
SOFT_TARGET=1       # 策略目标：1 = 完整访问分布 π**(1/SOFT_TEMP)（AGZ 口径）；0 = 硬 argmax one-hot
SOFT_TEMP=1.0       # 只在 SOFT_TARGET=1 时生效
TEMP_MOVES=30       # 自对弈前 N 个决策点 τ=1，之后 τ→0
DIR_TOTAL=10.83     # 根噪声 Dirichlet **总浓度**（每动作 α = 总浓度/合法动作数；围棋 0.03×361）
DIR_EPS=0.1         # 根噪声混合权重 ε（动作少 ⇒ 比围棋的 0.25 小）
GATE_GAMES=512      # 门控：一次并肩几棵树（`eval_run` 自己的批宽；只改墙钟，与采集池宽无关）
GATE_N_PER=512      # 门控：每项场数（裸网络 = 不给 `:sims`，直接量网络本身）
GATE_MARGIN=0.01    # 门控：候选须领先的配对胜率差；⚠️ 别拿绝对胜率比固定阈值（常年 0.9+）；
                    #   场数下限由裕度反推（32 场/项 ≈ ±2pp > 阈值 ⇒ 判据落进噪声）
GATE_SEED0=100000000  # 门控固定 seed 段（= `collect.EVAL_SEED0`，held-out，训练数据绝不用）
STAMP="$(date +%m%d_%H%M)"  # 轮次目录名；`STS_STAMP` 可指定（复用已存在的轮次目录）

# `./_start.sh` 只转发 `STS_*`，而上面的旋钮名不带前缀 ⇒ 这一层接进来（空值 = 保持常量）。
for _k in N_PER SIMS SIMS_CHEAP SIMS_FULL_FRAC SP_POOL SP_WORKERS SP_BATCH_WAIT SP_BATCH_TARGET \
          SP_CPU_ROWS SP_CPU_SESSIONS \
          ROLLOUT_THREADS DEVICE EPOCHS LR WORKERS \
          LOSS_CHUNK PI_WEIGHT REPLAY_ROUNDS REPLAY_DECAY \
          SOFT_TARGET SOFT_TEMP TEMP_MOVES DIR_TOTAL DIR_EPS \
          GATE_GAMES GATE_N_PER GATE_MARGIN GATE_SEED0 STAMP ROUNDS; do
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
# 取日志里 `<标签> <值>` 的值：**默认 FS**（空白）⇒ 标签是单独一个字段，值 = 下一个字段；
# 值只会是路径或数字（可打印 ASCII）⇒ 从第一个非 ASCII 字节起就是说明文字，截掉。
# ⚠️ **别把全角括号放进 `-F` 的字符类**（旧写法 `-F'[[:space:]（）()]'`）：mawk 在没有 locale 的
#    环境里按**字节**解释字符类，而 `（`/`）` 的 UTF-8 字节（EF BC 88 / EF BC 89）会命中标签自己
#    的字节（`物` = E7 **89** A9）⇒ 标签被劈成两段、值取不到（恒返回空）。实测：这台机 `LANG` 是空的
#    ⇒ 旧写法**每轮都在第一道解析上死**（`data/logs/chain_*/` 里只有 `collect_1.log`），
#    链条从没跑完整过一轮。判据：`awk -v k=产物目录 '...' collect_1.log` 必须回出路径。
pick() { awk -v k="$1" \
    '{for (i = 1; i < NF; i++) if ($i == k) v = $(i + 1)} END {gsub(/[^ -~].*/, "", v); print v}' "$2"; }

# 台账写入：`<键> <值>`，**同一个键只留一行**（键升序追加）。
# ⚠️ 为什么不能直接 `>>`：一轮失败重跑（同 `STS_STAMP`）会给同一轮写两行 ⇒ 回放台账里同一份数据
#    出现两次（**权重被悄悄加倍**、占比就不是配方了），种子台账也会出现两个"第 3 轮"。
ledger() {                                    # ledger <文件> <键> <值>
    { grep -v "^$2 " "$1" 2>/dev/null || true; printf '%s %s\n' "$2" "$3"; } >"$1.tmp" && mv "$1.tmp" "$1"
}

[ -f "$NET" ] || die "起步权重不存在：$NET（新实例上先蒸馏一个，或从别处拷成这个名字）"
BUDGET="MCTS($SIMS)"
if [ "$SIMS_CHEAP" -gt 0 ]; then     # 只给日志看：两档 + 比例（浮点算术交给 awk）
    BUDGET="$(awk -v c="$SIMS_CHEAP" -v s="$SIMS" -v f="$SIMS_FULL_FRAC" \
        'BEGIN{printf "MCTS(%d×%.0f%% + %d×%.0f%%)", c, 100 * (1 - f), s, 100 * f}')"
fi
say "起点 $NET｜设备 $DEVICE（前向 $ROLLOUT_THREADS 线程）｜采集 $N_PER 场/项 × $BUDGET × 池宽 $SP_POOL / $SP_WORKERS 线程 / 攒批 $SP_BATCH_WAITµs / 尾段 CPU $SP_CPU_SESSIONS 会话(≤$SP_CPU_ROWS 行)"
say "训练 $EPOCHS ep lr=$LR 目标 $SOFT_KIND π权重=$PI_WEIGHT 回放=$REPLAY_ROUNDS 轮(衰减 $REPLAY_DECAY)｜门控 $GATE_N_PER 场/项 裸网络｜日志 $LOGD"

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
    # ── 开局种子段：**每轮换一段**（确定性、互不重叠、恒 < EVAL_SEED0）────────────────────
    # 从前每轮都传 `--start-seed 0` ⇒ 每项恒用 seed 0..N_PER-1，**每轮都在同一批开局上过拟合**。
    # 段号 = `collect.round_seed0(轮号, N_PER)` 的纯函数 ⇒ 断点续跑不必存状态；台账只是留痕
    # （改了 `N_PER` 之后能反查那一轮实际用的是哪一段）。
    SEED0="$("$PY" -c 'import sys; from collect import round_seed0; print(round_seed0(int(sys.argv[1]), int(sys.argv[2])))' \
        "$r" "$N_PER")" || die "[$r] 算不出轮换种子段（collect.round_seed0）"
    ledger "$LOGD/seed_history.txt" "r=$r" "seed0=$SEED0 n_per=$N_PER"
    say "===== 第 $r 轮（续训=${CUR##*/}  参照 best=${BEST##*/}  开局段 seed0=$SEED0） ====="

    # 本轮产物的**名字**（不带配方；只用来把"这一轮/这次运行"分开）。产物名是固定的
    # （`<cache-dir>/spencs`）⇒ 不给 `--cache` 就会每轮撞进同一个目录、靠 `_recipe.json` 报错。
    SP_STEM="sp_${STAMP}_r${r}"

    "$PY" -u collect_selfplay.py --n "$N_PER" --sims "$SIMS" --net-path "$CUR" \
        --start-seed "$SEED0" \
        --sims-cheap "$SIMS_CHEAP" --sims-full-frac "$SIMS_FULL_FRAC" \
        --cache "data/$SP_STEM" --pool "$SP_POOL" --workers "$SP_WORKERS" \
        --batch-wait-us "$SP_BATCH_WAIT" --batch-target "$SP_BATCH_TARGET" \
        --cpu-rows "$SP_CPU_ROWS" --cpu-sessions "$SP_CPU_SESSIONS" \
        --rollout-threads "$ROLLOUT_THREADS" --device "$DEVICE" \
        --temp-moves "$TEMP_MOVES" --dirichlet-total "$DIR_TOTAL" --dirichlet-eps "$DIR_EPS" \
        >"$LOGD/collect_$r.log" 2>&1 || die "[$r] 采集失败 ⇒ collect_$r.log"
    DATA="$(pick 产物目录 "$LOGD/collect_$r.log")"
    [ -n "$DATA" ] || die "[$r] 解析不到产物目录（采集可能成功但日志格式变了）⇒ collect_$r.log"
    # 回放台账：一行一轮（`<轮号> <产物目录>`）。断点续跑靠它恢复"哪一轮用了哪份数据"。
    ledger "$LOGD/replay.txt" "$r" "$DATA"
    W="$(grep -aoE '⚠️.*' "$LOGD/collect_$r.log" | tail -1)"   # 采集侧的告警抬到链条日志
    say "[$r] 数据 $DATA${W:+  $W}"

    # ── 回放：挂最近 `REPLAY_ROUNDS` 轮的产物（权重按轮龄几何衰减，相对本轮 1.0）──────────────
    # ⚠️ 数据**不见了就当场死**：少一份 ≠ 少一点数据，而是**另一个配方**（静默降级最难查）。
    # ⚠️ 续跑时若台账缺行（比如只留了 `gate_history.txt`）也当场死 —— 让用户显式选 `REPLAY_ROUNDS=0`
    #    或补齐台账，而不是悄悄换成"只训本轮"。
    MIX=(); MIX_SAY=""
    if [ "$REPLAY_ROUNDS" -gt 0 ]; then
        while read -r _w _d; do
            [ -n "$_d" ] || continue
            [ -d "$_d" ] || die "[$r] 回放数据不存在：$_d（台账 $LOGD/replay.txt）"
            MIX+=(--mix "$_d=$_w")
            MIX_SAY="$MIX_SAY $_w@${_d##*/}"
        done < <(awk -v r="$r" -v k="$REPLAY_ROUNDS" -v dec="$REPLAY_DECAY" \
            '$1 < r { nl[n++] = $2 }
             END { m = (n < k ? n : k)
                   for (j = 0; j < m; j++) printf "%.4g %s\n", dec ^ (j + 1), nl[n - 1 - j] }' \
            "$LOGD/replay.txt")
        if [ "$r" -gt 1 ] && [ "$MIX_SAY" = "" ]; then
            die "[$r] REPLAY_ROUNDS=$REPLAY_ROUNDS 但回放台账里没有本轮之前的行 ⇒ $LOGD/replay.txt（续跑丢了台账？显式设 REPLAY_ROUNDS=0 或补齐）"
        fi
    fi
    [ -n "$MIX_SAY" ] && say "[$r] 回放$MIX_SAY"

    "$PY" -u train.py train "$DATA" "$EPOCHS" "data/$SP_STEM.pt" \
        --threads "$WORKERS" --lr "$LR" --wd 0.0001 \
        --batch 256 --z-spec hp $SOFT_ARG --pi-weight "$PI_WEIGHT" --loss-chunk "$LOSS_CHUNK" \
        --device "$DEVICE" --record 1 --record-dir data/runs --init "$CUR" \
        "${MIX[@]+"${MIX[@]}"}" \
        >"$LOGD/train_$r.log" 2>&1 || die "[$r] 训练失败 ⇒ train_$r.log"
    CAND="$(pick 已保存 "$LOGD/train_$r.log")"
    [ -n "$CAND" ] || die "[$r] 解析不到权重路径（训练可能成功但日志格式变了）⇒ train_$r.log"
    DW="$(pick 权重漂移 "$LOGD/train_$r.log")"   # 权重漂移（相对续训起点）
    CUR="$CAND"     # 无条件继承；门控只决定 best

    GSPEC_C="net:$CAND"; GSPEC_B="net:$BEST"
    "$PY" -u eval_gate.py "$GATE_N_PER" "$GSPEC_C,$GSPEC_B" "$GATE_SEED0" \
        "$LOGD/gate_$r.json" --games "$GATE_GAMES" --device "$DEVICE" \
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
    printf '第 %s 轮  候选 %s vs best %s  差 %s  漂移 %s  seed0 %s  回放 %s  %s\n' \
        "$r" "$DCAND" "$DBEST" "$DDIFF" "${DW:-未报告}" "$SEED0" "${REPLAY_ROUNDS}轮${MIX_SAY}" "$VERDICT" \
        >>"$LOGD/summary.txt"
done

say "自迭代结束：共 $((r - 1)) 轮，日志在 $LOGD"
