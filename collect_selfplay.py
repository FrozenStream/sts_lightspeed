"""自对弈采集：用网络自己的 PUCT 产训练数据（闭环的生成侧）。

    python collect_selfplay.py --n <每项场数> --sims <sims> --net-path <net.pt> \\
                              [--cache 输出目录] [--start-seed 0] [--name X]

⚠️ flag 由 `runtime.cli_for(collect_selfplay)` 按**签名**注册 ⇒ 这里没有「flag → 实参」的第二张
   表。逐 flag 的说明也只在**这里**（`--help` 把整段原样印出来），不另设一张表：

    --n                 每项场数（总场数 = `encs` 表项数 × n）
    --sims              MCTS 搜索预算（≥2；第 1 次迭代只展开根、不访问根边）
    --net-path          起点网络权重
    --cache             产物**目录**（不给 = 按配方自动命名）
    --games             并肩几棵树（= 前向批宽，也是"一批"的场数；树内恒 1 条路径）
    --start-seed        种子段起点（第 i 场用 `start_seed + i`）
    --sample/--no-sample 按访问分布 π 采样（默认 argmax）
    --eps               均匀探索概率（0 = 关）
    --rollout-threads   前向的 torch 线程数
    --cache-dir         产物目录的父目录（默认 `data/`）
    --temp-moves        前多少个决策点用 τ=1 采样（0 = 关；AGZ 用 30）
    --dirichlet-total   根节点 Dirichlet 的**总浓度**（每动作 α = 总浓度/合法动作数）
    --dirichlet-eps     根噪声权重 ε（0 = 关；AGZ 用 0.25）
    --shard-rows        每多少行落一个分片（只影响粒度，**不进产物名**）
    --name              自定义名字（进产物名最前面；只进名字，不是配方参数）

⚠️ `games` / `rollout_threads` **要进产物名**（批宽改 GEMM 归约顺序、torch 线程数同理）；
    `shard_rows` / `cache_dir` / `name` **不进名**。

⚠️ 任务表 / 造局面 / 搜索 / 落盘**全在 C++**（`sts.selfplay_run`）—— 这里只当**触发器**：
   拼产物名 + 把配方装进 `sts.SelfplayConfig` + 打一行统计（统计由 `CollectData.summary()` 现算）。
   `--workers` / `--mp-start` **已删**（2026-10-13）：触发器在**本进程**里跑完整段，没有 worker
   池，也就不再有"多进程各带一个 CUDA 上下文"那类问题。

只存 MCTS 根边的访问分布，而 `train()` 默认取 argmax，因此蒸馏的是 MCTS 的 argmax 动作。

探索必须开一个（`--sample` 或 `--eps`）：默认 argmax 确定性，且每轮复用同一批 seeds，
重跑会产出逐位相同的轨迹，闭环拿不到新状态。

⚠️ **K 棵树 + 共享前向服务**（`collect/selfplay.cpp`）：每棵树由**一条线程**跑到底，树间不互相
   等；前向在 C++ 侧攒批（`NNEval`，KataGo `NNEvaluator::evaluate` 同形）⇒ **K>1 不再逐位
   可复现**（批的组成由到达时序决定，而前向与批宽相关），**K=1** 仍与冻 golden 逐位对拍。
"""
import os
import shutil
import sys
import time

import numpy as np
import torch

import ckpt
from collect import CollectData, MAX_STEPS
from collect_io import list_shards
from runtime import cache_name, cache_path, cli_for, net_tag, recipe_kv
from aznet import OBS_DIM, net_kwargs
from zspec import DIRICHLET_TOTAL, DEFAULT_SPEC, c_puct_for, q_init_for

def _make_ts(path: str, net_path: str) -> None:
    """把 `AZNet` 在本地 torch 上 trace 成 TorchScript 存档（全 worker 共用一份）。

    网络定义只有 Python 一份（`aznet.AZNet`），C++ 侧只吃 trace 出来的图 ⇒ 这里不重复定义。
    ⚠️ 与 `_chk_selfplay.py` 一样：trace 前必须落 `_jit_set_profiling_executor(False)`
    （C++ 的 `TorchEvaluator` 构造里也会落；两条路径必须同一个执行器，否则数值对不上）。
    落盘是**原子**的（先写 `.tmp` 再 `replace`）⇒ 多进程并发时不会读到半个文件。
    """
    from aznet import AZNet
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cfg = ckpt.config_of(net_path)
    if not cfg:
        raise RuntimeError(f"{net_path} 存档里没有建网配置；用 `python ckpt.py {net_path}` 查看")
    net = AZNet(**net_kwargs(cfg))
    net.load_state_dict(ckpt.weights_of(net_path))
    net.eval()
    torch._C._jit_set_profiling_executor(False)
    with torch.no_grad():
        traced = torch.jit.trace(net, torch.zeros(8, OBS_DIM))
    traced.eval()
    tmp = f"{path}.{os.getpid()}.tmp"
    torch.jit.save(traced, tmp)
    os.replace(tmp, path)


