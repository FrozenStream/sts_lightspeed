#!/usr/bin/env bash
# 等一批后台作业全部退出，然后把结果行落盘到 stdout。
#
# 为什么等待要在**远端**跑：本地 ssh 长连接十几分钟就会被对端 reset
#（2026-09-21 实测：14 分钟后 `Connection reset by peer`，退出码 255）。
# 放在远端 + 本地只轮询文件 ⇒ 断线只是重连，不影响等待本身。
#
# 用法（远端），参数 = **日志 glob**（默认 run.ab_e32_s*.log）：
#   cd /root/autodl-tmp/sts_lightspeed && setsid nohup bash wait_logs.sh 'run.ab_dt2.log' \
#       > ab_dt2_results.txt 2>&1 < /dev/null &
# 然后本地：cat ab_dt2_results.txt
#
# ⚠️ 必须登记进 `sync.sh` 的 `FILES`，否则 `push` 不会送它过去
#    （2026-09-21 踩过：远端跑旧版 ⇒ “等 ab_dt” 打印出 ab_e32 的结果，**静默交叉**）。
set -u
cd "$(dirname "$0")"
PAT="${1:-run.ab_e32_s*.log}"

# ⚠️ 锚定 `^/root/miniconda3/bin/python`：只匹配 python 进程本身。
# 若只写 `train.py seeds`，那些 `bash -c`/`script` wrapper 也会命中 ——
# 而它们**生命周期和作业一样长**，等待就永远不结束。
while pgrep -f '^/root/miniconda3/bin/python -u train' >/dev/null 2>&1; do
  sleep 30
done

echo "=== 全部结束 $(date +%H:%M:%S)  匹配 $PAT ==="
for f in $PAT; do
  [ -f "$f" ] || continue
  echo "--- $f ---"
  grep -aE '丢弃撞步数上限|已保存|seed=|mean|std' "$f" | tail -8
  # ⚠️ 被外部 SIGKILL 的作业会**停在半路**，日志里只有 script -e 写的退出码 ⇒ 必须报出来
  grep -aoE 'COMMAND_EXIT_CODE="[0-9]+"' "$f" | tail -1 | sed 's/^/    退出: /'
done
echo "=== 最近写出的权重 ==="
ls -lat --time-style=+%m-%d_%H:%M data/*.pt 2>/dev/null | head -5 || echo "(没有 .pt —— 训练没走到存盘)"
