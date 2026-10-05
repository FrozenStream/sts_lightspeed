"""守卫总检：一条命令跑全部「永久不变量检查」（守卫的价值 = 被运行的概率）。

用途：把「永久不变量检查」从"躺在根目录等人想起"变成"跑一条命令"；退出码 = 失败个数。

原理：每个检查**各起一个子进程** —— 守卫可能撞上 C++ `assert(false)`（SIGABRT），
进程直接死、Python 抓不到；同进程跑第二个检查会连坐、且看不出崩的是哪个。

用法：
    python _chk_all.py                      # engine 档（秒级，不需缓存）
    python _chk_all.py --data data/x.npz    # 再加 data 档（需一份缓存）
    python _chk_all.py --list / -k mask     # 列清单 / 只跑名字含 mask 的

⚠️ 2026-09-30 退役 11 个守卫 —— 原文件都在 git 历史里（`git show HEAD:<路径>`）：
  · 10 个（整局 run 家族 + 已冗余的布局/选择/截断/回合）：`_chk_run` `_chk_runmap` `_chk_runnet`
    `_chk_rundata` `_chk_runtrain` `_chk_runpolicy` `_chk_snap` `_chk_select` `_chk_trunc` `_chk_turn`；
  · `realtest/_chk_comm.py`（真机桥接）：**长期红**（`comm_map.py` 的 `_FALLBACK` 停在旧布局
    ⇒ `KeyError: 'potion_bag_off'`），而修它要先定「真机 JSON 里拿不到的那 4 列怎么填」⇒ 退役。
    桥接代码**保留**（`realtest/`）；要单独跑：`python realtest/_chk_comm.py`。

⚠️ 2026-10-06 退役整局（run）线 —— `_chk_bossrelic.py` 连同 `runenv` `rundata` `runlayout`
  `runnet` `runpolicy` `collect_run` `train_run` `multi_hot` 一起删除（用户的裁决：过于过时）。
  这批文件**未被 git 跟踪** ⇒ 不在任何提交里，备份在仓库外 `d:/tmp/sts_run_series_backup_1006/`。
  `_chk_res.py` 只摘掉「全局侧逐槽」那一节（原第 0 节），其余 4 节守的是战斗侧 `azlayout`。
"""
import argparse
import os
import subprocess
import sys
import time

# ⚠️ 与 `runtime.py` 同一套兜底：输出重定向（`| tee log`）会让 stdout 退化成 GBK，
# 本文件打 ✅ —— 不加这行会崩在输出上，而不是报出真正的失败项。
for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

ENGINE, DATA = "engine", "data"

# (名字, argv 模板, 档位, 一句说明)
CHECKS = [
    ("资源块",        ["_chk_res.py"],                         ENGINE,
     "遗物多热池化 / 药水逐槽 id / 哪些列进网络 / 药水动作的 5×7 网格"),
    ("生死/复活",     ["_chk_revive.py"],                      ENGINE,
     "halfDead 把「待复活」从「尸体」里分出来 / monstersAlive·deckSize 逐帧自洽 / runCtx 已瘦身"),
    ("层数→状态",     ["_chk_shape.py"],                        ENGINE,
     "shape_floor：定点复现 / 层数⇒牌组遗物↑基础牌↓ / 两套形态 各 50% / floor·act 归调用方"
     " / 发放池里没有引擎表达不了的内容"),
    ("动作可编码",    ["_chk_actions.py"],                     ENGINE,
     "所有遭遇战的合法动作都能唯一编码进策略头"),
    ("掩码语义",      ["_chk_mask.py", "3", "200"],            ENGINE,
     "掩码 == 真实合法集（不是搜索树访问过的边）"),
    ("池化不变量",    ["_chk_pool.py"],                          ENGINE,
     "逐卡嵌入→mean→主网络 确实成立（含置换不变）"),
    ("推理前向",     ["_chk_infer.py"],                        ENGINE,
     "inference_net 就是 net 本身：前向确定性 + numpy/torch 两种输入都收 + 线程数落到 torch"),
    ("树复用",        ["_chk_search.py"],                      ENGINE,
     "SearchTree 复用：状态链逐位对齐 / nv 不泄漏 / 首步与从零建树逐位一致 / 错位必须报错"),
    ("缓存自检",      ["_chk_cache.py", "selftest"],           ENGINE,
     "缓存守卫本身会被触发"),
    ("线程数复现",    ["_chk_threads.py"],                     ENGINE,
     "同线程数跳进程权重逐位相同（挡住未播种 RNG / 调度敏感）；换线程数只作观测"),
    ("按怪组×地点采集", ["_chk_encs.py"],                     ENGINE,
     "每项固定场数：`enc`/`enc_floor` 列 + 按地点发初始状态 + 分片复用 + `eps` 探索"),
    ("缓存宽度",      ["_chk_cache.py", "{data}"],              DATA,
     "这份 obs 是不是**当前二进制**采的"),
    ("缩放表体检",    ["_chk_scale.py", "{data}", "30"],       DATA,
     "每个块的每个字段**缩放后**的分布"),
    ("遭遇战归因",    ["_chk_enc.py", "{data}", "60"],         DATA,
     "决策点按遭遇战归因（回答“多怪状态为什么这么少”）"),
]


