"""训练 AZNet。本文件只做训练：采集在 `collect_teacher.py` / `collect_selfplay.py`（都在 C++ 里生产）。

用法：
  python train.py train <数据路径> [epochs] [权重输出] [--lr 1e-3 …] [--mix <路径>=<权重> …]
  python train.py seeds <数据路径> [种子数] [--seed0 3 --save 'data/n1_s{seed}.pt']
（配方 flag 清单见 `python train.py train --help`）

⚠️ `--pi-objective pg` 与"软目标"都从**同一份数据**现算（`collect.CollectData`），换口径不改数据。

契约：
  · 策略目标用硬 argmax（`--soft-target` 只为复现旧结果保留；理由与数字见 `.agent/notes.md`）。
  · `seeds` 默认不存盘，给了 `--save` 才按种子保存。
  · **建网超参不是配方**：固定在 `aznet.py` 的模块常量里 ⇒ 无 CLI flag、不进名字（换值 = 换网）；
    存档仍记 `config`，`check_init()` 拿它挡旧架构的权重。
  · 配方（优化、值口径、线程、设备、打点）只来自 `train()` 的形参，不读环境变量：
    flag 由 `recipe_flags()` 从签名派生（连默认值一起），参数由 `train_kwargs()` 原样
    转交 `train()`，名字对不上就当场 `TypeError`。
  · 线程数属于配方：换了线程数轨迹就变，A/B 两个方案必须同值。
  · 种子纪律：训练段、采集段、自对弈段都不得与评估段（从 `collect.EVAL_SEED0` = 1e8 起）重叠。
  · 数据格式 = `collect.CollectData` 认的采集产物（**分片目录**）。自对弈与教师数据同格式，
    可以直接混训。
  · **产物名不带配方**（2026-10-15）：权重名 = `<数据目录名>.pt`（`out` 位置参数可显式给路径），
    **没有 `--name`**；配方（数据 + 混训 + 全部超参）写进**存档内部**的 `recipe` 字段
    ⇒ 不看名字也能复现。对应地，采集产物目录是固定的 `<cache-dir>/<kind>` + 目录内
    `_recipe.json`（见 `runtime` 顶部与 `collect_selfplay`）。
  · **回放**（`--mix`）：位置参数那份是**本轮**（恒 1.0），`--mix` 的权重都相对它 ⇒
    `--mix 上一轮=0.6 --mix 再上一轮=0.36` ≈ 50.9%/30.5%/18.5%。一轮的优化步数**不变**
    （行数 = 本轮的训练行数），所以"挂回放"与"多训几步"不会混在一起。
"""
import argparse
import inspect
import math
import os
import sys
import threading
import time
from typing import Any

import numpy as np
import torch
from tqdm import tqdm

import ckpt
import metrics
from aznet import AZNet, POLICY_DIM, net_cfg
from collect import CollectData, CollectMix
from runtime import cache_path, mp_context
from zspec import DEFAULT_SPEC

# ---------------------------------------------------------------- 配方
# 配方 flag 直接从 `train()` 的签名派生：名字、类型、默认值全读签名，不另抄一张表。
# `HELP` 只是 `--help` 的说明文字，写漏顶多少一句提示，不影响取值。
HELP: dict[str, str] = {
    "lr": "学习率",
    "wd": "权重衰减",
    "batch": "batch 大小",
    "epochs": "训练轮数",
    "z_spec": "值口径：hp / hp_turn / enginenorm（⚠️ enginenorm 要 `t_escore` 列，v3 落盘没有 ⇒ 只能采集、不能训）",
    "soft_target": "策略目标用软分布（默认硬 argmax）",
    "soft_temp": "软目标的温度：π**(1/T)，<1 更尖、>1 更平（仅 --soft-target 下生效）",
    "pi_weight": "π 损失的逐行权重口径：none（默认，每行等权）/ advantage（max(0, z−V̂) 归一）",
    "pi_objective": "策略损失口径：off（默认，模仿教师 π 的交叉熵）/ pg（策略梯度，只回传实际执行的动作）",
    "loss_chunk": "算指标的分块行数",
    "threads": "torch 线程数（属于配方，会改轨迹）",
    "device": "cpu / cuda[:k]",
    "record": "读数落盘开关（0/1）",
    "record_dir": "读数根目录（默认 data/runs）",
    "init": "续训起点权重（`ckpt.save` 的存档）；不设 = 随机初始化",
}
# `pi_weight` 的合法口径：`none` = 每行等权（历史行为，逐位不变）；`advantage` = 优势加权。
PI_WEIGHT_MODES: tuple[str, ...] = ("none", "advantage")
# `pi_objective` 的合法口径：`off` = 模仿教师 π（历史行为）；`pg` = 策略梯度。
PI_OBJECTIVE_MODES: tuple[str, ...] = ("off", "pg")
# 子命令里不属于配方的名字：这些不进 `train()`。新增一个 flag 而忘了登记（或者名字拼错），
# 都会在 `train(**kw)` 那一步当场 TypeError —— 就是要它炸。
JOB_KEYS = frozenset({"mode", "cache", "pos_epochs", "out", "job_timeout",
                      "n_seeds", "seed0", "save", "mp_start", "mix"})


