"""自对弈采集：用网络自己的 PUCT 产训练数据（闭环的生成侧）。

    python collect_selfplay.py <每项场数> <sims> <net.pt> [输出] [掺入npz] [seed0=0] [--name X]

触发器：任务流取自 `collect_encs.generate_encs_jobs`（`task="encs"`），标签换成网络自己的 MCTS。
指纹与教师数据相同，产物可直接掺进教师数据混训（`mix_caches`）；训练侧只依赖 npz 格式。

只存 MCTS 根边的访问分布，而 `train()` 默认取 argmax，因此蒸馏的是 MCTS 的 argmax 动作。

探索必须开一个（`--sample` 或 `--eps`）：默认 argmax 确定性，且每轮复用同一批 seeds，
重跑会产出逐位相同的轨迹，闭环拿不到新状态。RNG 按 seed 派生，串行与并行一致。

⚠️ **树在一局内跨步复用**（`SearchTree`）：走完一步就把那个孩子变成新根，子树连统计一起保留
（AGZ 的 tree reuse）。每步仍然只跑 `sims` 次**新增**模拟 ⇒ 一局的模拟总数与不复用时相同，
但每一步都是在比上一步更深的树上继续搜（`sims` 相同、信息更多）。
"""
import argparse
import os
import sys
import time

import numpy as np
import torch

import ckpt
from collect import Src, Task, assemble, collect_jobs, load_cache, mix_caches, run_battle
from collect_encs import generate_encs_jobs as encs_jobs
from runtime import cache_name, cache_path, dataset_tag, net_tag, recipe_kv
from zspec import DEFAULT_SPEC, baseline_win_rate, z_of
from aznet import (AZNet, DIRICHLET_TOTAL, SearchTree, action_dim, inference_net, is_card_select_state,
                   legal_actions_and_idx, net_kwargs)

TASK = Task.ENCS

# 数据集弱于教师的告警阀值（胜率差，见 `collect_selfplay` 里的比较）。
WEAK_GAP = 0.03

# worker 全局；参数不可 pickle，由 initializer 装进每个进程。
_MP: dict = {}

# 探索开关现在是 `_MP` 里的**显式值**（由 `_init_worker` 从 initargs 装），不再是模块级全局。


def _explore_idx(rng, counts, legal_idx, sample, eps):
    """自对弈的动作选择，返回动作下标。

    `rng is None` 时取 `argmax(访问数)`。
    否则：以 `eps` 概率在合法动作里均匀取，否则 `sample` 开着就按访问分布采样，
    都没触发则回到 argmax。
    """
    if rng is not None:
        if eps > 0 and rng.random() < eps:
            return int(legal_idx[rng.integers(len(legal_idx))])
        if sample:
            p = np.array([counts.get(i, 0) for i in legal_idx], dtype=np.float64)
            if p.sum() > 0:
                return int(rng.choice(legal_idx, p=p / p.sum()))
    return int(max(counts.items(), key=lambda kv: kv[1])[0])


def _init_worker(cfg):
    """装网与参数（池的 initializer，串行路径也走它）。

    建网配置取自存档本身（`ckpt.config_of`），不读训练脚本的旋钮：权重能 load 但结构与
    存档不符时会得到静默错误的结果。
    """
    net_path, sims, batch, sample, eps, rollout_th, temp_moves, d_total, d_eps = cfg
    torch.set_num_threads(rollout_th)
    net_cfg = ckpt.config_of(net_path)
    if not net_cfg:
        raise RuntimeError(
            f"{net_path} 存档里没有建网配置（旧的两文件格式或裸权重）；"
            f"用 `python ckpt.py {net_path}` 查看，必要时 `ckpt.migrate`")
    net = AZNet(**net_kwargs(net_cfg))
    net.load_state_dict(ckpt.weights_of(net_path))
    net.eval()
    net = inference_net(net, intra_threads=rollout_th)      # 推理前向；线程数落到 torch
    _MP.clear()
    _MP.update(net=net, sims=sims, batch=batch, sample=sample, eps=eps,
               temp_moves=temp_moves, d_total=d_total, d_eps=d_eps)


