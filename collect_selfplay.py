"""自对弈采集：用网络自己的 PUCT 产训练数据（闭环的生成侧）。

    python collect_selfplay.py --n <每项场数> --sims <sims> --net-path <net.pt> \\
                              [--cache 输出目录] [--start-seed 0]

⚠️ flag 由 `runtime.cli_for(collect_selfplay)` 按**签名**注册 ⇒ 这里没有「flag → 实参」的第二张
   表。逐 flag 的说明也只在**这里**（`--help` 把整段原样印出来），不另设一张表：

    --n                 每项场数（总场数 = `encs` 表项数 × n）
    --sims              MCTS 搜索预算（≥2；第 1 次迭代只展开根、不访问根边）
    --sims-cheap-frac   便宜档 = `--sims` 的**占比**（max(2, round(sims×它))；0 = 关 = 恒用 `--sims`）。
                        多数决策点跑便宜档，配 `--sims-full-frac` 让少量跑完整预算
    --sims-full-frac   走完整 `--sims` 的决策点比例（仅 `--sims-cheap-frac > 0` 时生效）
    --long-decay-start **长局衰减**的起点步数（0 = 关 = 在役基线）：第 N 步之后预算 ∝ `N/步数`
                        （下限 32）⇒ 卡死局（646/1000 步那种，正常场 p90 才 36 步）的边际成本降到 1/10。
                        动机：2016 场那轮 **70% 的墙钟**耗在最后 7 场长局上，而它们只占 **6.6% 的行**，
                        且都是"没打完"的废帧（尾段只剩 1~2 条链在走 ⇒ 3.6M 次前向串行 1056 s）。
                        ⚠️ 不丢帧（每步照记），只把那些步的搜索变浅；关着时逐位不变
    --net-path          起点网络权重
    --cache             产物**目录**（不给 = 默认目录 `<cache-dir>/spencs`）
    --pool              **池宽** = 在飞的场数（跑完一场立刻补一场）。一棵树同时只挂 1 个前向请求
                        ⇒ 这也是**前向批宽的上限**；任务表大小由 `--n` 定，与它无关。实测（探针）：
                        512 ⇒ 202 s / 均批 79；1024 ⇒ 214 s / 83.6；整表全开 ⇒ 209 s / 84.2
    --workers           worker 线程数（0 = 硬件并发数；树**不绑定线程**，推一步就回池）
    --start-seed        种子段起点（第 i 场用 `start_seed + i`）。⚠️ **链条每轮都要换一段**：
                        `collect.round_seed0(轮号, n)` 给的是**确定性、互不重叠**的段（恒 <
                        `EVAL_SEED0`）；一直传 0 = 每轮都在同一批开局上自对弈
    --sample/--no-sample 按访问分布 π 采样（默认 argmax）
    --eps               均匀探索概率（0 = 关）
    --cache-dir         默认产物目录的父目录（默认 `data/`）
    --temp-moves        前多少个决策点用 τ=1 采样（0 = 关；AGZ 用 30）
    --dirichlet-total   根节点 Dirichlet 的**总浓度**（每动作 α = 总浓度/合法动作数）
    --dirichlet-eps     根噪声权重 ε（0 = 关；AGZ 用 0.25）。⚠️ **每个新决策点**都重混一次
                        （子树提升为新根时也混，不只每场的第一个决策点）
    --shard-rows        每多少行落一个分片（只影响粒度，**不进配方**）

    ⚠️ **尾段没有旋钮**：已交付过至少一场、且在飞场数 ≤ `--workers` 时，worker 用自己的 CPU
    会话就地前向（会话懒建，见 `collect/selfplay.cpp` 的 `tail_mode_`）；CPU 存档用
    `SelfplayConfig::model_path_cpu`（空 = 用主存档 + CPU）。

⚠️ **产物名不带配方**（2026-10-15 定案，`runtime.artifact_dir` / `check_recipe`）：默认目录就是
    `<cache-dir>/spencs`，**没有自定义名字**（`--name` 已删）—— 要落到别处就 `--cache <目录>`。
    配方写进**产物内部**的 `_recipe.json`：复用一个已有目录前会**逐字比对**，不一致当场报错
    （同名目录不许装两份配方，也绝不复用）；换 `shard_rows` 不算换配方（行序与字节都不变）。
    ⇒ 从名字看不出配方，看 `_recipe.json`、存档或**本脚本打的那行配方**（采集前打一次）。

⚠️ 任务表 / 造局面 / 搜索 / 落盘**全在 C++**（`sts.selfplay_run`）—— 这里只当**触发器**：
   装配方 + 打一行统计（统计由 `CollectData.summary()` 现算）。
   `--mp-start` **已删**（2026-10-13）：触发器在**本进程**里跑完整段，没有多进程池，也就不再有
   "多进程各带一个 CUDA 上下文"那类问题。`--workers` 现在是**C++ 侧的 worker 线程数**（上面那条）。

只存 MCTS 根边的访问分布，而 `train()` 默认取 argmax，因此蒸馏的是 MCTS 的 argmax 动作。

探索必须开一个（`--sample` 或 `--eps`）：默认 argmax 确定性，且每轮复用同一批 seeds，
重跑会产出逐位相同的轨迹，闭环拿不到新状态。

⚠️ **W 条 worker 线程 + 池宽 `pool` 棵树在飞**（`collect/selfplay.cpp`，与 `teacher.cpp` 的 worker 池
   同形）：每棵树是一台可挂起的状态机（推一步就回池，谁抢到谁继续），碰上要前向就把请求塞进池、
   不占线程，活交给前向服务线程（`NNEval`，KataGo `NNEvaluator::evaluate` 同形）⇒ 批宽受 `pool` 管、
   线程数由 `--workers` 管，两者互不牵制。⚠️ **K>1 不再逐位可复现**（批的组成由到达时序决定，而前向
   与批宽相关），**K=1** 仍与冻 golden 逐位对拍。
"""
import os
import sys
import time

