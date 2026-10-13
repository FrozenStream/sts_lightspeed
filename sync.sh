#!/usr/bin/env bash
# 本地 → 远端（seetacloud）同步 + 作业控制 —— **薄分发器**。
#
#   ./sync.sh                          比较两端 md5（默认）      = ./_check.sh
#   ./sync.sh push [文件...]           推送受管文件              = ./_push.sh
#   ./sync.sh start <脚本.py> [参数...] 远端 tmux 会话里启动      = ./_start.sh
#   ./sync.sh attach | kill            接入 / 终止会话           = ./_attach.sh / ./_kill.sh
#   ./sync.sh status                   会话 / 进程 / 日志一览     = ./_status.sh
#   ./sync.sh log [n]                  最新日志尾部              = ./_log.sh
#   ./sync.sh wait <日志名> [间隔] [轮数]  静默完成触发器         = ./_wait.sh
#
# 每个子命令都已拆成 `=` 右侧的独立脚本（各自自足：自带身份 / SSH 头，不 source 公共库），
# 可以直接单独调用；本文件只是保留 `./sync.sh <子命令>` 这种老写法
# （以及各处文档里的引用）。

set -euo pipefail
RDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "${1:-}" in
  '')            exec "$RDIR/_check.sh" ;;
  push)   shift; exec "$RDIR/_push.sh" "$@" ;;
  start)  shift; exec "$RDIR/_start.sh" "$@" ;;
  attach) shift; exec "$RDIR/_attach.sh" "$@" ;;
  kill)   shift; exec "$RDIR/_kill.sh" "$@" ;;
  wait)   shift; exec "$RDIR/_wait.sh" "$@" ;;
  log)    shift; exec "$RDIR/_log.sh" "$@" ;;
  status) shift; exec "$RDIR/_status.sh" "$@" ;;
  usage|help|-h|--help) sed -n '2,/^$/p' "$0" ;;
  *) echo "未知子命令: $1" >&2; sed -n '2,/^$/p' "$0"; exit 2 ;;
esac