def sp_battle(job):
    """MCTS 走完一场并记录每步的访问分布；`job` = `(seed, enc, floor)`。

    执行的动作就是本次搜索的 π 里抽出来的，标签与执行不可分离，都写在 `label` 里。
    """
    seed, enc, floor = job
    net, sims, batch = _MP["net"], _MP["sims"], _MP["batch"]
    sample, eps = _MP["sample"], _MP["eps"]
    temp_moves, d_total, d_eps = _MP["temp_moves"], _MP["d_total"], _MP["d_eps"]
    # 探索擉币 / 根噪声的专有流；四个开关都关时不消费随机数。
    rng = (np.random.default_rng(seed * 1000003 + 17)
           if (sample or eps > 0 or temp_moves > 0 or d_eps > 0) else None)

    # 一棵树服务一场（`run` 会校验真实状态与树根逐位相同；对不上就报错）。
    tree = None
    step = 0            # 已走过的决策点数：温度按它衰减

    def label(b):
        nonlocal tree, step
        cand = legal_actions_and_idx(b)
        if tree is None:
            tree = SearchTree(b, net, batch_size=batch, dirichlet_total=d_total,
                              dirichlet_eps=d_eps, rng=rng)
        counts, actions = tree.run(b, sims)
        pi = np.zeros(action_dim(b), dtype=np.float32)
        mask = np.zeros(action_dim(b), dtype=bool)
        for _, i in cand:
            mask[i] = True
        for i, n in counts.items():
            pi[i] = n
        # 温度（AGZ）：前 `temp_moves` 个决策点 τ=1，按访问数比例采样以铺开探索面；
        # 之后 τ→0 取 argmax，出最强的招式。`sample` 开着则全程采样。
        idx = _explore_idx(rng, counts, [i for _, i in cand],
                           sample or step < temp_moves, eps)
        step += 1
        # `pi.sum() > 0` 由 `sims ≥ 2` 保证（`collect_selfplay` 入口已挡）—— 这里不兜底。
        # 先推进树再交回动作：`run_battle` 会在返回后执行 `actions[idx]`，与 `advance` 一致。
        tree.advance(idx)
        return pi / pi.sum(), mask, is_card_select_state(b), actions[idx], None

    return run_battle(seed, enc, floor, label)


# ---------------------------------------------------------------- 缓存名

# 不进缓存名的参数：`cache` / `cache_dir` 是**输出路径**；`verbose` 是日志；
# `workers` / `mp_start` 是并行方式（并行结果与串行逐位相同，见 `_mp_test.py`）；
# `net_path` / `mix_with` 是路径 —— 它们进名的是**身份**（见下），不是路径本身。
# `rollout_threads` **要进名**：它改 torch 线程数 ⇒ 改矩阵乘的归约顺序，属配方（同 `train.threads`）。
NAME_DROP = ("cache", "name", "verbose", "workers", "mp_start", "cache_dir", "net_path", "mix_with")


def default_cache(n: int, sims: int, net_path: str, batch: int = 1, start_seed: int = 0,
                  sample: bool = False, eps: float = 0.0, mix_with: str | None = None,
                  rollout_threads: int = 1, temp_moves: int = 0,
                  dirichlet_total: float = 0.0, dirichlet_eps: float = 0.0,
                  name: str | None = None,
                  cache_dir: str | None = None) -> str:
    """默认缓存路径 —— **唯一**拼名点。全部配方参数（含默认值）都在名里。

    ⚠️ 旧写法只写 `n` / `sims`：换起点权重、`--eps`、`--sample`、`--mcts-batch` 任意一项都会
    **命中同一份旧缓存**（`main()` 先拼名，`load_cache` 见文件存在就直接返回）⇒ 静默复用了
    别的配方采的数据。
    """
    kv = recipe_kv(collect_selfplay,
                   dict(n=n, sims=sims, net_path=net_path, batch=batch, start_seed=start_seed,
                        sample=sample, eps=eps, mix_with=mix_with,
                        rollout_threads=rollout_threads, temp_moves=temp_moves,
                        dirichlet_total=dirichlet_total, dirichlet_eps=dirichlet_eps), NAME_DROP)
    n_, s = kv.pop("n"), kv.pop("sims")
    # 路径类参数只留人给的名字：起点权重 = 它的自定义部分 + **内容指纹**（同一个名字训两遍是
    # 两份不同的网，光看名字分不出）、掺入数据 = 它的数据引用（`/` 进不了文件名）。
    extra = {"net": net_tag(net_path), "wfp": ckpt.read(net_path)["fp"]}
    if mix_with:
        extra["mix"] = dataset_tag(mix_with)
    return cache_path(cache_name(name, "spencs", n_, s, **kv, **extra), cache_dir)