import numpy as np
import torch

import ckpt
from collect import CollectData, MAX_STEPS
from runtime import artifact_dir, cli_for, recipe_kv, reuse_shards
from netarchive import ensure_net
from zspec import DIRICHLET_TOTAL, DEFAULT_SPEC, c_puct_for, q_init_for

# 产物**种类**（默认目录名 = 它；也是 `_recipe.json` 里的 `kind`）。
KIND = "spencs"

# 不进**配方**的形参（改了它们数据仍逐位相同）：
#   · `cache` / `cache_dir` 是**输出路径**（`--cache` 给的就是目录）
#   · `shard_rows` 只改分片粒度（行序/字节都不变，`CollectData` 跨片读）
#   · `net_path` 是**路径**：网络进配方的是**内容**（指纹 + 推理后端，见 `recipe_of`）——
#     同一份权重拷到别的路径仍是同一个网，不该判成"配方不同"
# 其余**全部形参**都进配方（`recipe_kv` 按签名取，缺一个当场 KeyError）：
#   · `pool` 决定"一次前向吃多少行"（跨树合批）⇒ GEMM 归约顺序随批宽变（实测 K=1 与 K=4 的 π
#     逐位相同，但不保证换宽后仍如此）
#   · `workers` 改"谁在跑"⇒ 改批的组成
RECIPE_DROP = ("cache", "cache_dir", "shard_rows", "net_path")


def recipe_of(kw: dict) -> dict:
    """本次采集的**配方**（写进 `_recipe.json`）——`collect_selfplay()` 的全部形参 + 网络身份。

    ⚠️ `locals()` 交给 `recipe_kv`：那一行只有形参 ⇒ **不可能漏登记新形参**（漏了反而 KeyError）。
    网络身份 = **权重内容指纹**（同一个路径可能装着两份不同的网）+ **推理引擎**
    （现在恒 `onnx`；留着是因为换引擎 = 换数值 ⇒ 旧产物应当与新的区分开）。
    ⚠️ 路径**不进**配方（同一份权重换个路径还是同一个网，判成不同只会误伤复用）；
    它只出现在日志里（`main()` 那行 `网=<path>`）。
    """
    from netarchive import backend as _backend

    r = recipe_kv(collect_selfplay, kw, RECIPE_DROP)
    r["net"] = {"fp": ckpt.read(kw["net_path"])["fp"], "be": _backend()}
    return r