def _ts_path(net_path: str) -> str:
    """`net_path` 对应的 TorchScript 存档路径（**唯一**拼名点）。

    `data/selfplay_ts/` 是**派生目录**（不在 `data/` 的缓存命名体系里）：名字由内容指纹决定
    ⇒ 换权重自动换文件、重跑同一份权重直接命中。它是**只读**的（`torch.jit.load` 不会写），
    所以全部 worker 共用一份 —— 不必每进程一份，也不必加锁。
    """
    h = str(ckpt.read(net_path)["fp"])
    stem = os.path.splitext(os.path.basename(net_path))[0].split("-")[0]
    return os.path.join("data", "selfplay_ts", f"{stem}-{h}.ts")


def _shard_dir(cache: str | None, cache_dir: str | None) -> str:
    """产物目录（**唯一拼点**）：`--cache` 给的就是目录。"""
    if cache:
        return cache
    return os.path.join(cache_dir or "data", "_sp_tmp")


# ---------------------------------------------------------------- 产物名

# 不进产物名的参数：`cache` / `cache_dir` 是**输出路径**；`name` 是标签（它是 `default_cache`
# 的形参，不是 `collect_selfplay()` 的）；`net_path` 是路径 —— 它进名的是**身份**（见下），
# 不是路径本身。`shard_rows` **不进名**，但它的实现方式不同（要留一个常量段）⇒ 见 `default_cache`。
# `rollout_threads` **要进名**：它改 torch 线程数 ⇒ 改矩阵乘的归约顺序，属配方（同 `train.threads`）。
# `games` **要进名**：它决定"一次前向吃多少行"（跨树合批）⇒ GEMM 的归约顺序随批宽变，
# 与"每场各跑一次 1 行前向"不是同源数据（实测 K=1 与 K=4 的 π 逐位相同，但不保证换宽后仍如此）。
NAME_DROP = ("cache", "name", "cache_dir", "net_path")


def default_cache(**kw) -> str:
    """默认缓存路径 —— **唯一**拼名点。`kw` = `collect_selfplay()` 的**全部**形参
    （照惯例就是 `default_cache(**vars(parse_args()))`，另外多一个 `name`）——
    进名的与不进名的在这里一次说清，不另抄一张"该传哪些"的清单。

    ⚠️ 旧写法只写 `n` / `sims`：换起点权重、`--eps`、`--sample`、批宽任意一项都会
    **命中同一份旧缓存**（`main()` 先拼名，`load_cache` 见文件存在就直接返回）⇒ 静默复用了
    别的配方采的数据。
    """
    # ⚠️ `shard_rows` 钉成常量 0 送进名字（旧名字里一直带着这一段 `shard_rows0`）：
    #    用真实值 = "同一份数据两个名字"；去掉这一段 = `data/` 里所有旧缓存失联。两个都别做。
    kv = recipe_kv(collect_selfplay, {**kw, "shard_rows": 0}, NAME_DROP)
    n_, s = kv.pop("n"), kv.pop("sims")
    # 路径类参数只留人给的名字：起点权重 = 它的自定义部分 + **内容指纹**（同一个名字训两遍是
    # 两份不同的网，光看名字分不出）。
    extra = {"net": net_tag(kw["net_path"]), "wfp": ckpt.read(kw["net_path"])["fp"]}
    return cache_path(cache_name(kw.get("name"), "spencs", n_, s, **kv, **extra),
                      kw.get("cache_dir"))