def parse_mix(specs: list[str] | None) -> list[tuple[str, float]]:
    """`--mix <路径>=<权重>` → `[(路径, 权重)]`；权重是**相对本轮（恒 1.0）**的比。

    ⚠️ 分隔符是 `=`：路径里不会有它（`:` 会有 —— 挂载点 / 驱动器号）。写法不对当场退出。
    """
    out: list[tuple[str, float]] = []
    for s in specs or []:
        path, sep, w = s.rpartition("=")
        if not sep or not path:
            raise SystemExit(f"❌ --mix 要写成 <路径>=<权重>：{s!r}")
        try:
            wv = float(w)
        except ValueError:
            raise SystemExit(f"❌ --mix 的权重不是数：{s!r}")
        if not wv > 0:
            raise SystemExit(f"❌ --mix 的权重必须为正（0 = 别给这份）：{s!r}")
        out.append((path, wv))
    return out

def load_data(cache: str, mix: list[str] | None):
    """位置参数那份（**本轮**）+ `--mix` 各份 → 训练用的数据集（单份 = `CollectData`，逐位不变）。"""
    parts = [(cache, 1.0)] + parse_mix(mix)
    return (CollectData(cache) if len(parts) == 1 else CollectMix(parts)), parts


def recipe_flags(sp: argparse.ArgumentParser) -> None:
    """按 `train()` 的签名注册配方 flag（类型与默认值也从签名来，`--help` 会显示）。"""
    for k, p in inspect.signature(train).parameters.items():
        if p.kind is not inspect.Parameter.KEYWORD_ONLY or k == "epochs":
            continue                    # `epochs` 由两个子命令各自加（`train` 是位置参数）
        kw: dict[str, Any] = dict(default=p.default, help=HELP.get(k, ""))
        if k == "record":
            kw |= dict(type=int, choices=(0, 1))     # 布尔 flag 表达不了 0/1
        elif isinstance(p.default, bool):
            kw |= dict(action="store_true")
        else:
            kw |= dict(type=str if p.default is None else type(p.default))
        sp.add_argument("--" + k.replace("_", "-"), **kw)


def train_kwargs(cli: argparse.Namespace) -> dict[str, Any]:
    """命令行里的配方参数，原样交给 `train()`；名字对不上就让它自己 TypeError。"""
    kw = {k: v for k, v in vars(cli).items() if k not in JOB_KEYS}
    if kw.get("epochs") is None:        # `train` 子命令里 epochs 是位置参数，namespace 里没有
        kw["epochs"] = inspect.signature(train).parameters["epochs"].default
    if "record" in kw:
        kw["record"] = bool(kw["record"])      # flag 只有 0/1，`train()` 要 bool
    return kw


# check_init() 要比对的键 = net_cfg() 的键（键名只在 net_cfg() 里写一份）
NET_KEYS = tuple(net_cfg())


def _stem(path: str | None) -> str:
    """取**目录名/文件名主干**当 TensorBoard 运行名（名字本身就是短的，直接用它）。"""
    return os.path.splitext(os.path.basename(os.path.normpath(path or "")))[0]


# ------------------------------------- 训练阶段的任务级并行
# 同时训练几个 seed。每个子进程只用 1 个 torch 线程，这样跨进程结果才完全一致。
# 取 3 与以往实验的对照条件一致；改它就等于换条件。
PAR_TRAIN = 3
# fork 出来的子进程能看到这个模块级 dict，`data` 因此靠写时复制共享（当参数传会被 pickle 一份）。
# 这也说明并行依赖 fork：`--mp-start spawn` 时子进程拿到空 dict，`seeds` 并行就不成立。
_PAR_DATA: dict[str, Any] = {}


def _train_seed(job: tuple[int, str, dict[str, Any]]
                ) -> tuple[int, dict[str, torch.Tensor], dict[str, float]]:
    """子进程里的单 seed 训练（`job = (seed, tag, kw)`），返回 `(seed, state_dict, metrics)`。

    必须 `threads=1`：线程数属于配方，换了轨迹就变；跨进程也只在同线程数下结果一致。
    """
    seed, tag, kw = job
    assert kw["threads"] == 1, (
        f"并行训练必须 threads=1（当前 {kw['threads']}）——"
        "换线程数就换轨迹，A/B 对比不成立")
    net, m = train(_PAR_DATA["data"], seed=seed, verbose=False, tag=tag, **kw)
    return seed, {k: v.cpu() for k, v in net.state_dict().items()}, m


# ---------------------------------------------------------------- 训练
def _first_nonfinite(net: AZNet) -> str | None:
    """返回第一个含 NaN/Inf 的参数名，全部有限时返回 None。发散检查与存盘检查共用。"""
    for name, p in net.named_parameters():
        if not bool(torch.isfinite(p).all()):
            return name
    return None


def save_ckpt(net: AZNet, path: str, *, cfg: dict[str, Any], z_spec: str,
              recipe: dict | None = None, **metrics: float) -> str:
    """把权重和建网配置写进同一个文件（`ckpt.save`），返回存档路径。

    `net` 必须在 CPU 上；`cfg` 是 `net_cfg()` 的结果；`**metrics` 是训练指标，写进存档备查。
    `recipe` = 本次训练的**完整配方**（数据 + 混训权重 + 全部超参）：权重名不再带配方（`runtime`
    顶部那段）⇒ 它就是"不看名字也能复现"的凭据（连同 `cfg` / `z_spec` / `metrics`）。
    含 NaN/Inf 的权重会拒绝存盘并抛 `RuntimeError`：这种权重形状合法、能 `strict=True`
    加载，但评估恒为 0%，属于不报错的坏结果，必须在源头拦住。
    """
    bad = _first_nonfinite(net)
    if bad is not None:
        msg = f"拒绝存盘 {path}：参数 `{bad}` 含 NaN/Inf —— 训练已发散。"
        metrics.fault("save_nonfinite", msg)
        raise RuntimeError(msg)

    return ckpt.save(path, net.state_dict(), cfg,
                      space=POLICY_DIM, z_spec=z_spec, recipe=recipe,
                      metrics={k: float(v) for k, v in metrics.items()})