def collect_selfplay(n: int, sims: int, net_path: str, cache=None, pool=8, workers=0,
                     start_seed=0,
                     sample=False, eps=0.0, device="cpu", cache_dir=None,
                     temp_moves=0, dirichlet_total=DIRICHLET_TOTAL, dirichlet_eps=0.0,
                     sims_cheap_frac=1.0 / 6.0, sims_full_frac=0.25, long_decay_start=0,
                     hard_only=False,
                     shard_rows=200000) -> list[str]:
    """产自对弈数据：`n` = 每项场数（总场数 = 表项数 × `n`）；返回**分片路径**。

    旋钮的说明在**模块** docstring 里（`--help` 会印出来），这里不重复一遍。

    ⚠️ 树内批已删：`pool` = 在飞的场数 = **前向批宽的上限**（树内恒 1 条路径），不改变任何一棵树的
    搜索语义（实测 K=1 与 K=4 的 π 逐位相同；但**换批宽会改 GEMM 归约顺序** ⇒ `pool` 进产物名）。
    ⚠️ `workers` = **跑这些树的线程数**（0 = 硬件并发数）：树不绑定线程（推一步就回池，谁抢到谁继续），
    碰上前向就把请求塞进池、不占线程 ⇒ 线程数由它管、批宽由 `pool` 管，两者互不牵制。
    ⚠️ `device`（`cpu` / `cuda`）是**配方**（进产物名）：CUDA 的存档要单独导一份（`netarchive`），
    且 CUDA 核非确定 ⇒ 同一份权重在两种设备上不是同一条轨迹。

    整段（任务表 / 造局面 / 搜索 / 落盘）在 C++ 的 `sts.selfplay_run` 里 —— 这里只装配方。
    """
    import slaythespire as sts

    # 配方在**函数开头**取：此刻 `locals()` 里除了形参只有 `sts`（`recipe_kv` 只读签名里的键，
    # 多出来的键无害）；放开头 = 后面几个局部量（`got`/`shard_dir`…）不会混进来。
    recipe = recipe_of(locals())

    if not os.path.exists(net_path):
        sys.exit(f"找不到网络权重 {net_path}")
    if sims < 2:
        # 第 1 次迭代只展开根、不访问任何根边 ⇒ π 全零（`azspec` 的 `sims ≥ 2` 契约）。
        sys.exit(f"sims 太小：{sims}（至少 2，否则本场搜不出可记的 π）")
    if not (0.0 <= sims_cheap_frac < 1.0):
        sys.exit(f"sims_cheap_frac 要在 [0, 1) 里（现在是 {sims_cheap_frac}）：0 = 关，其余 = 便宜档/sims")
    # 日志用：与 C++ `cheapBudget()` 同一公式
    cheap_sims = min(sims, max(2, int(sims * sims_cheap_frac + 0.5))) if sims_cheap_frac > 0 else 0
    shard_dir = cache or artifact_dir(KIND, cache_dir)
    got = reuse_shards(KIND, shard_dir, recipe)  # 空壳清理 + 配方闸 + 命中复用（各打一行）
    if got:
        return got

    # 前向在 C++ 侧 ⇒ 先备一份本设备的存档（`netarchive.ensure_net` 是唯一入口，
    # **按设备**各一份）—— ONNX Runtime 认 `.onnx`。
    ts = ensure_net(net_path, device)
    # 尾段用 CPU 会话：主设备不是 CPU 时才单独导一份 CPU 存档（各几百 MB，别白导）。
    cpu_ts = "" if str(device) in ("", "cpu") else ensure_net(net_path, "cpu")

    t0 = time.perf_counter()
    cfg = sts.SelfplayConfig()
    cfg.model_path = ts
    cfg.device = str(device)        # 空/"cpu" = CPU；"cuda[:k]" = 卡上（存档必须是同设备 trace 的）
    # 尾段（worker 就地前向）：空路径 = 用主存档 + CPU。判据与时机都在 C++（`tail_mode_`）。
    cfg.model_path_cpu = cpu_ts

    cfg.z_spec = DEFAULT_SPEC
    cfg.sims = int(sims)
    cfg.sims_cheap_frac = float(sims_cheap_frac)  # 0 = 关（恒用 `sims`）；否则 便宜预算 = max(2, round(sims×它))
    cfg.full_frac = float(sims_full_frac)         # 走完整预算的决策点比例
    # **长局衰减**：从第 `long_decay_start` 步起，预算 ∝ `start/步数`（下限 32）⇒ 卡死局（几百上千步）
    # 的边际成本从"每步一整个预算"降到 1/10。动机见 `collect/selfplay.h` 的 `long_decay_start`：
    # 2016 场那轮 **70% 的墙钟** 耗在最后 7 场（646/1000 步、只占 6.6% 的行）。
    cfg.long_decay_start = int(long_decay_start)
    cfg.max_steps = MAX_STEPS
    cfg.pool = int(pool)            # 池宽 = 在飞的场数（跑完一场立刻补一场；= 批宽上限）
    cfg.workers = int(workers)      # 跑这些树的线程数（0 = 硬件并发数；批宽不受它影响）
    cfg.c_puct = c_puct_for(DEFAULT_SPEC)
    cfg.q_init = q_init_for(DEFAULT_SPEC)
    cfg.sample = bool(sample)
    cfg.eps = float(eps)
    cfg.temp_moves = int(temp_moves)
    cfg.dirichlet_total = float(dirichlet_total)
    cfg.dirichlet_eps = float(dirichlet_eps)
    parts = [str(p) for p in sts.selfplay_run(int(n), cfg, int(start_seed), shard_dir,
                                             int(shard_rows), bool(hard_only))]
    if not parts:
        sys.exit(f"采集没有产出任何分片 ⇒ 看 {shard_dir}")
    # 预算读数：随机化开着时把两档与比例都报出来（否则日志里只有 `sims`，看不出是混合预算）。
    budget = (f"MCTS({cheap_sims}×{1.0 - sims_full_frac:.0%} + {sims}×{sims_full_frac:.0%})"
              if 0 < cheap_sims < sims else f"MCTS({sims})")
    if long_decay_start > 0:
        budget += f"·长局{sims // max(1, long_decay_start)}×衰减(≥{long_decay_start}步)"
    print(f"\n自对弈完成：{len(parts)} 个分片；{budget} 池宽 {pool}（{device}） 探索="
          f"{'π采样' if sample else ('eps=%g' % eps if eps > 0 else 'argmax')}；"
          f"尾段=worker 就地前向 "
          f"{time.perf_counter() - t0:.0f}s", flush=True)    # 不存 `z`：这里只是把产物打一行统计（训练侧的口径自己选）。
    d = CollectData(shard_dir)
    print(f"  {d.summary()}", flush=True)
    print(f"产物目录 {shard_dir}", flush=True)
    return parts


def main() -> int:
    p = cli_for(collect_selfplay, "collect self-play data (closed loop)")
    a = p.parse_args()
    kw = vars(a)
    # 产物目录：`--cache` 给的就是它；不给 = `<cache-dir>/spencs`（名字里不带配方）。
    kw["cache"] = kw["cache"] or artifact_dir(KIND, kw["cache_dir"])
    print(f"自对弈：{a.n} 项 × MCTS({a.sims}，便宜档 {a.sims_cheap_frac:.0%} / 完整档 {a.sims_full_frac:.0%}) "
          f"网={a.net_path} 前向设备={a.device} "
          f"探索={'π采样' if a.sample else ('eps=%g' % a.eps if a.eps > 0 else 'argmax')} "
          f"温度={a.temp_moves} 根噪声=ε{a.dirichlet_eps}/总浓度{a.dirichlet_total} "
          f"尾段=worker 就地前向 "
          f"池宽={a.pool} seed0={a.start_seed} → {kw['cache']}",
          flush=True)
    collect_selfplay(**kw)
    return 0


if __name__ == "__main__":
    sys.exit(main())

