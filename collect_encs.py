"""按怪组 × 分布地点采集教师数据：每项固定场数。

    python collect_encs.py <每组场数> <sims> [out.npz] [--force]

触发器：只定义任务（`ENCS` 表），逐项执行在 `collect_part`（造环境 / 跑教师在 `collect`）。
`task="encs"`。分片落 `<缓存 stem>_parts/<enc>@<层>.npz`，已存在则复用；中断后重跑即续跑。

表项 = `(sts.MonsterEncounter, 出现层)`：
  · 分组依据引擎的 `MonsterEncounterPool`（`include/constants/MonsterEncounters.h`）——
    一幕前半场抽弱怪池、后半场抽强怪池，精英房全幕同池，Boss 房在幕末；
  · 层号取该档的代表位置，只影响 `devshape` 按层发什么卡组/遗物/hp；
  · 各项共用同一段 seed，项间只差遭遇与层号。
改表即换任务：新表与旧缓存不对应，旧缓存得重采。

调度四步：① 列清单 ② 剔除已有分片 ③ 逐项拉起 `collect_part.py` 子进程（并发 = `--workers`）
④ 逐项回收判码。
③ 的并发用**线程池**：launcher 只阻塞等子进程、不占 CPU，用进程池只会多付一遍 import。
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Iterable, Mapping, Sequence

import numpy as np
import slaythespire as sts
from tqdm import tqdm

from collect import Job
# 逐项采集的执行者在 `collect_part`（任务标识 `TASK` 与单项任务流都在那边）；
# 这里重导出一次，`collect_encs.TASK` / `collect_encs.generate_single_enc_jobs` 的老引用照旧可用。
from collect_part import TASK, generate_single_enc_jobs, part_argv
import runtime
from runtime import cache_name, cache_path, recipe_kv

ENCS_KIND = "encdata"        # 缓存名前缀

# 任务表；改一行即换任务（覆盖 (遭遇, 层) 去重集）。
ENCS = (
    # ---------------- Act 1（层 1..16）----------------
    (sts.MonsterEncounter.CULTIST, 4),                  # 弱怪池（前半场）
    (sts.MonsterEncounter.JAW_WORM, 4),
    (sts.MonsterEncounter.TWO_LOUSE, 4),
    (sts.MonsterEncounter.SMALL_SLIMES, 4),
    (sts.MonsterEncounter.GREMLIN_GANG, 12),            # 强怪池（后半场）
    (sts.MonsterEncounter.LOTS_OF_SLIMES, 12),
    (sts.MonsterEncounter.RED_SLAVER, 12),
    (sts.MonsterEncounter.EXORDIUM_THUGS, 12),
    (sts.MonsterEncounter.EXORDIUM_WILDLIFE, 12),
    (sts.MonsterEncounter.BLUE_SLAVER, 12),
    (sts.MonsterEncounter.LOOTER, 12),
    (sts.MonsterEncounter.LARGE_SLIME, 12),
    (sts.MonsterEncounter.THREE_LOUSE, 12),
    (sts.MonsterEncounter.TWO_FUNGI_BEASTS, 12),
    (sts.MonsterEncounter.GREMLIN_NOB, 12),             # 精英房
    (sts.MonsterEncounter.LAGAVULIN, 12),
    (sts.MonsterEncounter.THREE_SENTRIES, 12),
    (sts.MonsterEncounter.SLIME_BOSS, 16),              # Boss 房（幕末）
    (sts.MonsterEncounter.THE_GUARDIAN, 16),
    (sts.MonsterEncounter.HEXAGHOST, 16),
    # ---------------- Act 2（层 17..33）----------------
    (sts.MonsterEncounter.SPHERIC_GUARDIAN, 20),        # 弱怪池
    (sts.MonsterEncounter.CHOSEN, 20),
    (sts.MonsterEncounter.SHELL_PARASITE, 20),
    (sts.MonsterEncounter.THREE_BYRDS, 20),
    (sts.MonsterEncounter.TWO_THIEVES, 20),
    (sts.MonsterEncounter.CHOSEN_AND_BYRDS, 30),        # 强怪池
    (sts.MonsterEncounter.SENTRY_AND_SPHERE, 30),
    (sts.MonsterEncounter.CULTIST_AND_CHOSEN, 30),
    (sts.MonsterEncounter.THREE_CULTIST, 30),
    (sts.MonsterEncounter.SHELLED_PARASITE_AND_FUNGI, 30),
    (sts.MonsterEncounter.SNECKO, 30),
    (sts.MonsterEncounter.SNAKE_PLANT, 30),
    (sts.MonsterEncounter.CENTURION_AND_HEALER, 30),
    (sts.MonsterEncounter.GREMLIN_LEADER, 30),          # 精英房
    (sts.MonsterEncounter.SLAVERS, 30),
    (sts.MonsterEncounter.BOOK_OF_STABBING, 30),
    (sts.MonsterEncounter.AUTOMATON, 33),               # Boss 房
    (sts.MonsterEncounter.COLLECTOR, 33),
    (sts.MonsterEncounter.CHAMP, 33),
    # ---------------- Act 3（层 34..50）----------------
    (sts.MonsterEncounter.THREE_DARKLINGS, 37),         # 弱怪池（它也在强怪池里，只收这一档）
    (sts.MonsterEncounter.ORB_WALKER, 37),
    (sts.MonsterEncounter.THREE_SHAPES, 37),
    (sts.MonsterEncounter.SPIRE_GROWTH, 45),            # 强怪池
    (sts.MonsterEncounter.TRANSIENT, 45),
    (sts.MonsterEncounter.FOUR_SHAPES, 45),
    (sts.MonsterEncounter.MAW, 45),
    (sts.MonsterEncounter.SPHERE_AND_TWO_SHAPES, 45),
    (sts.MonsterEncounter.JAW_WORM_HORDE, 45),
    (sts.MonsterEncounter.WRITHING_MASS, 45),
    (sts.MonsterEncounter.GIANT_HEAD, 45),              # 精英房
    (sts.MonsterEncounter.NEMESIS, 45),
    (sts.MonsterEncounter.REPTOMANCER, 45),
    (sts.MonsterEncounter.AWAKENED_ONE, 50),            # Boss 房
    (sts.MonsterEncounter.TIME_EATER, 50),
    (sts.MonsterEncounter.DONU_AND_DECA, 50),
    # ---------------- Act 4（层 51..）----------------
    (sts.MonsterEncounter.SHIELD_AND_SPEAR, 53),
    (sts.MonsterEncounter.THE_HEART, 53),
    # 事件遭遇（需事件上下文；层 = 该事件所在幕的代表位置）
    (sts.MonsterEncounter.LAGAVULIN_EVENT, 12),
    (sts.MonsterEncounter.MUSHROOMS_EVENT, 12),
    (sts.MonsterEncounter.COLOSSEUM_EVENT_SLAVERS, 30),
    (sts.MonsterEncounter.COLOSSEUM_EVENT_NOBS, 30),
    (sts.MonsterEncounter.MASKED_BANDITS_EVENT, 30),
    (sts.MonsterEncounter.MYSTERIOUS_SPHERE_EVENT, 30),
)


def generate_encs_jobs(n_per: int, start_seed: int = 0, encs: Iterable[tuple[sts.MonsterEncounter, int]] = ENCS) -> list[Job]:
    """整张表的任务流（三条件采集线共用）。"""
    return [j for enc, floor in encs for j in generate_single_enc_jobs(enc, floor, n_per, start_seed)]


# ---------------------------------------------------------------- 缓存名

# 不进缓存名的参数：名字里要能看出「怎么采的」，下面这些进去只会添乱。
#   `encs` 的差别由表指纹 `ENCS_FP` 代表（一张表打不成文件名）；
#   `cache` / `cache_dir` 是**输出路径**；`force` 只管重采不重采；`verbose` 是日志；
#   `workers` 是并行度 —— 不改内容（并行结果与串行逐位相同，见 `_mp_test.py`）。
# `name` 是**标签**（进名字最前面），不是配方参数 —— 由 `default_cache` 单独放。
NAME_DROP = ("encs", "cache", "name", "force", "verbose", "workers", "cache_dir")


def table_fp(encs: Iterable[tuple[sts.MonsterEncounter, int]] = ENCS) -> str:
    """任务表的 8 位指纹（`(遭遇 id, 层)` 序列的哈希）。

    ⚠️ 改表即换任务，但名字里看不出表变过 ⇒ 旧缓存会被当成本表的续跑复用（分片目录同名）。
    指纹进名字后，改表自动换名。
    """
    s = ";".join(f"{int(e)}@{int(f)}" for e, f in encs)
    return hashlib.sha1(s.encode()).hexdigest()[:8]


ENCS_FP = table_fp()


def default_cache(n_per: int, sims: int, eps: float = 0.0, start_seed: int = 0,
                  name: str | None = None, cache_dir: str | None = None) -> str:
    """本表的默认缓存路径 —— **唯一**拼名点（免得各处各算一份）。

    名字 = `[<自定义名字>_]<kind>_n<N>_s<SIMS>_<全部配方参数（含默认值，按签名顺序）>_fp<表指纹>.npz`。
    参数名/顺序跟着 `collect_encs()` 的签名走 ⇒ 新增一个配方参数就自动进名字；忘了传它，
    `recipe_kv` 当场 `KeyError`（一种参数写同一个名字是静默错，代价高）。
    """
    kv = recipe_kv(collect_encs, dict(n_per=n_per, sims=sims, eps=eps, start_seed=start_seed),
                   NAME_DROP)
    n, s = kv.pop("n_per"), kv.pop("sims")
    return cache_path(cache_name(name, ENCS_KIND, n, s, **kv, fp=ENCS_FP), cache_dir)


# ---------------------------------------------------------------- ③ 子进程：拉起一项

def _launch_single_enc(enc: sts.MonsterEncounter, floor: int, n_per: int, sims: int, eps: float,
                       start_seed: int, part: str) -> tuple[sts.MonsterEncounter, int, int, str]:
    """③ 拉起**一项**的采集子进程（`collect_part.py`），只回小结果（不传张量）。

    命令行由 `collect_part.part_argv` 按**被调函数的签名**拼 ⇒ 加参数时两侧一起变，
    漏给或拼错一个名字当场 `TypeError`。
    返回 `(enc, floor, 退出码, 子进程输出原文)`；原文**只剔 tqdm 进度条行**，其余不嗅探、不截断，
    判读交给 `_report`（**完成一项就判一项**）。已完成的分片在**进池之前**就被剔掉了，这里不再判复用。

    `enc` 是枚举，`collect_part` 只收它的 `int`（argv 传不了枚举）。
    """
    cmd = part_argv(sys.executable, enc=int(enc), floor=int(floor), n_per=n_per, sims=sims,
                    out=part, eps=eps, start_seed=start_seed)
    p = subprocess.run(cmd, cwd=os.path.dirname(os.path.abspath(__file__)),
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    # `stderr` 在前（原生的 abort / 断言从那里出来）。逐行**保留缩进**（traceback 的帧缩进
    # 是读现场的一部分），只丢掉空行与 tqdm 的进度条帧。
    raw = ((p.stderr or "") + (p.stdout or "")).splitlines()
    text = "\n".join(ln for ln in raw if ln.strip() and "%|" not in ln and "it/s]" not in ln)
    return enc, floor, p.returncode, text


def _concat_caches(parts: Sequence[Mapping[str, np.ndarray]]) -> dict[str, Any]:
    """拼接同 schema 的分片"""
    scalars = ("space", "obs_dim", "task", "src")
    base = parts[0]
    keys = set(base)
    n_batches: list[int] = []
    for i, part in enumerate(parts):
        if set(part) != keys:
            raise RuntimeError(f"第 {i} 片分片的键集与第 0 片不同：多 {sorted(set(part) - keys)}、缺 {sorted(keys - set(part))}")
        diff = [k for k in scalars if not np.array_equal(part[k], base[k])]
        if diff:
            raise RuntimeError(f"第 {i} 片分片的 {diff} 与第 0 片不一致，不可拼接")
        b = np.asarray(part["bid"])
        k = len(np.unique(b))                       # 该片场数（空片 = 0）
        if len(b) and (int(b.min()) != 0 or int(b.max()) + 1 != k):
            raise RuntimeError(f"第 {i} 片分片的 `bid` 取值不是 `0..k-1`：min={int(b.min())}、"
                               f"max={int(b.max())}、场数={k} —— 错开后会有空洞")
        n_batches.append(k)

    offs, off = [], 0
    for k in n_batches:                             # 错开量 = 前面各片的场数之和
        offs.append(off)
        off += k
    out: dict[str, Any] = {}
    for k in sorted(keys):
        if k in scalars:
            out[k] = base[k]                        # 已验过四片一致，取基准片那份
        elif k == "bid":
            out[k] = np.concatenate([p[k] + o for p, o in zip(parts, offs)])
        else:
            out[k] = np.concatenate([p[k] for p in parts])
    return out


# ---------------------------------------------------------------- 主流程

def dup_check(pairs: Sequence[tuple[sts.MonsterEncounter, int]]) -> None:
    """重复项会各采一遍（分片同名会互相覆盖），等于静默多采。"""
    dup = sorted({p for p in pairs if pairs.count(p) > 1})
    if dup:
        raise SystemExit(f"表里有重复的（怪组, 层）对：{dup}")


def parts_dir_of(cache: str) -> str:
    """分片目录 = 缓存名 + `_parts`；建好返回。"""
    d = os.path.splitext(cache)[0] + "_parts"
    os.makedirs(d, exist_ok=True)
    return d


# 一项的调度载荷：`(遭遇, 层, 分片路径)` —— 只装逐项会变的东西
# （`n_per` / `sims` / `eps` / `start_seed` 全表共用，不进载荷）。
Item = tuple[sts.MonsterEncounter, int, str]


def _report(r: tuple[sts.MonsterEncounter, int, int, str],
            ok: list[tuple[int, int]], failed: list[tuple[int, int]]) -> None:
    """④ 回收：判**一项**的退出码，把结果记进 `ok` / `failed` 并打印这一项的结果。

    ⚠️ **完成一项就调一次**（不是整轮跑完才判）：实测 61/63 项崩溃时，旧写法让日志十几分钟里
    只显示一条「正常推进」的进度条（失败项是秒崩的，`as_completed` 照样计数）。
    ⚠️ 成功只打子进程自己那行战报；**失败把子进程输出原文全打出来**（不截断 —— 失败现场是
    唯一能定位原生 abort / 断言 / traceback 的东西）。
    ⚠️ 走 `tqdm.write` 而不是 `print`，否则会把进度条那行撕成两半；代价是**成功项在日志里的
    顺序 = 完成顺序**（与调度有关），不再按 (层, 遭遇) 排序。
    """
    enc, floor, rc, text = r
    tag = f"{enc.name}@{floor}"
    if rc == 0:
        ok.append((int(enc), int(floor)))
        if text:
            tqdm.write(f"  {tag}：{text.strip()}")
    else:
        failed.append((int(enc), int(floor)))
        tqdm.write(f"  {tag} 失败（退出码 {rc}）：\n"
                   f"{text or '（子进程无任何输出 —— 原生终止？）'}")


def collect_items(n_per: int, sims: int, pairs: Sequence[tuple[sts.MonsterEncounter, int]],
                  parts_dir: str, eps: float = 0.0, start_seed: int = 0, force: bool = False,
                  verbose: bool = True, desc: str = "采集",
                  workers: str | int | None = None) -> list[tuple[int, int]]:
    """采一批项，返回成功的 `(enc, 层)`。

    ① 列清单 → ② 剔已有分片（没分片的进 `todo`、有的进 `reused`）→ ③ 逐项拉起子 进程 → ④ 逐项回收。
    每项一个子进程是**隔离边界**（C++ 原生 abort 只带走那一项），也是「每项固定场数」的来源。

    ③ 用**线程池**：launcher 只阻塞在 `subprocess.run`、不占 CPU，用进程池只会多付一遍 import。
    `workers` = 并发上限（`None` = 按 cgroup 配额；'serial'/0 = 串行）。

    载荷 = `Item`，只装**逐项会变**的东西；`n_per` / `sims` / `eps` / `start_seed` 全表共用，
    直接进调用点、不进载荷。

    返回**有分片的项的全集**（复用的 + 本次采成功的）—— 调用方拿它去 `merge_and_save`，
    只回「本次实采的」会把复用的那批静默丢掉。
    """
    items: list[Item] = [(e, f, os.path.join(parts_dir, f"{int(e)}@{f}.npz"))
                         for e, f in pairs]
    todo: list[Item] = []                          # ② 还没分片的
    skip: list[Item] = []                        #    已有分片的（本次跳过）
    for item in items:
        if force or not os.path.exists(item[-1]):
            todo.append(item)
            print(f"-todo: {item[-1]}")
        else:
            skip.append(item)
            print(f"-skip: {item[-1]}")
    if skip:
        print(f"  复用已有分片 {len(skip)} 项，本次实采 {len(todo)} 项", flush=True)

    w = runtime.workers(workers)
    ok: list[tuple[int, int]] = []
    failed: list[tuple[int, int]] = []
    if w and w > 1 and len(todo) > 1:
        with ThreadPoolExecutor(max_workers=min(w, len(todo))) as ex:
            futs = [ex.submit(_launch_single_enc, e, f, n_per, sims, eps, start_seed, part)
                    for e, f, part in todo]
            for fut in tqdm(as_completed(futs), total=len(futs), desc=desc, unit="项",
                            disable=not verbose, dynamic_ncols=True, mininterval=0.4):
                _report(fut.result(), ok, failed)
    else:
        for e, f, part in tqdm(todo, desc=desc, unit="项", disable=not verbose,
                               dynamic_ncols=True):
            _report(_launch_single_enc(e, f, n_per, sims, eps, start_seed, part), ok, failed)
    if failed:
        names = [f"{sts.MonsterEncounter(e).name}@{f}" for e, f in sorted(failed)]
        print(f"{len(failed)}/{len(items)} 项采不到，已排除：{names}", flush=True)
    return ok + [(int(e), int(f)) for e, f, _part in skip]


def merge_and_save(cache: str, parts_dir: str, ok: Sequence[tuple[int, int]],
                   t0: float | None = None) -> dict[str, Any]:
    """成功分片 → 缓存 + 逐项战报 + 落盘。

    `ok` 一律按 (层, 遭遇) 排序后合并：行序只由任务决定，与"一次采完"还是"逐层分批采"
    无关，两种采法产物逐位相同。
    """
    ok = sorted(ok, key=lambda p: (p[1], p[0]))
    data = _concat_caches([dict(np.load(os.path.join(parts_dir, f"{e}@{f}.npz")))
                           for e, f in ok])
    np.savez(cache, **data)

    nb = np.bincount(data["bid"])
    first = np.unique(data["bid"], return_index=True)[1]
    win_b = np.bincount(data["bid"], weights=data["t_win"]) / nb
    agg: dict[tuple[int, int], list] = {}
    for j, (e, f) in enumerate(zip(data["enc"][first].tolist(),
                                   data["enc_floor"][first].tolist())):
        a = agg.setdefault((int(e), int(f)), [0, 0, 0.0])
        a[0] += 1
        a[1] += int(nb[j])
        a[2] += float(win_b[j])
    print(f"\n逐项战报（{len(ok)} 项 / {len(first)} 场 / {len(data['obs'])} 行）：")
    print(f"  {'遭遇':28s} {'层':>3} {'场':>4} {'行':>7} {'均行/场':>7} {'教师胜率':>8}")
    for (e, f), (n, rows, wsum) in sorted(agg.items(), key=lambda kv: (-kv[0][1], kv[0][0])):
        print(f"  {sts.MonsterEncounter(e).name:28s} {f:3d} {n:4d} {rows:7d} "
              f"{rows / n:7.1f} {wsum / n:8.1%}")
    print(f"合计 {len(first)} 场 / {len(data['obs'])} 行"
          + (f"；{time.perf_counter() - t0:.0f}s" if t0 is not None else "")
          + f"；教师总胜率 {float(np.mean(win_b)):.1%}", flush=True)
    print(f"已存 {cache}\n"
          f"训练：python train.py train {cache} <epochs>", flush=True)
    return data


def collect_encs(n_per: int, sims: int,
                 encs: Iterable[tuple[sts.MonsterEncounter, int]] = ENCS, eps: float = 0.0,
                 start_seed: int = 0, name: str | None = None, cache: str | None = None,
                 force: bool = False, verbose: bool = True, workers: str | int | None = None,
                 cache_dir: str | None = None) -> dict[str, Any]:
    """整张表一次采完，返回缓存 dict（同时落盘）。

    参数：
      `n_per`      每项场数（总场数 = 项数 × `n_per`）
      `sims`       教师搜索预算（`teacher_action` 的模拟次数）
      `encs`       `(sts.MonsterEncounter, 层)` 可迭代，默认 `ENCS`
      `eps`        教师均匀探索概率，`0.0` = argmax
      `start_seed` 起始 seed，各项共用这一段（第 i 场用 `start_seed + i`）
      `name`       自定义名字（进缓存名最前面；同名会指向同一份数据）
      `cache`      输出 npz；`None` 时走 `default_cache(n_per, sims, eps, start_seed, name)`
      `workers`    并发上限（`None` = 按 cgroup 配额；'serial'/0 = 关并行）
      `cache_dir`  缓存目录（`None` = 仓库内 `./data/`）
      `force`      忽略已有分片重采
    """
    pairs = [(e, int(f)) for e, f in encs]
    dup_check(pairs)
    if cache is None:
        cache = default_cache(n_per, sims, eps, start_seed, name, cache_dir)
    parts_dir = parts_dir_of(cache)
    t0 = time.perf_counter()
    print(f"按怪组×分布地点采集：{len(pairs)} 项 × 每项 {n_per} 场 @ {sims} sims  eps={eps}"
          f"（每项一个子进程；分片 {parts_dir}，已存在则复用）", flush=True)
    ok = collect_items(n_per, sims, pairs, parts_dir, eps, start_seed, force, verbose,
                       desc="采集（按怪组×地点）", workers=workers)
    if not ok:
        raise SystemExit("所有项都失败，见上面逐项报错")
    return merge_and_save(cache, parts_dir, ok, t0)


def main() -> None:
    p = argparse.ArgumentParser(description="按怪组×分布地点采集教师数据（每项固定场数）",
                                epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("n_per", type=int, help="每项采多少场")
    p.add_argument("sims", type=int, help="教师 MCTS 模拟数")
    p.add_argument("out", nargs="?", default=None, help="缓存路径（默认按 `cache_name` 拼）")
    p.add_argument("--force", action="store_true", help="忽略已有分片，重采")
    p.add_argument("--eps", type=float, default=0.0, help="教师均匀探索概率（0 = argmax）")
    p.add_argument("--seed0", type=int, default=0, help="种子段起点（各项共用这一段）")
    p.add_argument("--name", default=None, help="自定义名字（进缓存名最前面）")
    p.add_argument("--workers", default=None,
                   help="并行 worker 数；'serial'/0 关闭并行；不给 = 按 cgroup 配额")
    p.add_argument("--cache-dir", default=None, help="缓存目录（默认 <repo>/data）")
    a = p.parse_args()
    collect_encs(a.n_per, a.sims, eps=a.eps, start_seed=a.seed0, name=a.name, cache=a.out,
                 force=a.force, workers=a.workers, cache_dir=a.cache_dir)


if __name__ == "__main__":
    sys.exit(main())
