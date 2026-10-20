"""教师采集：`sts.teacher_run` 一个调用（任务表 / 造局面 / 搜索 / 落盘全在 C++），本文件只打读数。

    python collect_teacher.py --n 32 --sims 3000 [--net-path x.pt] [--cache out]

⚠️ 产物名不带配方（同 `collect_selfplay`，见 `runtime.check_recipe`）：默认目录 = `<cache-dir>/encdata_teacher`，
   配方写进目录里的 `_recipe.json`，复用前逐字比对。
"""
import os
import sys
import time

import slaythespire as sts

from collect import CollectData, MAX_STEPS
from runtime import artifact_dir, cli_for, recipe_kv, reuse_shards

# 产物**种类**（默认目录名 = 它；也是 `_recipe.json` 里的 `kind`）。
KIND = "encdata_teacher"

# 不进**配方**的形参：`cache` / `cache_dir` 是输出路径；`shard_rows` 只改分片粒度；
# `threads` 只改墙钟（数据逐位相同）；`net_path` **教师采集根本不加载网络**（旧名字里带着它
# 只是为了"看得出是哪份网"，但教师数据与网无关）⇒ 不进配方。
RECIPE_DROP = ("cache", "cache_dir", "shard_rows", "threads", "net_path")


def recipe_of(kw: dict) -> dict:
    """本次采集的**配方**（写进 `_recipe.json`）：`collect_teacher()` 的全部形参（按 `RECIPE_DROP` 除外）。"""
    return recipe_kv(collect_teacher, kw, RECIPE_DROP)


def collect_teacher(n: int, sims: int, net_path: str = "-", cache: str | None = None,
                    eps: float = 0.0, start_seed: int = 0,
                    cache_dir: str | None = None, shard_rows: int = 200000,
                    threads: int = 0) -> list[str]:
    """采 `n` × 全表，返回分片路径（产物 = `cache` 目录）。

    `net_path` **不加载权重**（保留只是因为调用方习惯传它；不进配方）。
    `shard_rows` / `threads` 只改分片粒度与墙钟（数据逐位相同）。
    """
    recipe = recipe_of(locals())
    if sims < 1:
        sys.exit(f"sims 太小：{sims}（教师至少 1）")
    shard_dir = cache or artifact_dir(KIND, cache_dir)
    got = reuse_shards(KIND, shard_dir, recipe)  # 空壳清理 + 配方闸 + 命中复用（各打一行）
    if got:
        return got

    cfg = sts.TeacherConfig()
    cfg.sims, cfg.max_steps = int(sims), MAX_STEPS
    cfg.eps, cfg.tau, cfg.threads = float(eps), 0.0, int(threads)
    t0 = time.perf_counter()
    parts = [str(p) for p in sts.teacher_run(int(n), cfg, int(start_seed), shard_dir,
                                             int(shard_rows))]
    if not parts:
        sys.exit(f"采集没有产出任何分片 ⇒ 看 {shard_dir}")
    d = CollectData(shard_dir)
    print(f"教师完成：{d.summary()}；sims={sims} eps={eps}；{time.perf_counter() - t0:.0f}s",
          flush=True)
    return parts


def main() -> int:
    kw = vars(cli_for(collect_teacher, "collect teacher data (all in C++)").parse_args())
    kw["cache"] = kw["cache"] or artifact_dir(KIND, kw["cache_dir"])
    print(f"教师采集：{kw['n']} 项 × UCT({kw['sims']}) → {kw['cache']}", flush=True)
    collect_teacher(**kw)
    return 0


if __name__ == "__main__":
    sys.exit(main())