def run_one(name, argv, data, timeout):
    cmd = [sys.executable] + [a.format(data=data or "") for a in argv]
    # 子进程统一 UTF-8：中文 Windows 默认 GBK，读源码/日志时会 UnicodeDecodeError。
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", env=env, timeout=timeout,
                           cwd=os.path.dirname(os.path.abspath(__file__)))
    except subprocess.TimeoutExpired:
        return False, f"⏱ 超时 {timeout}s", time.time() - t0, ""
    dt = time.time() - t0
    out = (p.stdout or "") + (p.stderr or "")
    tail = "\n".join([ln for ln in out.strip().splitlines() if ln.strip()][-3:])
    if p.returncode == 0:
        return True, "✅ 通过", dt, tail
    # 负数 = 被信号打死（Windows 上也可能是大整数）；SIGABRT(=6) 就是 C++ assert
    sig = -p.returncode if p.returncode < 0 else None
    why = (f"💥 被信号 {sig} 打死（C++ assert / SIGABRT？）" if sig
           else f"❌ 退出码 {p.returncode}")
    return False, why, dt, tail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", help="data 档需要的缓存路径（.npz）")
    ap.add_argument("--list", action="store_true", help="只列清单")
    ap.add_argument("-k", dest="kw", help="只跑名字里含该子串的")
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args()

    tiers = [ENGINE] + ([DATA] if args.data else [])
    todo = [c for c in CHECKS
            if c[2] in tiers and (not args.kw or args.kw.lower() in c[0].lower())]

    if args.list:
        print(f"{'档位':8} {'名字':16} 说明")
        for name, argv, tier, desc in CHECKS:
            print(f"{tier:8} {name:16} {desc}\n{'':25} → {' '.join(argv)}")
        return 0

    print(f"守卫总检：{len(todo)} 项"
          f"（档位 {', '.join(tiers)}"
          + (f"；data={args.data}" if args.data else "") + "）")
    print("（每项各起一个子进程 —— 守卫可能被 C++ assert 打死，同进程跑会连坐）\n")

    fails = []
    for name, argv, tier, desc in todo:
        print(f"[{tier}] {name:16} {desc}", flush=True)
        ok, why, dt, tail = run_one(name, argv, args.data, args.timeout)
        print(f"        → {why}   ({dt:.1f}s)", flush=True)
        if not ok:
            for ln in tail.splitlines():
                print(f"          | {ln}")
            fails.append(name)

    n_run = len(todo)
    print(f"\n================ {n_run - len(fails)} / {n_run} 通过"
          + " ================")
    if fails:
        print(f"失败：{fails}")
    return len(fails)


if __name__ == "__main__":
    sys.exit(main())