def collect_selfplay(n: int, sims: int, net_path: str, cache=None, games=8, start_seed=0,
                     sample=False, eps=0.0, rollout_threads=1, cache_dir=None,
                     temp_moves=0, dirichlet_total=DIRICHLET_TOTAL, dirichlet_eps=0.0,
                     shard_rows=200000) -> list[str]:
    """产自对弈数据：`n` = 每项场数（总场数 = 表项数 × `n`）；返回**分片路径**。

    旋钮的说明在**模块** docstring 里（`--help` 会印出来），这里不重复一遍。

    ⚠️ 树内批已删：`games` = 并肩几棵树 = **前向批宽**（树内恒 1 条路径），不改变任何一棵树的
    搜索语义（实测 `games=4` 与 `games=1` 的 π 逐位相同）。

    整段（任务表 / 造局面 / 搜索 / 落盘）在 C++ 的 `sts.selfplay_run` 里 —— 这里只装配方。
    """
    import slaythespire as sts

    if not os.path.exists(net_path):
        sys.exit(f"找不到网络权重 {net_path}")
    if sims < 2:
        # 第 1 次迭代只展开根、不访问任何根边 ⇒ π 全零（`azspec` 的 `sims ≥ 2` 契约）。
        sys.exit(f"sims 太小：{sims}（至少 2，否则本场搜不出可记的 π）")
    shard_dir = _shard_dir(cache, cache_dir)
    if cache and os.path.isdir(shard_dir):
        got = [str(p) for p in list_shards(shard_dir)]
        if got:
            # 命中已有产物也要报路径：调用方（`_chain.sh`）靠这一行拿到数据位置，
            # 静默返回会让"复用"看起来像"采集没报路径"。
            print(f"产物目录 {shard_dir}（复用已有 {len(got)} 个分片）", flush=True)
            return got
        shutil.rmtree(shard_dir, ignore_errors=True)
    os.makedirs(shard_dir, exist_ok=True)

    # 前向在 C++ 侧 ⇒ 先备一份 TorchScript 存档（`_ts_path` 是唯一拼名点）。
    ts = _ts_path(net_path)
    if not os.path.exists(ts):
        _make_ts(ts, net_path)

    t0 = time.perf_counter()
    cfg = sts.SelfplayConfig()
    cfg.ts_path = ts
    cfg.z_spec = DEFAULT_SPEC
    cfg.sims = int(sims)
    cfg.max_steps = MAX_STEPS
    cfg.threads = int(rollout_threads)
    cfg.games = int(games)          # 并肩几棵树 = 前向批宽（也是"一批"的场数）
    cfg.c_puct = c_puct_for(DEFAULT_SPEC)
    cfg.q_init = q_init_for(DEFAULT_SPEC)
    cfg.sample = bool(sample)
    cfg.eps = float(eps)
    cfg.temp_moves = int(temp_moves)
    cfg.dirichlet_total = float(dirichlet_total)
    cfg.dirichlet_eps = float(dirichlet_eps)
    parts = [str(p) for p in sts.selfplay_run(int(n), cfg, int(start_seed), shard_dir,
                                             int(shard_rows))]
    if not parts:
        sys.exit(f"采集没有产出任何分片 ⇒ 看 {shard_dir}")
    print(f"\n自对弈完成：{len(parts)} 个分片；MCTS({sims}) 并肩 {games} 棵 探索="
          f"{'π采样' if sample else ('eps=%g' % eps if eps > 0 else 'argmax')}；"
          f"{time.perf_counter() - t0:.0f}s", flush=True)
    # 不存 `z`：这里只是把产物打一行统计（训练侧的口径自己选）。
    d = CollectData(shard_dir)
    print(f"  {d.summary()}", flush=True)
    print(f"产物目录 {shard_dir}", flush=True)
    return parts


def main() -> int:
    p = cli_for(collect_selfplay, "collect self-play data (closed loop)")
    # `--name` 只进缓存名（`collect_selfplay()` 没有这个形参）⇒ 不在签名里，得单独接。
    p.add_argument("--name", help="自定义名字（进缓存名最前面）")
    a = p.parse_args()
    kw = vars(a)
    name = kw.pop("name")
    kw["cache"] = kw["cache"] or default_cache(name=name, **kw)
    print(f"自对弈：{a.n} 项 × MCTS({a.sims}) 网={a.net_path} "
          f"探索={'π采样' if a.sample else ('eps=%g' % a.eps if a.eps > 0 else 'argmax')} "
          f"温度={a.temp_moves} 根噪声=ε{a.dirichlet_eps}/总浓度{a.dirichlet_total} "
          f"并肩={a.games} seed0={a.start_seed} → {kw['cache']}", flush=True)
    collect_selfplay(**kw)
    return 0


if __name__ == "__main__":
    sys.exit(main())