def collect_selfplay(n, sims, net_path, cache=None, batch=1, start_seed=0, mix_with=None,
                     verbose=True, sample=False, eps=0.0, workers=None, mp_start=None,
                     rollout_threads=1, cache_dir=None,
                     temp_moves=0, dirichlet_total=DIRICHLET_TOTAL, dirichlet_eps=0.0):
    """产自对弈数据：`n` = 每项场数（总场数 = 表项数 × `n`）。

    `mix_with` 为要掺入的 npz，拼接规则见 `collect.mix_caches`（含 `bid` 错开）。
    旋钮全部走形参：`batch`（MCTS 叶批量）/ `sample` / `eps` / `workers` / `mp_start` /
    `rollout_threads`（每个 worker 的 torch 线程数）/ `cache_dir`。
    """
    if not os.path.exists(net_path):
        sys.exit(f"找不到网络权重 {net_path}")
    if sims < 2:
        # 第 1 次迭代只展开根、不访问任何根边 ⇒ π 全零（`azsearch` 的 `sims ≥ 2` 契约）。
        sys.exit(f"sims 太小：{sims}（至少 2，否则本场搜不出可记的 π）")
    got = load_cache(cache, Src.MCTS)
    if got is not None:
        # 命中已有缓存也要报路径：调用方（`_chain.sh`）靠这一行拿到数据位置，
        # 静默返回会让"复用"看起来像"采集没报路径"。
        print(f"已缓存到 {cache}", flush=True)
        return got
    jobs = encs_jobs(n, start_seed)

    t0 = time.perf_counter()
    results, wins = collect_jobs(jobs, sp_battle, f"自对弈 MCTS({sims})",
                                 initializer=_init_worker,
                                 initargs=((net_path, sims, batch, sample, eps,
                                            rollout_threads, temp_moves, dirichlet_total,
                                            dirichlet_eps),),
                                 verbose=verbose, workers=workers, mp_start=mp_start)
    data = assemble(results, TASK, Src.MCTS)
    print(f"\n自对弈完成：{len(results)} 场 / {sum(len(r.traj) for r in results)} 决策点 / "
          f"{len(data['obs'])} 样本；胜率 {wins}/{len(results)} = {wins / len(results):.1%}；"
          f"MCTS({sims}) batch={batch} 探索="
          f"{'π采样' if sample else ('eps=%g' % eps if eps > 0 else 'argmax')}；"
          f"{time.perf_counter() - t0:.0f}s", flush=True)
    # 缓存不存 `z`：这里只是把**默认口径**下的值目标分布打出来看一眼（训练侧的口径自己选）。
    zt = z_of(data, DEFAULT_SPEC)
    print(f"  z({DEFAULT_SPEC}): mean {zt.mean():+.3f} var {zt.var():.3f}", flush=True)

    # 数据集质量告警：自对弈胜率明显低于教师基准 ⇒ 本轮数据弱于教师（只报不改）。
    # 两边同口径（同一张 ENCS 表、同一“赢下遭遇”定义），但本侧有温度/根噪声压低胜率、
    # 且两边 seed 段不同 ⇒ 留 `WEAK_GAP` 余量，小差距不报。
    bl = baseline_win_rate()
    wr = wins / len(results)
    if bl is not None and wr + WEAK_GAP < bl[1]:
        print(f"⚠️ 自对弈胜率 {wr:.1%} 明显低于教师基准 {bl[0]} = {bl[1]:.1%}（差 "
              f"{bl[1] - wr:.1%} > {WEAK_GAP:.0%}）⇒ 本轮数据集弱于教师：先查策略是否退化、"
              f"探索噪声是否过大、教师基准是否换了规格", flush=True)

    if mix_with:
        md = np.load(mix_with)
        data = mix_caches({k: md[k] for k in md.files}, data,
                          where=f"掺入 {os.path.basename(mix_with)}：")
    if cache:
        np.savez(cache, **data)
        print(f"已缓存到 {cache}", flush=True)
    return data