# ---------------------------------------------------------------- 训练
def train(data, seed: int = 0, val_frac: float = 0.2,
          verbose: bool = True, tag: str | None = None, *,
          init: str | None = None,
          lr: float = 1e-3, wd: float = 1e-4, epochs: int = 64, batch: int = 256,
          z_spec: str = DEFAULT_SPEC, soft_target: bool = False, soft_temp: float = 1.0,
          pi_weight: str = "none",
          pi_objective: str = "off",
          loss_chunk: int = 65536, threads: int = 1, device: str = "cpu",
          record: bool = True,
          record_dir: str | None = None) -> tuple[AZNet, dict[str, float]]:
    """训练一个网络，返回 `(net, metrics)`（`train_ce`/`val_ce`/`train_r2`/`val_r2`）。

    参数：
      `data`     `collect.CollectData`（采集产物 = 分片**目录**）或 `collect.CollectMix`
                 （多份**加权混训** = 回放；两者同一套出口，行序走 `data.epoch_rows`）
      `seed`     决定留出集划分与初始化；同数据同种子必然得到同一条轨迹
      `val_frac` 留出集比例（按场划分，同一场不会跨两侧）
      `verbose`  只控制台读数；进度条与 TensorBoard 打点不受它影响
      `tag`      TensorBoard 运行名；空 = `adhoc`

    `init` 是**续训起点**（`ckpt.save` 的存档路径）：只加载权重，**不继承优化器状态**
    （每轮重新起 Adam）；不设 = 随机初始化。建网配置必须与本次一致，不一致在
    `run_train` 读缓存之前就被拦下。自迭代里「网络一直续训、门控只决定 best」靠它实现。

    建网超参固定在 `aznet` 的模块常量里（不在签名里 ⇒ 不能用 flag 改、也不进权重名）。

    配方（键名与默认值都在 `train()` 的签名里）：
      `lr/wd/batch/epochs`  优化参数
      `z_spec`  值口径（`zspec`）：`hp`（默认）/ `hp_turn` / `enginenorm`。
                    ⚠️ `enginenorm` 要 `t_escore` 列，而 v3 落盘已删该列（`z_of` 会响亮拒绝）
                    ⇒ 现在只有采集/搜索侧能用它。
      `soft_target`  策略目标用软分布还是硬 argmax；默认硬 argmax（软目标更差的证据见
                     `.agent/notes.md`）
      `soft_temp`  软目标的温度：先 `π**(1/T)` 再归一化，<1 更尖、>1 更平；只在
                    `soft_target=True` 时生效（T=1 就是原来的软目标）
      `pi_weight`   **π 损失**（策略交叉熵）的逐行权重口径：`none` / `advantage`。
                    `none` 每行等权（历史行为，逐位不变）；`advantage` 用
                    `w = max(0, z − V̂.detach())` 加权、再除以权重均值
                    ⇒ 只放大「结局比网络当前预期更好」的决策，**尺度与等权同量级**
                    （不改等效学习率）。`z` = 值目标，`V̂` = 网络自己的值输出。
                    ⚠️ 只作用于 **π 损失**；记录里的 `train_ce`/`val_ce` 仍是**等权**口径
                    ⇒ 跨口径可比。
      `pi_objective`  **策略损失口径**：`off`（默认）/ `pg`。
                    `off` = 模仿：把 `π_θ` 往教师的 `π` 上拉（CE），上界 = 教师。
                    `pg` = 策略梯度：只回传**实际执行**的那个动作，权重 = 优势 `z − V̂`（可正可负）。
                    本质差别：优势为负时会**推离**该动作，而模仿只会照拄教师 argmax、
                    无论那条线最后赢没赢 ⇒ 上界不再被教师钉死。
                    `pg` **忽略** `pi_weight`；值头损失不变（它就是 baseline）。
                    需要缓存列 `a_act`/`a_sel`（实际动作下标）⇒ 旧缓存要重采。
      `loss_chunk`  算指标时的分块行数（防显存爆，CPU/GPU 共用）
      `threads`  torch 线程数，属于配方：换了轨迹就变，A/B 两个方案必须同值
      `device`   `cpu` / `cuda[:k]`；数据集留在 host，每个 batch 再上卡
      `record/record_dir`  读数落盘（`metrics.py`，纯 JSONL）

    返回的 `net` 已经 `.eval()` 且在 CPU 上（`save_ckpt`/`check_determinism` 都按 CPU 跑）。
    """
    if pi_weight not in PI_WEIGHT_MODES:
        raise ValueError(f"未知 --pi-weight {pi_weight!r}（可选：{' / '.join(PI_WEIGHT_MODES)}）")
    if pi_objective not in PI_OBJECTIVE_MODES:
        raise ValueError(f"未知 --pi-objective {pi_objective!r}（可选：{' / '.join(PI_OBJECTIVE_MODES)}）")
    if pi_objective == "pg" and "a_act" not in data.cols:
        raise ValueError(
            f"`--pi-objective pg` 要 `a_act` 列（实际执行的动作），这份数据里没有（{data.path}）。"
            f"自对弈在 `step < temp_moves` 时按 π **采样**，只知道分布 ⇒ 用新版采集重采。")
    torch.set_num_threads(threads)   # 必须固定，否则 GEMM 的归约顺序会变
    torch.manual_seed(seed)
    np.random.seed(seed)

    cfg = net_cfg()          # 建网配置（值固定在 `aznet.py`）：下面打印与建网都用它
    if verbose:
        print(f"  策略目标 = {'软分布 π**(1/%g)' % soft_temp if soft_target else '硬 argmax one-hot'}"
              f"  (--soft-target={int(soft_target)}, --soft-temp={soft_temp:g})", flush=True)
        # 会影响轨迹的设置都要打印（值口径、语义开关、设备）；不打印的话两份日志对不上。
        print(f"  值目标口径 = {z_spec}  (--z-spec)", flush=True)
        print(f"  π 损失权重 = {pi_weight}  (--pi-weight)", flush=True)
        print(f"  策略损失口径 = {pi_objective}  (--pi-objective)", flush=True)
        print("  选牌区门控 = **无条件开启**（非 select 帧置 0）", flush=True)
        print("  整局资源 = **恒可见**（遗物 181 + 药水 count/capacity + 药水槽；"
              "`use_resources` 已删）", flush=True)
        print(f"  设备 = {device}  batch = {batch}  lr = {lr:g}  (--device/--batch/--lr)",
              flush=True)
        print(f"  建网 = {cfg}", flush=True)
    tr, va = data.split(val_frac, seed)
    # 退化划分会让训练**静默为空**（0 步）：`split` 按段分组、每段整体归一侧，而段只有
    # （或几）个时必有一侧为空 —— loss 全 0、权重不动，账面看起来像"训了但没退步"。宁可当场报错。
    if not tr.any() or not va.any():
        raise RuntimeError(
            f"留出集划分退化：train {int(tr.sum())} 行 / val {int(va.sum())} 行，"
            f"段只有 {data.n_games} 个"
            "—— 每段整体归一侧，必有一侧为空。自对弈数据请让每项采多局（N_PER ≥ 2）。")
    tr_t, va_t = torch.as_tensor(tr), torch.as_tensor(va)

    # 数据集留在 host、每个 batch 再上卡：比整批上卡快，显存也只占当前 batch。
    dev = device
    if dev != "cpu":
        free = torch.cuda.mem_get_info(dev)[0] / 2**30
        print(f"  设备 {dev}（数据集留 host、逐 batch 上卡）· 卡上可用 {free:.1f}GB",
              flush=True)

    net = AZNet(**cfg).to(dev)
    if init:
        net.load_state_dict(ckpt.weights_of(init))
        if verbose:
            print(f"  续训起点 {init}", flush=True)
    opt = torch.optim.Adam(net.parameters(), lr=lr, weight_decay=wd)

    # 漂移基准 = 本次训练从哪儿出发（`--init` 加载之后）。逐 epoch 记 `train/dw`。
    # 用途：门控差为零时，区分「训了但没退化」与「步长太小、根本没动」。
    # 定义与跨轮一致性：`Σ|w − w0| / Σnumel`（换定义就不可跨轮比较）。
    w_ref = {k: v.detach().clone() for k, v in net.state_dict().items()}
    w_ref_n = sum(v.numel() for v in w_ref.values())

    def drift() -> float:
        """与训练起点的平均绝对差（跨轮、跨配置可比）。"""
        with torch.no_grad():
            s = sum(float((v.detach() - w_ref[k]).abs().sum())
                    for k, v in net.state_dict().items())
        return s / max(w_ref_n, 1)

    # 逐层读数：全局范数 / 全局漂移会把「第 0 层不动、头猛动」平均掉。名字取自 `named_modules()`
    # （与 state_dict 键同源）⇒ 换 trunk 结构这里不用改。
    trunk_lins = [(n_.removeprefix("trunk."), m) for n_, m in net.named_modules()
                  if n_.startswith("trunk.") and isinstance(m, torch.nn.Linear)]

    def trunk_readings() -> dict[str, float]:
        """trunk 每层的梯度 **RMS** 与权重漂移均值。每 epoch 一次（每 batch 记会灌满 JSONL）。

        用逐元素 RMS 而不是范数：范数随该层参数个数放大（第 0 层 fan_in 大几倍）⇒ 跨层不可比。
        """
        out: dict[str, float] = {}
        for i, m in trunk_lins:
            g = torch.cat([p.grad.ravel() for p in m.parameters() if p.grad is not None])
            out[f"trunk{i}/grad"] = float(torch.linalg.vector_norm(g) / g.numel() ** 0.5)
            out[f"trunk{i}/dw"] = float(
                (m.weight.detach() - w_ref[f"trunk.{i}.weight"]).abs().mean())
        return out

    # 读数旁路：关掉时 `w=None`，下面 `metrics.*` 全是空操作。配方与曲线同目录，便于跨运行对比。
    w = metrics.writer(tag or "adhoc", sub=f"seed{seed}", on=record, out_dir=record_dir)
    metrics.recipe(w, dict(cfg, lr=lr, wd=wd, batch=batch, epochs=epochs, val_frac=val_frac,
                       z_spec=z_spec, soft_target=soft_target, soft_temp=soft_temp,
                       pi_weight=pi_weight,
                       pi_objective=pi_objective,
                       device=dev, seed=seed,
                       threads=threads, rows=data.n_rows))

    def losses(mask: torch.Tensor) -> tuple[float, float, float]:
        """算 CE / R²。先切 eval 再算、算完切回 train，否则 dropout 会让指标抖动。

        按 `loss_chunk` 行分块：每块**按需**取行（`data.batch`）⇒ 整个 split 不必常驻内存，
        显存也只占一块；`dev="cpu"` 时 `.to(dev)` 不做任何事，CPU/GPU 共用这一段。
        分块的 GEMM 归约顺序与整批不同，末几位不能逐位比较。
        """
        was_training = net.training
        net.eval()
        with torch.no_grad():
            ii = torch.nonzero(mask).squeeze(1)
            tot_ce = tot_se = 0.0
            for s0 in range(0, len(ii), loss_chunk):
                b = data.batch(ii[s0:s0 + loss_chunk], soft_target, soft_temp)
                oj, paj, maj = b["obs"].to(dev), b["pi_act"].to(dev), b["m_act"].to(dev)
                pselj, mselj, vj = b["pi_sel"].to(dev), b["m_sel"].to(dev), b["z"].to(dev)
                o_act, o_sel, v = net(oj)
                ce = (-(paj * torch.log_softmax(o_act.masked_fill(~maj, -1e9), -1)).sum(-1)
                      - (pselj * torch.log_softmax(
                          o_sel.masked_fill(~mselj, -1e9), -1)).sum(-1))
                tot_ce += float(ce.sum())
                tot_se += float((v - vj).pow(2).sum())
            n_row = max(len(ii), 1)
            ce, vm = tot_ce / n_row, tot_se / n_row
            vv = float(np.var(np.asarray(data.z(z_spec))[mask.numpy()]))
        if was_training:
            net.train()
        return ce, vm, 1.0 - vm / max(vv, 1e-12)

    idx_tr = torch.nonzero(tr_t).squeeze(1)
    # 进度条始终开着（作业跑在 `script` 的 pty 里）；`verbose` 只控制台读数。
    last_loss = None
    gstep = 0            # 全局 batch 序号，即 `points.jsonl` 里 `train/loss` 的横轴
    pg = tqdm(total=epochs, desc=f"训练 seed={seed}", unit="ep",
              dynamic_ncols=True, mininterval=0.4)
    for ep in range(epochs):
        # 行序由数据集给（单份 = `idx_tr` 的一遍乱序，逐位同旧行为；混训 = 按权重分位，
        # 且**长度 = 本轮的训练行数** ⇒ 挂回放不改优化步数）。
        order = data.epoch_rows(idx_tr, ep, seed)
        for s in range(0, len(order), batch):
            idx = order[s:s + batch]
            # 逐批取行 + 逐批转换，再整批上卡；`dev="cpu"` 时 `.to(dev)` 不做任何事。
            b = data.batch(idx, soft_target, soft_temp)
            ob, pa, ma = b["obs"].to(dev), b["pi_act"].to(dev), b["m_act"].to(dev)
            psel, msel, vv = b["pi_sel"].to(dev), b["m_sel"].to(dev), b["z"].to(dev)
            o_act, o_sel, v_hat = net(ob)
            ce_row = (-(pa * torch.log_softmax(o_act.masked_fill(~ma, -1e9), -1)).sum(-1)
                      - (psel * torch.log_softmax(o_sel.masked_fill(~msel, -1e9), -1)).sum(-1))
            if pi_objective == "pg":
                # 策略梯度：只回传**实际执行**的那个动作，逐行权重 = 优势 `z − V̂`（可正可负）。
                # 与模仿的本质差别：优势为负时它**推离**该动作；模仿只会照拄教师 argmax。
                # ⚠️ 每行用**它自己空间**的头与掩码：选牌帧的 `m_act` 全 0，拿 act 头算会得到垃圾。
                is_sel_row = ~ma.any(-1)
                lp_a = torch.log_softmax(o_act.masked_fill(~ma, -1e9), -1)
                lp_s = torch.log_softmax(o_sel.masked_fill(~msel, -1e9), -1)
                # 索引按**输出张量的 device**上卡（不靠外部 `dev`）：网在 GPU 而缓存留 host 时，
                # 靠外部变量很容易漏一次 `.to()`，而报错直到 `gather` 才出现（且只在 cuda 上）。
                aa = b["a_act"].to(lp_a.device).clamp_min(0)
                as_ = b["a_sel"].to(lp_s.device).clamp_min(0)
                logp = torch.where(is_sel_row,
                                   lp_s.gather(1, as_[:, None]).squeeze(1),
                                   lp_a.gather(1, aa[:, None]).squeeze(1))
                adv = vv - v_hat.detach()
                # 标准化只改尺度、不改批内序；`hp` 口径下 adv 量级随数据漂，标准化后不挑 lr。
                # ⚠️ 代价：批内均值被减掉后，**整批同一个 z** 时信号退化成 `-(V̂ - mean V̂)`
                #    （只剩“比批内平均估值低的状态就推”）—— 同一场的行 z 相同，
                #    批里若只装了一两场就会这样。真跑时看批内 z 的方差来发现。
                adv = (adv - adv.mean()) / adv.std().clamp_min(1e-6)
                ce = -(adv * logp).mean()
            elif pi_weight == "advantage":
                # 优势加权：`vv` 是值目标、`v_hat` 是网络自己的估值 ⇒ 只奖「比当前预期更好」的决策。
                # 除以权重均值 ⇒ CE 的尺度与等权时同量级（不改等效学习率）。
                w_row = (vv - v_hat.detach()).clamp_min(0.0)
                sw = float(w_row.sum())
                # 权重全零时退回等权——否则策略梯度会**静默**归零（loss 看着正常，参数不动）。
                ce = (w_row * ce_row).sum() / sw if sw > 0.0 else ce_row.mean()
            else:
                ce = ce_row.mean()
            loss = ce + (v_hat - vv).pow(2).mean()
            # 发散必须当场报错：NaN/Inf 会一路带到存盘，而 `strict=True` 加载并不报错。
            lv = float(loss.detach())
            if not math.isfinite(lv):
                msg = (f"训练发散：loss={lv} 非有限（epoch {ep + 1}/{epochs}，第 "
                       f"{s // batch + 1}/{-(-len(order) // batch)} 个 batch，"
                       f"上一个有限 loss={last_loss}）；"
                       f"先调小 --lr（当前 {lr:g}）并检查数据。batch={batch} device={dev}")
                metrics.fault("loss_nonfinite", msg, step=gstep)
                raise RuntimeError(msg)
            last_loss = lv
            opt.zero_grad()
            loss.backward()
            # 只看不动：只算范数，不做裁剪（裁剪等于改配方）。梯度先爆，NaN 后到。
            gn = float(torch.linalg.vector_norm(torch.cat(
                [p.grad.ravel() for p in net.parameters() if p.grad is not None])))
            opt.step()
            metrics.scalars(w, gstep, **{"train/loss": lv, "train/grad_norm": gn})
            gstep += 1
        # loss 有限但参数非有限：说明是优化器更新那一步炸的，和上一条不是同一个原因。
        pmax = max(float(p.detach().abs().max()) for p in net.parameters())
        bad = _first_nonfinite(net)
        if bad is not None:
            msg = (f"训练发散：epoch {ep + 1}/{epochs} 末参数 `{bad}` 含 NaN/Inf（loss 仍有限）；"
                   f"先调小 --lr（当前 {lr:g}）")
            metrics.fault("param_nonfinite", msg, step=ep + 1)
            raise RuntimeError(msg)
        metrics.scalars(w, ep + 1, **{"train/param_absmax": pmax, "train/dw": drift()},
                        **trunk_readings())
        # 留出集指标的打点频率：每 run 至少 8 个点（否则短训练只能得到一个点、画不出趋势）。
        if (ep + 1) % max(1, epochs // 8) == 0 or ep + 1 == epochs:
            # `losses()` 的第二个返回值是 value 的 MSE（只在算 R² 时用得到），这里丢弃。
            c_tr, _, r_tr = losses(tr_t)
            c_va, _, r_va = losses(va_t)
            pg.set_postfix_str(f"train ce={c_tr:.3f} R2={r_tr:+.3f} | "
                               f"val ce={c_va:.3f} R2={r_va:+.3f}", refresh=False)
            metrics.scalars(w, ep + 1, **{"train/ce": c_tr, "train/r2": r_tr,
                                      "val/ce": c_va, "val/r2": r_va})
        pg.update()
    pg.close()

    c_tr, _, r_tr = losses(tr_t)
    c_va, _, r_va = losses(va_t)
    print(f"  最终 train ce={c_tr:.3f} R2={r_tr:+.3f} | val ce={c_va:.3f} R2={r_va:+.3f}",
          flush=True)
    print(f"  权重漂移 {drift():.6g}（mean|dw|，相对续训起点）", flush=True)
    metrics.scalars(w, epochs, **{"train/ce": c_tr, "train/r2": r_tr,
                              "val/ce": c_va, "val/r2": r_va})
    metrics.close(w)       # 收尾 flush（落盘关掉时 `w=None`，这里是 no-op）
    net.eval()
    # 收尾搬回 CPU：`check_determinism` / `save_ckpt` 都按 CPU 口径跑。
    net.to("cpu")
    return net, dict(train_ce=c_tr, val_ce=c_va, train_r2=r_tr, val_r2=r_va)


# ---------------------------------------------------------------- 评估
def check_determinism(net: AZNet, data, n: int = 32) -> bool:
    """同一批输入连算两次必须完全一致，用来挡「返回的网络还停在 train 模式」（dropout 开着）。

    先查非有限值再比一致性：`NaN != NaN`，发散时也会返回 False，会被误报成「没切 eval」。
    """
    net.eval()
    x = data.batch(np.arange(min(n, data.n_rows)))["obs"]
    with torch.no_grad():
        a = net(x)
        b = net(x)
    bad = [nm for nm, t in zip(("策略", "选牌", "价值"), a)
           if not bool(torch.isfinite(t).all())]
    if bad:
        msg = f"输出里有非有限值（{'/'.join(bad)}）—— 不是 eval 没切，是权重或输入已经坏了"
        metrics.fault("determinism_nonfinite", f"确定性自检: {msg}")
        print(f"  确定性自检: ❌ {msg}", flush=True)
        return False
    same = all(torch.equal(x_, y_) for x_, y_ in zip(a, b))
    if not same:
        metrics.fault("determinism", "同一批输入两次前向结果不同（是不是没切 eval？）")
    print(f"  确定性自检: {'通过（两次前向完全一致）' if same else '❌ 不一致 —— 是不是没切 eval？'}")
    return same


# ---------------------------------------------------------------- 看门狗
# 判断卡死用日志文件的 mtime（`script -f` 是逐写落盘）：真卡死时日志不再更新，而
# 「进度条不动」分不出「块缓冲」和「真死了」。只在有 `STS_BGLOG` 时生效（作业环境变量，
# 不属于配方）；`0` 表示关闭。
_BGLOG = os.environ.get("STS_BGLOG", "")


def _start_watchdog(timeout: float, bglog: str | None = None) -> None:
    """看门狗：`bglog` 超过 `timeout` 秒没有更新就 `os._exit(3)`；`timeout<=0` 表示关闭。

    `timeout` 来自 CLI 的 `--job-timeout`（秒）；`bglog` 默认取 `STS_BGLOG`。
    """
    log = _BGLOG if bglog is None else bglog
    if timeout <= 0 or not log:
        return None

    def loop():
        # 判据是累计静默时间（当前时间减 mtime）；改成「两次采样相等」会把阈值压成约 60 秒。
        while True:
            time.sleep(60)
            try:
                mt = os.path.getmtime(log)
            except OSError:
                continue
            idle = time.time() - mt
            if idle > timeout:
                print(f"\n❌ 看门狗：{log} 已 {idle:.0f}s 没有写入（阈值 {timeout:.0f}s）"
                      f"⇒ 判定卡死，主动退出。（取证三件：日志 mtime / 完成标记出现次数 / "
                      f"**父进程 CPU%** —— 单看进度条区分不了'块缓冲'与'真死了'；"
                      f"--job-timeout 0 可关）",
                      flush=True)
                os._exit(3)

    threading.Thread(target=loop, daemon=True).start()
    return None


# ---------------------------------------------------------------- CLI
# 位置参数是沿用下来的接口（`chain.sh` 与文档都在用），保持不动。


class _Help(argparse.ArgumentDefaultsHelpFormatter, argparse.RawDescriptionHelpFormatter):
    """`--help` 既显示每个 flag 的默认值，又保留 docstring 里的换行。"""


def build_parser() -> argparse.ArgumentParser:
    """构造 `train`（训练一个网络并存盘）与 `seeds`（多 seed 并行）两个子命令；`epilog` 用模块 docstring。"""
    p = argparse.ArgumentParser(description="train", epilog=__doc__, formatter_class=_Help,
                                allow_abbrev=False)   # 拼错必报，不当唯一前缀缩写吃
    sub = p.add_subparsers(dest="mode", required=True)

    def add(name, help_text, *pos, with_epochs=True):
        """构造子命令：先加位置参数，再按签名加配方 flag，最后 `--epochs`。"""
        sp = sub.add_parser(name, help=help_text, description=help_text,
                            formatter_class=_Help, allow_abbrev=False)
        for nm, kw in pos:
            sp.add_argument(nm, **kw)
        recipe_flags(sp)
        if with_epochs:
            sp.add_argument("--epochs", type=int, help=HELP["epochs"],
                            default=inspect.signature(train).parameters["epochs"].default)
        # 作业环境（不是配方）：看门狗阈值。
        sp.add_argument("--job-timeout", type=float, default=1800.0,
                        help="看门狗阈值（秒）；0 = 关")
        # 回放（加权混训）：不属于 `train()` 的配方（它是**数据集**的形状，不是优化参数）⇒
        # 在这里注册、由 `load_data()` 拼数据集；权重进存档的 `recipe`（名字里不带）。
        sp.add_argument("--mix", action="append", default=None, metavar="路径=权重",
                        help="回放数据：可重复；权重是相对本轮的比（本轮 / 位置参数恒 1.0）。"
                             "例：--mix data/r4=0.6 --mix data/r3=0.36 ⇒ 50.9/30.5/18.5%%"
                             "（⚠️ `%%` 是为了转义：argparse 会把 help 里的 `%%` 当格式符）")
        return sp

    # 未知子命令和未知 flag 都会在这里报错（退出码 2），不会悄悄当成别的任务跑。
    add("train", "训练一个网络并存盘",
        ("cache", dict()), ("pos_epochs", dict(type=int, nargs="?", default=None)),
        ("out", dict(nargs="?", default=None,
                     help="权重路径（不给 = `<数据目录名>.pt`，落在 `data/`）")),
        with_epochs=False)
    add("seeds", "多种子训练（并行）",
        ("cache", dict()), ("n_seeds", dict(type=int, nargs="?", default=3)),
        ("--seed0", dict(type=int, default=0, help="种子起点（续跑用）")),
        ("--save", dict(default=None, help="路径模板，用 {seed} 占位；不设则不存盘")),
        ("--mp-start", dict(default=None, help="multiprocessing 启动方式（fork/spawn）")))
    return p


def check_init(path: str, kw: dict[str, Any]) -> None:
    """校验续训起点：文件存在、建网配置与本次一致。

    放在读缓存之前：路径写错或网不合，不该先花几分钟读 GB 级缓存，也不该等
    `load_state_dict` 抛出难读的形状错。
    """
    if not os.path.exists(path):
        raise SystemExit(f"❌ --init 不存在：{path}")
    icfg = ckpt.config_of(path)
    if icfg is None:
        raise SystemExit(f"❌ --init 读不出建网配置（旧格式存档？）：{path}")
    cfg = net_cfg()
    bad = {k: (icfg.get(k), cfg[k]) for k in NET_KEYS if icfg.get(k) != cfg[k]}
    if bad:
        raise SystemExit(f"❌ --init 的建网配置与本次不一致：{bad}")


def run_train(a: argparse.Namespace) -> None:
    """`train` 子命令：训练一个网络并存盘。"""
    kw = train_kwargs(a)
    if a.pos_epochs is not None:
        kw["epochs"] = a.pos_epochs          # 位置参数优先于签名里的默认值
    if kw.get("init"):
        check_init(kw["init"], kw)
    data, parts = load_data(a.cache, a.mix)
    if len(parts) > 1:                       # 回放：把**各份与占比**打进日志（可复现的台账）
        print(f"  {data.summary()}", flush=True)
        for p, w in parts[1:]:
            print(f"    + {p}（权重 {w:g}）", flush=True)
    # 权重路径：默认 = `<数据目录名>.pt`（落在 `data/`）——**名字不带配方**（见 `runtime` 顶部）。
    # 配方（数据 + 混训 + 全部训练超参）写进**存档内部**的 `recipe` ⇒ 不看名字也复得出来。
    out = a.out or cache_path(f"{_stem(a.cache)}.pt")
    recipe = {"data": parts, "epochs": kw["epochs"], **kw}
    net, m = train(data, tag=_stem(out), **kw)
    check_determinism(net, data)
    save_ckpt(net, out, cfg=net_cfg(),
              z_spec=kw["z_spec"], recipe=recipe, **m)
    print(f"已保存 {out}  threads={kw['threads']}  "
          f"device={kw['device']}  batch={kw['batch']}（配方在存档的 recipe 字段里）", flush=True)


def run_seeds(a: argparse.Namespace) -> None:
    """`seeds` 子命令：多 seed 并行训练；只有给了 `--save` 才按种子存盘。"""
    kw = train_kwargs(a)
    data, parts = load_data(a.cache, a.mix)
    # TensorBoard 运行名 = 数据目录名（+ 混训份数，免得两种混法挤进同一个运行目录）。
    tag = _stem(a.cache) + (f"+mix{len(parts) - 1}" if len(parts) > 1 else "")
    n_seeds, s0 = a.n_seeds, a.seed0 or 0
    save_tpl = a.save or ""
    # 这两项检查都放在读缓存之前：参数不对就不必先花几分钟读 GB 级的缓存。
    if n_seeds > 1 and mp_context(a.mp_start).get_start_method() != "fork":
        print(f"❌ `seeds` 的并行训练需要 fork（当前 {mp_context(a.mp_start).get_start_method()!r}）"
              "—— 数据靠 fork 后的模块全局 `_PAR_DATA` 交给子进程，spawn 的子进程拿不到。"
              "请用 `--mp-start fork`（POSIX）或把 n_seeds 设为 1。", flush=True)
        sys.exit(2)
    if save_tpl and "{seed}" not in save_tpl and n_seeds > 1:
        print(f"❌ --save={save_tpl!r} 没有 {{seed}} 占位符，但 n_seeds={n_seeds} —— "
              f"会把 {n_seeds} 个网络互相覆盖。请写成 '..._s{{seed}}.pt'。", flush=True)
        sys.exit(2)

    print(f"样本 {data.n_rows}  段 {data.n_games}  "
          f"threads={kw['threads']}  device={kw['device']}  "
          f"batch={kw['batch']}  lr={kw['lr']:g}  wd={kw['wd']:g}  "
          f"epochs={kw['epochs']}  "
          f"目标={'软分布' if kw['soft_target'] else '硬argmax'}  z口径={kw['z_spec']}  "
          f"π损失权重={kw['pi_weight']}  π口径={kw['pi_objective']}")
    print(f"  建网 {net_cfg()}（固定值，见 `aznet.py`）", flush=True)
    seeds_ = list(range(s0, s0 + n_seeds))
    if s0:
        print(f"种子起点 --seed0 {s0} → 跑 seed {s0}..{seeds_[-1]}", flush=True)

    if n_seeds > 1:                     # 用 fork 把数据交给子进程（`_PAR_DATA` 就是为它存在的）
        print(f"训练阶段：{n_seeds} 个进程并行（各 {kw['threads']} 线程，"
              f"最多 {min(n_seeds, PAR_TRAIN)} 个）", flush=True)
        _PAR_DATA["data"] = data
        with mp_context(a.mp_start).Pool(min(n_seeds, PAR_TRAIN)) as pool:
            trained = pool.map(_train_seed, [(s, tag, kw) for s in seeds_])
        trained.sort(key=lambda x: x[0])
    else:                               # 单个种子：spawn 下进程池不可用，只能串行跑
        trained = []
        for s in seeds_:
            net_s, m_s = train(data, seed=s, verbose=False, tag=tag, **kw)
            trained.append((s, {k: v.cpu() for k, v in net_s.state_dict().items()}, m_s))

    cfg_net = net_cfg()
    for s, sd, m in trained:
        net = AZNet(**cfg_net)
        net.load_state_dict(sd)
        net.eval()
        if save_tpl:
            out = save_tpl.format(seed=s)
            d = os.path.dirname(out)
            if d:
                os.makedirs(d, exist_ok=True)
            save_ckpt(net, out, cfg=cfg_net, z_spec=kw["z_spec"], seed=s,
                      recipe={"data": parts, "epochs": kw["epochs"], **kw}, **m)
            print(f"  已保存 {out}  (seed={s})", flush=True)
        print(f"  seed={s}  val_ce={m['val_ce']:.3f} "
              f"val_R2={m['val_r2']:+.3f}", flush=True)


def main() -> None:
    a = build_parser().parse_args()
    _start_watchdog(a.job_timeout)
    if a.mode == "train":
        run_train(a)
    elif a.mode == "seeds":
        run_seeds(a)


if __name__ == "__main__":
    main()
