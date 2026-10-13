"""教师采集：`sts.teacher_run` 一个调用（任务表 / 造局面 / 搜索 / 落盘全在 C++），本文件只拼产物名 + 打读数。

    python collect_teacher.py --n 32 --sims 3000 [--net-path x.pt] [--cache out]
"""
import os
import shutil
import sys
import time

import slaythespire as sts

import ckpt
from collect import CollectData, MAX_STEPS
from collect_io import list_shards
from runtime import cache_name, cache_path, cli_for, net_tag

# 不进产物名的参数（改了它数据仍逐位相同）。⚠️ 加名字前先问这一句。
NOT_IN_NAME = ("cache", "cache_dir", "shard_rows", "threads")


def default_cache(*, n: int, sims: int, net_path: str = "-", eps: float = 0.0,
                  start_seed: int = 0, cache_dir: str | None = None, **not_in_name) -> str:
    """默认产物目录（唯一拼名点）。传入未列入 `NOT_IN_NAME` 的参数 = 有配方参数没进名字 ⇒ 报错。"""
    stray = sorted(set(not_in_name) - set(NOT_IN_NAME))
    if stray:
        raise TypeError(f"{stray} 不是已知的「不进名字」参数：要么拼错，要么得让它进名字")
    extra = {"net": net_tag(net_path), "wfp": ckpt.read(net_path)["fp"]} if net_path != "-" else {}
    return cache_path(cache_name(None, "encdata_teacher", n, sims, eps=eps,
                                 start_seed=start_seed, **extra), cache_dir)


def _shard_dir(cache: str | None, cache_dir: str | None) -> str:
    """产物目录（**唯一拼点**）：`--cache` 给的就是目录。"""
    if cache:
        return cache
    return os.path.join(cache_dir or "data", "_teacher_tmp")


def collect_teacher(n: int, sims: int, net_path: str = "-", cache: str | None = None,
                    eps: float = 0.0, start_seed: int = 0,
                    cache_dir: str | None = None, shard_rows: int = 200000,
                    threads: int = 0) -> list[str]:
    """采 `n` × 全表，返回分片路径（产物 = `cache` 目录）。

    `net_path` 只为命名与 `obs_dim`，**不加载权重**；`-` = 不关心名字。
    `shard_rows` / `threads` 只改分片粒度与墙钟（数据逐位相同）。
    """
    if sims < 1:
        sys.exit(f"sims 太小：{sims}（教师至少 1）")
    shard_dir = _shard_dir(cache, cache_dir)
    if cache and os.path.isdir(shard_dir):
        got = [str(p) for p in list_shards(shard_dir)]
        if got:
            print(f"已缓存到 {shard_dir}（{len(got)} 分片）", flush=True)   # 调用方靠这行拿路径
            return got
        shutil.rmtree(shard_dir, ignore_errors=True)
    os.makedirs(shard_dir, exist_ok=True)

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
    kw["cache"] = kw["cache"] or default_cache(**kw)
    print(f"教师采集：{kw['n']} 项 × UCT({kw['sims']}) → {kw['cache']}", flush=True)
    collect_teacher(**kw)
    return 0


if __name__ == "__main__":
    sys.exit(main())
