"""采**一项**（怪组 × 分布地点）写一个分片 —— 每项一个进程的隔离边界。

    python collect_part.py --enc <遭遇 id> --floor <层> --n-per <场数> --sims <模拟数> \\
                           --out <分片路径> [--eps E] [--start-seed N]

被 `collect_encs.collect_items` 逐项拉起。为什么要跨进程：引擎会对个别遭遇原生 abort
（MSVC `abort()` / SIGABRT），同进程的 `try/except` 接不住，而且会带走整轮采集。

本脚本只认**一项**：任务表（有哪些 `(怪组, 层)`）在 `collect_encs`。
"""
import argparse
import inspect
import os
import sys
from typing import Any

import numpy as np
import slaythespire as sts

from collect import Job, Src, Task, assemble, make_teacher

TASK = Task.ENCS              # 任务标识，写进缓存的 `task` 列

HELP = {
    "enc": "遭遇 id（`sts.MonsterEncounter` 的取值）",
    "floor": "分布层（`devshape` 按层发初始状态）",
    "n_per": "场数",
    "sims": "教师 MCTS 模拟数",
    "out": "分片路径",
    "eps": "教师均匀探索概率（0 = argmax）",
    "start_seed": "seed 段起点（第 i 场用 `start_seed + i`）",
}


def generate_single_enc_jobs(enc: sts.MonsterEncounter, floor: int, n_per: int,
                             start_seed: int = 0) -> list[Job]:
    """一项的任务流：同一段 seed × `n_per` 场。"""
    return [(start_seed + i, enc, int(floor)) for i in range(n_per)]


def collect_part(enc: int, floor: int, n_per: int, sims: int, out: str,
                 eps: float = 0.0, start_seed: int = 0) -> dict[str, Any]:
    """采一项 `(怪组, 层)` 的 `n_per` 场，写分片 `out`，返回缓存 dict（同时落盘）。

    任何一场出错就**当场崩**（不吞异常、不写短分片）：异常冒到顶层 ⇒ 非零退出 ⇒ 父进程按
    退出码把**整项**判失败并排除。一项一个进程就是隔离边界，「每项固定场数」由此成立。

    `enc` 是 `sts.MonsterEncounter` 的 id（argv 只能传 int，这里转回枚举）。
    """
    e = sts.MonsterEncounter(int(enc))
    fl = int(floor)
    battle = make_teacher(sims, eps)
    trajs = [battle(job) for job in generate_single_enc_jobs(e, fl, n_per, start_seed)]
    data = assemble(trajs, TASK, Src.TEACHER)
    np.savez(out, **data)
    print(f"  {e.name}@{fl}: {len(trajs)} 场"
          f"胜率 {sum(int(r.term.t_win > 0) for r in trajs) / len(trajs):.1%} "
          f"行 {len(data['obs'])}", flush=True)
    return data


def part_argv(python: str, **kw) -> list[str]:
    """`collect_part()` 的 kwargs → 子进程 argv（本脚本路径也在里面）。

    flag 名从 `collect_part()` 的签名派生（与 `_cli()` 同源）⇒ 加参数时两侧一起变。
    调用方漏给或拼错一个名字都当场 `TypeError`，不会静默用子进程的默认值 —— 那等于采错数据。
    """
    sig = inspect.signature(collect_part)
    missing = [k for k in sig.parameters if k not in kw]
    unknown = sorted(set(kw) - set(sig.parameters))
    if missing or unknown:
        raise TypeError(f"`part_argv` 与 `collect_part()` 的形参不一致：缺 {missing}、多 {unknown}")
    return ([python, "-u", os.path.abspath(__file__)]
            + [f"--{k.replace('_', '-')}={kw[k]}" for k in sig.parameters])


def _cli() -> argparse.ArgumentParser:
    """按 `collect_part()` 的签名注册 flag（类型与默认值也从签名来）。"""
    p = argparse.ArgumentParser(description="采一项（怪组 × 分布地点）写一个分片",
                                epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    for k, prm in inspect.signature(collect_part).parameters.items():
        kw: dict[str, Any] = dict(help=HELP.get(k, ""))
        if prm.default is inspect.Parameter.empty:
            kw |= dict(required=True, type=prm.annotation)
        else:
            kw |= dict(default=prm.default, type=type(prm.default))
        p.add_argument("--" + k.replace("_", "-"), **kw)
    return p


def main() -> None:
    # 名字对不上就让它自己 TypeError —— 不写「flag → 实参」的第二张表。
    collect_part(**vars(_cli().parse_args()))


if __name__ == "__main__":
    sys.exit(main())
