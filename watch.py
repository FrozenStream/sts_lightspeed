"""作业看门狗：采样日志状态与进程占用，写出 JSON 状态文件（不依赖任何本地会话）。

    watch.py <名字>=<日志> … [--idle 1800] [--out data/watch.json] [--pat <pgrep 模式>]

每次采样记录：
  - 日志静默秒数；退出码 = 日志尾部 `COMMAND_EXIT_CODE="N"` 的数值（无标记为 null）
  - `--pat` 命中的进程数与 RSS 合计
  - flag：`ok` / `stale?`（静默 ≥ `--idle` 且进程仍在 ⇒ 疑似死锁）/ `done`

说明：
  - `rss_sum_mb` 对共享页重复计数（COW 子进程各算一份），仅作趋势指标。
  - `STS_WATCH_KILL=1` 时对 `stale?` 作业执行 `pkill -9 -f <pat>`；默认只报告。
  - 采样含 `sleep`：用 `./sync.sh start watch.py …` 启动，不要放进同步终端。
"""
import json
import os
import re
import subprocess
import sys
import time

DEFAULT_IDLE = 1800                                      # 秒
_CODE = re.compile(r'COMMAND_EXIT_CODE="(\d+)"')


def _count(pat):
    """`--pat` 命中的进程数。"""
    if not pat:
        return None
    r = subprocess.run(["pgrep", "-c", "-f", pat], capture_output=True, text=True)
    try:
        return int(r.stdout.strip() or 0)
    except ValueError:
        return 0


def _rss_sum(pat):
    """`--pat` 命中进程的 RSS 合计（MB）；共享页会重复计入。"""
    if not pat:
        return None
    r = subprocess.run(["ps", "-eo", "rss,args"], capture_output=True, text=True)
    tot = 0
    for line in r.stdout.splitlines():
        if pat in line:
            head = line.strip().split(None, 1)[0]
            if head.isdigit():
                tot += int(head)
    return tot // 1024


def _code(log):
    """日志尾部的退出码；未写标记时返回 None。"""
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            m = _CODE.findall(f.read())
        return m[-1] if m else None
    except OSError:
        return None


def main():
    args, jobs, idle, out, pat = sys.argv[1:], [], DEFAULT_IDLE, "data/watch.json", ""
    i = 0
    while i < len(args):
        if args[i] == "--idle":
            idle, i = int(args[i + 1]), i + 2
        elif args[i] == "--out":
            out, i = args[i + 1], i + 2
        elif args[i] == "--pat":
            pat, i = args[i + 1], i + 2
        else:
            name, _, log = args[i].partition("=")
            jobs.append((name, log or f"{name}.log"))
            i += 1
    if not jobs:
        raise SystemExit(__doc__)
    kill = os.environ.get("STS_WATCH_KILL", "0") not in ("0", "", "off", "false")

    state, spin = {}, set()
    while True:
        now = time.time()
        for name, log in jobs:
            try:
                age = int(now - os.path.getmtime(log))
            except OSError:
                age = -1                                  # 日志尚未创建
            code = _code(log)
            done = code is not None
            prev = state.get(name, {})
            flag = "done" if done else ("stale?" if age >= idle else "ok")
            state[name] = dict(log=log, silent_s=age, code=code, done=done,
                               ok=(code == "0"), flag=flag,
                               stale_s=prev.get("stale_s", 0) + (30 if flag == "stale?" else 0))
            if flag == "stale?" and name not in spin:
                spin.add(name)
                print(f"⚠️ {name} 静默 {age}s（> {idle}s）—— 疑似死锁", flush=True)
            if done and prev.get("code") != code:
                bad = "" if code == "0" else "  ⚠️ 非 0（137 = 外部 SIGKILL，见 experiment-pitfalls.md）"
                print(f"📄 {name} 结束：退出码 {code}{bad}", flush=True)
        n = _count(pat)
        rss = _rss_sum(pat)
        payload = dict(ts=now, pat=pat, n_proc=n, rss_sum_mb=rss, idle_limit=idle, jobs=state)
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=1)
        alive = [k for k, v in state.items() if not v["done"]]
        bad = [f"{k}:{v['code']}" for k, v in state.items() if v["done"] and v["code"] != "0"]
        print(f"[{time.strftime('%H:%M:%S')}] 未完 {len(alive)}/{len(jobs)}"
              f"  进程={n if n is not None else '-'}  RSS合计={rss}MB"
              f"  {('非零退出码 ' + ','.join(bad)) if bad else ''}", flush=True)
        if not alive:
            ok = [k for k, v in state.items() if v["code"] == "0"]
            print(f"✅ 全部结束：正常 {len(ok)}/{len(jobs)}；非零 {bad or '无'}", flush=True)
            return 0
        if kill and spin:
            subprocess.run(["pkill", "-9", "-f", pat])
            print(f"⚠️ STS_WATCH_KILL=1 ⇒ 已杀 {sorted(spin)}", flush=True)
            return 3
        time.sleep(30)


if __name__ == "__main__":
    raise SystemExit(main())