def main() -> int:
    p = argparse.ArgumentParser(description="用网络自己的 MCTS 产训练数据（闭环）",
                                epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("n", type=int, help="每项场数")
    p.add_argument("sims", type=int, help="MCTS 搜索预算")
    p.add_argument("net", help="起点网络权重")
    p.add_argument("cache", nargs="?", default=None, help="输出 npz（默认自动命名）")
    p.add_argument("mix", nargs="?", default=None, help="掺入的 npz（防遗忘）")
    p.add_argument("seed0", nargs="?", type=int, default=0, help="种子段起点")
    p.add_argument("--sample", action="store_true", help="按访问分布 π 采样（默认 argmax）")
    p.add_argument("--eps", type=float, default=0.0, help="均匀探索概率（0 = 关）")
    p.add_argument("--mcts-batch", type=int, default=1, help="MCTS 叶评估批量（1 = 与历史等价）")
    p.add_argument("--workers", default=None,
                   help="并行 worker 数；'serial'/0 关闭并行；不给 = 按 cgroup 配额")
    p.add_argument("--mp-start", default=None, help="multiprocessing 启动方式（fork/spawn）")
    p.add_argument("--rollout-threads", type=int, default=1, help="每个 worker 的 torch 线程数")
    p.add_argument("--cache-dir", default=None, help="缓存目录（默认 <repo>/data）")
    p.add_argument("--name", default=None, help="自定义名字（进缓存名最前面）")
    p.add_argument("--temp-moves", type=int, default=0,
                   help="前多少个决策点用 τ=1 采样（0 = 关；AGZ 用 30）")
    p.add_argument("--dirichlet-total", type=float, default=DIRICHLET_TOTAL,
                   help="根节点 Dirichlet 的**总浓度**（每动作 α = 总浓度/合法动作数；围棋 0.03×361=10.83）")
    p.add_argument("--dirichlet-eps", type=float, default=0.0,
                   help="根噪声权重 ε（0 = 关；AGZ 用 0.25）")
    a = p.parse_args()

    cache = a.cache or default_cache(n=a.n, sims=a.sims, net_path=a.net, batch=a.mcts_batch,
                                    start_seed=a.seed0, sample=a.sample, eps=a.eps,
                                    mix_with=a.mix, rollout_threads=a.rollout_threads,
                                    temp_moves=a.temp_moves,
                                    dirichlet_total=a.dirichlet_total,
                                    dirichlet_eps=a.dirichlet_eps,
                                    name=a.name, cache_dir=a.cache_dir)
    print(f"自对弈：{a.n} 项 × MCTS({a.sims}) 网={a.net} "
          f"探索={'π采样' if a.sample else ('eps=%g' % a.eps if a.eps > 0 else 'argmax')} "
          f"温度={a.temp_moves} 根噪声=ε{a.dirichlet_eps}/总浓度{a.dirichlet_total} "
          f"seed0={a.seed0} → {cache}", flush=True)
    collect_selfplay(a.n, a.sims, a.net, cache=cache, batch=a.mcts_batch,
                     start_seed=a.seed0, mix_with=a.mix, sample=a.sample, eps=a.eps,
                     workers=a.workers, mp_start=a.mp_start,
                     rollout_threads=a.rollout_threads, cache_dir=a.cache_dir,
                     temp_moves=a.temp_moves, dirichlet_total=a.dirichlet_total,
                     dirichlet_eps=a.dirichlet_eps)
    return 0


if __name__ == "__main__":
    sys.exit(main())
