#!/usr/bin/env bash
# 训练效果监控：循环刷新 `_progress.py`（曲线要点 / 门控逐轮 / 逐怪群退化）+ 当前阶段与进度。
#
#   _monitor.sh [刷新秒数]        默认 30；Ctrl-C 退出
#
# 读的都是落盘产物（`data/runs/*/seed0/points.jsonl`、`data/logs/chain_*/`），只读不写。
# 本地看：`ssh -tt <host> "cd <repo> && bash ./_monitor.sh 30"`
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
PY="${STS_PY:-/root/miniconda3/bin/python}"
REFRESH="${1:-30}"

stage_of() {   # 传日志文件模式；只取**最新**那份（多轮后 glob 命中多个），打最后一行
    f="$(ls -t $1 2>/dev/null | head -1)"
    # 进度条用 \r 原地刷新 ⇒ 先按 \r 切开再取最后一条非空行，否则会带一堆残渣
    [ -n "$f" ] && tail -c 4000 "$f" | tr '\r' '\n' | grep -v '^[[:space:]]*$' | tail -1
}

trim() {       # 日志里全是全路径 + 150 字符的 npz/权重名，屏幕上放不下 ⇒ 抹前缀、压短名
    sed -e 's#/root/autodl-tmp/sts_lightspeed/##g' -e 's#data/logs/[^/ ]*/##g' \
        -e 's#\(it[0-9][^ ]\{18\}\)[^ ]*\.npz#\1….npz#g' \
        -e 's#\(train_[0-9]*\)[^ ]*\.pt#\1.pt#g'
}

live_log() {   # 传进程短名 → 该阶段**本轮**的日志模式（空 = 这个阶段没在跑）
    # 模式整体当一个参数回传，**不能**让 shell 提前展开（否则只会拿到第 1 轮的文件）
    case "$1" in
        collect_selfplay.py) echo "$D/collect_*.log" ;;
        "train.py train")    echo "$D/train_*.log" ;;
        eval_encs.py)        echo "$D/gate_*.log" ;;
    esac
}

while :; do
    # 2J=清屏 3J=连滚动缓冲一起清（否则帧帧堆叠，看不到当前进度）
    printf '\033[2J\033[3J\033[H%s   每 %ss 刷新（Ctrl-C 退出）\n' "$(date '+%m-%d %H:%M:%S')" "$REFRESH"

    D="$(ls -td data/logs/chain_* 2>/dev/null | head -1)"
    if [ -n "$D" ]; then
        LIVE="$(ps -eo args= | grep -oE 'collect_selfplay\.py|train\.py train|eval_encs\.py' | head -1)"
        LG="$(live_log "$LIVE")"
        # 进度只取**在跑**那个阶段的末行（采集时即 tqdm 进度条）；其余阶段不占屏
        printf '\n== %s · 已完成 %s 轮门控 · 采集 %s 轮 ==\n' "${D##*/}" \
            "$(ls "$D"/gate_*.json 2>/dev/null | wc -l)" "$(ls "$D"/collect_*.log 2>/dev/null | wc -l)"
        # 配方（含批数）：直接读链条自己写的 banner —— 旋钮都在 `_chain.sh`，监控不猜也不抄。
        # 锚点用 ASCII 的 `MCTS(`：中文括号在 C locale 下会被当字节集，匹配不可靠。
        printf '  配方 : %s\n' "$(sed -n 's/^[0-9][0-9]:[0-9][0-9]:[0-9][0-9][[:space:]]*//p' \
            "$D/watchdog.log" 2>/dev/null | grep -m1 'MCTS(' | sed 's/  .*//')"
        printf '  在跑 : %s\n' "${LIVE:-（空档：交接/收尾中）}"
        printf '  进度 : %s\n' "$([ -n "$LG" ] && stage_of "$LG" | trim)"
        case "$LIVE" in
            collect_selfplay.py) printf '  上一步: %s\n' "$(stage_of "$D/gate_*.log" | trim)" ;;
            "train.py train")    printf '  上一步: %s\n' "$(stage_of "$D/collect_*.log" | trim)" ;;
            eval_encs.py)        printf '  上一步: %s\n' "$(stage_of "$D/train_*.log" | trim)" ;;
        esac
    fi

    echo
    "$PY" _progress.py --brief
    # 正式折线图：每帧覆盖同一张 PNG（不留档），本地拉下来在编辑器里看
    "$PY" _plot.py "$D" ./data/plots/strength.png >/dev/null 2>&1 || true
    printf '  折线图: ./data/plots/strength.png\n'
    df -h /root/autodl-tmp | tail -1 | sed 's/^/磁盘 : /'
    sleep "$REFRESH"
done
