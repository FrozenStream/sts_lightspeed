"""训练 AZNet。本文件只做训练：采集在 `collect_encs.py` / `collect_selfplay.py`，
评估在 `eval_walk.py`（离线探针 `probe_imit.py` / `probe_capacity.py`）。

用法：
  python train.py train <缓存路径> [epochs] [权重输出] [--lr 1e-3 …]
  python train.py seeds <缓存路径> [种子数] [--seed0 3 --save 'data/n1_s{seed}.pt']
（配方 flag 清单见 `python train.py train --help`）

契约：
  · 策略目标用硬 argmax（`--soft-target` 只为复现旧结果保留；理由与数字见 `.agent/notes.md`）。
  · `seeds` 默认不存盘，给了 `--save` 才按种子保存。
  · **建网超参不是配方**：固定在 `aznet.py` 的模块常量里 ⇒ 无 CLI flag、不进权重名（换值 = 换网）；
    存档仍记 `config`，`check_init()` 拿它挡旧架构的权重。
  · 配方（优化、值口径、线程、设备、打点）只来自 `train()` 的形参，不读环境变量：
    flag 由 `recipe_flags()` 从签名派生（连默认值一起），参数由 `train_kwargs()` 原样
    转交 `train()`，名字对不上就当场 `TypeError`。
  · 线程数属于配方：换了线程数轨迹就变，A/B 两个方案必须同值。
  · 种子纪律：训练段、采集段、自对弈段都不得与评估段（从 `collect.EVAL_SEED0` = 1e8 起）重叠。
  · 数据格式 = `collect.assemble` 写出的 npz 键集；自对弈数据同格式，可以直接混训。
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
from collect import check_action_space, check_cache_obs
from runtime import ckpt_name, mp_context, short_name
from zspec import DEFAULT_SPEC, z_of

# ---------------------------------------------------------------- 配方
# 配方 flag 直接从 `train()` 的签名派生：名字、类型、默认值全读签名，不另抄一张表。
# `HELP` 只是 `--help` 的说明文字，写漏顶多少一句提示，不影响取值。
HELP: dict[str, str] = {
    "lr": "学习率",
    "wd": "权重衰减",
    "batch": "batch 大小",
    "epochs": "训练轮数",
    "z_spec": "值口径：enginenorm / hp / hp_turn",
    "soft_target": "策略目标用软分布（默认硬 argmax）",
    "soft_temp": "软目标的温度：π**(1/T)，<1 更尖、>1 更平（仅 --soft-target 下生效）",
    "loss_chunk": "算指标的分块行数",
    "threads": "torch 线程数（属于配方，会改轨迹）",
    "device": "cpu / cuda[:k]",
    "record": "读数落盘开关（0/1）",
    "record_dir": "读数根目录（默认 data/runs）",
    "init": "续训起点权重（`ckpt.save` 的存档）；不设 = 随机初始化",
}
# 子命令里不属于配方的名字：这些不进 `train()`。新增一个 flag 而忘了登记（或者名字拼错），
# 都会在 `train(**kw)` 那一步当场 TypeError —— 就是要它炸。
JOB_KEYS = frozenset({"mode", "cache", "pos_epochs", "out", "name", "job_timeout",
                      "n_seeds", "seed0", "save", "mp_start"})


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
    """取**显示用短名**当 TensorBoard 运行名（完整名带全部超参，做目录名太长）。"""
    return short_name(path or "")
    return os.path.splitext(os.path.basename(path or ""))[0]


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
              **metrics: float) -> str:
    """把权重和建网配置写进同一个文件（`ckpt.save`），返回存档路径。

    `net` 必须在 CPU 上；`cfg` 是 `net_cfg()` 的结果；`**metrics` 是训练指标，写进存档备查。
    含 NaN/Inf 的权重会拒绝存盘并抛 `RuntimeError`：这种权重形状合法、能 `strict=True`
    加载，但评估恒为 0%，属于不报错的坏结果，必须在源头拦住。
    """
    bad = _first_nonfinite(net)
    if bad is not None:
        msg = f"拒绝存盘 {path}：参数 `{bad}` 含 NaN/Inf —— 训练已发散。"
        metrics.fault("save_nonfinite", msg)
        raise RuntimeError(msg)

    return ckpt.save(path, net.state_dict(), cfg,
                      space=POLICY_DIM, z_spec=z_spec,
                      metrics={k: float(v) for k, v in metrics.items()})


# ---------------------------------------------------------------- 训练
def battle_split(bid: np.ndarray, val_frac: float = 0.2, seed: int = 0,
                 group: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """按场划分留出集：同一场的所有状态共享一个 z，按样本切会把答案泄露到训练侧。

    `group` 是行的分组键，同组必须分到同一侧。应当传 `data["seed"]`：`bid` 只是批次内下标，
    教师数据和自对弈数据里同一个 seed 的 `bid` 相同，按 `bid` 分会把同一个开局的两种轨迹
    分到两侧（同样泄露）。只有旧缓存没这个键时才退回用 `bid`。
    """
    g = bid if group is None else group
    keys = np.unique(g)
    rng = np.random.default_rng(seed)
    rng.shuffle(keys)
    n_val = max(1, int(round(len(keys) * val_frac)))
    val_set = set(keys[:n_val].tolist())
    return (np.array([k not in val_set for k in g]),
            np.array([k in val_set for k in g]))


def hard_target(p: np.ndarray) -> np.ndarray:
    """访问分布转成硬 argmax 的 one-hot；全零的行保持全零。"""
    out = np.zeros_like(p)
    rows = p.sum(-1) > 0
    out[rows, p[rows].argmax(-1)] = 1.0
    return out


def soft_target(p: np.ndarray, temp: float = 1.0) -> np.ndarray:
    """访问分布转成软目标（逐行归一化）；全零的行保持全零。

    比 `hard_target` 多保留 MCTS 对所有合法动作的相对排序。全零行（`pi_act` 的选牌行、
    `pi_sel` 的非选牌行）在交叉熵里贡献 0。

    `temp` 是温度：先算 `π**(1/temp)` 再归一化 —— <1 更尖（靠近硬目标）、>1 更平。
    `temp=1` 就是原来的软目标（逐位不变）。
    """
    out = np.array(p, dtype=np.float32, copy=True)
    if temp != 1.0:
        np.power(out, 1.0 / temp, out=out)
    s = out.sum(-1, keepdims=True)
    nz = s[..., 0] > 0
    out[nz] /= s[nz]
    return out


def targets(p: np.ndarray, soft: bool, temp: float = 1.0) -> np.ndarray:
    """按软/硬口径生成策略目标（`temp` 只在软目标下生效）。"""
    return soft_target(p, temp) if soft else hard_target(p)


def z_values(data: dict[str, np.ndarray], z_spec: str) -> np.ndarray:
    """按 `z_spec` 从终局列算值目标（`zspec.z_of`）"""
    return z_of(data, z_spec).astype(np.float32)


def train(data: dict[str, np.ndarray], seed: int = 0, val_frac: float = 0.2,
          verbose: bool = True, tag: str | None = None, *,
          init: str | None = None,
          lr: float = 1e-3, wd: float = 1e-4, epochs: int = 64, batch: int = 256,
          z_spec: str = DEFAULT_SPEC, soft_target: bool = False, soft_temp: float = 1.0,
          loss_chunk: int = 65536, threads: int = 1, device: str = "cpu",
          record: bool = True,
          record_dir: str | None = None) -> tuple[AZNet, dict[str, float]]:
    """训练一个网络，返回 `(net, metrics)`（`train_ce`/`val_ce`/`train_r2`/`val_r2`）。

    参数：
      `data`     缓存 dict（`collect.assemble` 的键集）
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
      `z_spec`  值口径（`zspec`）：`enginenorm`（默认）/ `hp` / `hp_turn`
      `soft_target`  策略目标用软分布还是硬 argmax；默认硬 argmax（软目标更差的证据见
                     `.agent/notes.md`）
      `soft_temp`  软目标的温度：先 `π**(1/T)` 再归一化，<1 更尖、>1 更平；只在
                    `soft_target=True` 时生效（T=1 就是原来的软目标）
      `loss_chunk`  算指标时的分块行数（防显存爆，CPU/GPU 共用）
      `threads`  torch 线程数，属于配方：换了轨迹就变，A/B 两个方案必须同值
      `device`   `cpu` / `cuda[:k]`；数据集留在 host，每个 batch 再上卡
      `record/record_dir`  读数落盘（`metrics.py`，纯 JSONL）

    返回的 `net` 已经 `.eval()` 且在 CPU 上（`save_ckpt`/`check_determinism` 都按 CPU 跑）。
    """
    check_action_space(data, where="train()")
    torch.set_num_threads(threads)   # 必须固定，否则 GEMM 的归约顺序会变
    torch.manual_seed(seed)
    np.random.seed(seed)

    cfg = net_cfg()          # 建网配置（值固定在 `aznet.py`）：下面打印与建网都用它
    obs = torch.as_tensor(data["obs"])
    if verbose:
        print(f"  策略目标 = {'软分布 π**(1/%g)' % soft_temp if soft_target else '硬 argmax one-hot'}"
              f"  (--soft-target={int(soft_target)}, --soft-temp={soft_temp:g})", flush=True)
        # 会影响轨迹的设置都要打印（值口径、语义开关、设备）；不打印的话两份日志对不上。
        print(f"  值目标口径 = {z_spec}  (--z-spec)", flush=True)
        print("  选牌区门控 = **无条件开启**（非 select 帧置 0）", flush=True)
        print("  整局资源 = **恒可见**（遗物 181 + 药水 count/capacity + 药水槽；"
              "`use_resources` 已删）", flush=True)
        print(f"  设备 = {device}  batch = {batch}  lr = {lr:g}  (--device/--batch/--lr)",
              flush=True)
        print(f"  建网 = {cfg}", flush=True)
    P_act, P_sel = (torch.as_tensor(targets(data["pi_act"], soft_target, soft_temp)),
                    torch.as_tensor(targets(data["pi_sel"], soft_target, soft_temp)))
    M_act, M_sel = torch.as_tensor(data["m_act"]), torch.as_tensor(data["m_sel"])
    V = torch.as_tensor(z_values(data, z_spec))
    grp = data.get("seed")
    if grp is None:
        print("⚠️ 缓存没有 `seed` 键（旧格式）⇒ val 分割退回按 `bid`。"
              "单源缓存无妨；**混源（教师+自对弈）下这是泄漏** —— 同一 seed 的行会落到"
              "两侧。重跑 collect/selfplay 就会带上该键。", flush=True)
    tr, va = battle_split(data["bid"], val_frac, seed, group=grp)
    # 退化划分会让训练**静默为空**（0 步）：`battle_split` 按 `seed` 分组、每组整体归一侧，
    # 所以 `seed` 只有一个取值时（例如每项只采 1 局）训练集必空 —— loss 全 0、权重不动，
    # 账面看起来像"训了但没退步"。宁可当场报错。
    if not tr.any() or not va.any():
        g_all = grp if grp is not None else data["bid"]
        raise RuntimeError(
            f"留出集划分退化：train {int(tr.sum())} 行 / val {int(va.sum())} 行，"
            f"分组键 {'seed' if grp is not None else 'bid'} 只有 {np.unique(g_all).size} 个取值 "
            "—— 每个取值整体归一侧，必有一侧为空。自对弈缓存请让每项采多局（N_PER ≥ 2）。")
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

    # 读数旁路：关掉时 `w=None`，下面 `metrics.*` 全是空操作。配方与曲线同目录，便于跨运行对比。
    w = metrics.writer(tag or "adhoc", sub=f"seed{seed}", on=record, out_dir=record_dir)
    metrics.recipe(w, dict(cfg, lr=lr, wd=wd, batch=batch, epochs=epochs, val_frac=val_frac,
                       z_spec=z_spec, soft_target=soft_target, soft_temp=soft_temp,
                       device=dev, seed=seed,
                       threads=threads, rows=int(len(data["obs"]))))

    def losses(mask: torch.Tensor) -> tuple[float, float, float]:
        """算 CE / R²。先切 eval 再算、算完切回 train，否则 dropout 会让指标抖动。

        按 `loss_chunk` 行分块前向：整个 split 一次前向会爆显存；`dev="cpu"` 时 `.to(dev)`
        不做任何事，所以 CPU/GPU 共用这一段。分块的 GEMM 归约顺序与整批不同，末几位不能
        逐位比较。
        """
        was_training = net.training
        net.eval()
        with torch.no_grad():
            ii = torch.nonzero(mask).squeeze(1)
            tot_ce = tot_se = 0.0
            for s0 in range(0, len(ii), loss_chunk):
                j = ii[s0:s0 + loss_chunk]
                oj, paj, maj = obs[j].to(dev), P_act[j].to(dev), M_act[j].to(dev)
                pselj, mselj, vj = P_sel[j].to(dev), M_sel[j].to(dev), V[j].to(dev)
                o_act, o_sel, v = net(oj)
                ce = (-(paj * torch.log_softmax(o_act.masked_fill(~maj, -1e9), -1)).sum(-1)
                      - (pselj * torch.log_softmax(
                          o_sel.masked_fill(~mselj, -1e9), -1)).sum(-1))
                tot_ce += float(ce.sum())
                tot_se += float((v - vj).pow(2).sum())
            n_row = max(len(ii), 1)
            ce, vm = tot_ce / n_row, tot_se / n_row
            vv = float(V[mask].var())
        if was_training:
            net.train()
        return ce, vm, 1.0 - vm / max(vv, 1e-12)

    idx_tr = torch.nonzero(tr_t).squeeze(1)
    n = len(idx_tr)
    # 进度条始终开着（作业跑在 `script` 的 pty 里）；`verbose` 只控制台读数。
    last_loss = None
    gstep = 0            # 全局 batch 序号，即 `points.jsonl` 里 `train/loss` 的横轴
    pg = tqdm(total=epochs, desc=f"训练 seed={seed}", unit="ep",
              dynamic_ncols=True, mininterval=0.4)
    for ep in range(epochs):
        perm = idx_tr[torch.randperm(n, device=idx_tr.device)]
        for s in range(0, n, batch):
            idx = perm[s:s + batch]
            # 在 host 上按行取，再整批上卡；`dev="cpu"` 时 `.to(dev)` 不做任何事。
            ob, pa, ma = obs[idx].to(dev), P_act[idx].to(dev), M_act[idx].to(dev)
            psel, msel, vv = P_sel[idx].to(dev), M_sel[idx].to(dev), V[idx].to(dev)
            o_act, o_sel, v_hat = net(ob)
            ce = (-(pa * torch.log_softmax(o_act.masked_fill(~ma, -1e9), -1)).sum(-1)
                  - (psel * torch.log_softmax(o_sel.masked_fill(~msel, -1e9), -1)).sum(-1)
                  ).mean()
            loss = ce + (v_hat - vv).pow(2).mean()
            # 发散必须当场报错：NaN/Inf 会一路带到存盘，而 `strict=True` 加载并不报错。
            lv = float(loss.detach())
            if not math.isfinite(lv):
                msg = (f"训练发散：loss={lv} 非有限（epoch {ep + 1}/{epochs}，第 "
                       f"{s // batch + 1}/{-(-n // batch)} 个 batch，"
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
        metrics.scalars(w, ep + 1, **{"train/param_absmax": pmax, "train/dw": drift()})
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
def check_determinism(net: AZNet, data: dict[str, np.ndarray], n: int = 32) -> bool:
    """同一批输入连算两次必须完全一致，用来挡「返回的网络还停在 train 模式」（dropout 开着）。

    先查非有限值再比一致性：`NaN != NaN`，发散时也会返回 False，会被误报成「没切 eval」。
    """
    net.eval()
    x = torch.as_tensor(data["obs"][:n])
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
        return sp

    # 未知子命令和未知 flag 都会在这里报错（退出码 2），不会悄悄当成别的任务跑。
    add("train", "训练一个网络并存盘（评估在 `eval_walk.py`）",
        ("cache", dict()), ("pos_epochs", dict(type=int, nargs="?", default=None)),
        ("out", dict(nargs="?", default=None)),
        ("--name", dict(default=None,
                        help="权重名里的自定义前缀：`<名字>-<数据引用>-<全部配方参数>.pt`")),
        with_epochs=False)
    add("seeds", "多种子训练（并行；评估在 `eval_walk.py`）",
        ("cache", dict()), ("n_seeds", dict(type=int, nargs="?", default=3)),
        ("--seed0", dict(type=int, default=0, help="种子起点（续跑用）")),
        ("--save", dict(default=None, help="路径模板，用 {seed} 占位；不设则不存盘")),
        ("--mp-start", dict(default=None, help="multiprocessing 启动方式（fork/spawn）")))
    return p


def load_cache(path: str) -> dict[str, np.ndarray]:
    """读缓存的所有键；内容是否合法由调用方检查（`check_cache_obs` 与 `train()` 里的
    `check_action_space`）。"""
    z = np.load(path)
    return {k: z[k] for k in z.files}


def ckpt_params(kw: dict[str, Any]) -> dict[str, Any]:
    """进权重文件名的**全部配方参数**（含默认值），按 `train()` 签名的顺序。

    全塞的理由：名字本身就是一份完整配方，不看文件、不查 TB 就能复现。
    建网超参（`hidden/depth/arch/sel_arch/dropout`）**不在签名里** ⇒ 天然不进名字。
    三个例外：`epochs` 由 `ckpt_name` 单独排（紧跟数据集名，位置固定）；`record` / `record_dir`
    是读数落盘（改不了权重）；`init` 是**路径**（进名字会带分隔符，也能把名字撑到几百字符）。
    代价：续训出来的权重与从零训出来的**同名**，后者会覆盖前者。
    """
    return {k: kw[k] for k, p in inspect.signature(train).parameters.items()
            if p.kind is inspect.Parameter.KEYWORD_ONLY
            and k not in ("epochs", "record", "record_dir", "init")}


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
    out = a.out or ckpt_name(a.cache, kw["epochs"], name=a.name, **ckpt_params(kw))
    data = load_cache(a.cache)
    check_cache_obs(data, f"train {a.cache}")
    net, m = train(data, tag=_stem(out), **kw)
    check_determinism(net, data)
    save_ckpt(net, out, cfg=net_cfg(),
              z_spec=kw["z_spec"], **m)
    print(f"已保存 {out}  threads={kw['threads']}  "
          f"device={kw['device']}  batch={kw['batch']}", flush=True)


def run_seeds(a: argparse.Namespace) -> None:
    """`seeds` 子命令：多 seed 并行训练；只有给了 `--save` 才按种子存盘。"""
    kw = train_kwargs(a)
    tag = _stem(a.cache)        # TensorBoard：一个运行名下面按 seed 分子目录，便于并排比较
    n_seeds, s0 = a.n_seeds, a.seed0 or 0
    save_tpl = a.save or ""
    # 这两项检查都放在读缓存之前：参数不对就不必先花几分钟读 GB 级的缓存。
    if n_seeds > 1 and mp_context(a.mp_start).get_start_method() != "fork":
        print(f"❌ `seeds` 的并行训练需要 fork（当前 {mp_context(a.mp_start).get_start_method()!r}）"
              "—— 缓存靠写时复制共享，spawn 的子进程拿不到。请用 `--mp-start fork`"
              "（POSIX）或把 n_seeds 设为 1。", flush=True)
        sys.exit(2)
    if save_tpl and "{seed}" not in save_tpl and n_seeds > 1:
        print(f"❌ --save={save_tpl!r} 没有 {{seed}} 占位符，但 n_seeds={n_seeds} —— "
              f"会把 {n_seeds} 个网络互相覆盖。请写成 '..._s{{seed}}.pt'。", flush=True)
        sys.exit(2)

    data = load_cache(a.cache)
    check_cache_obs(data, f"seeds {a.cache}")
    print(f"样本 {len(data['obs'])}  场 {len(np.unique(data['bid']))}  "
          f"threads={kw['threads']}  device={kw['device']}  "
          f"batch={kw['batch']}  lr={kw['lr']:g}  wd={kw['wd']:g}  "
          f"epochs={kw['epochs']}  "
          f"目标={'软分布' if kw['soft_target'] else '硬argmax'}  z口径={kw['z_spec']}")
    print(f"  建网 {net_cfg()}（固定值，见 `aznet.py`）", flush=True)
    seeds_ = list(range(s0, s0 + n_seeds))
    if s0:
        print(f"种子起点 --seed0 {s0} → 跑 seed {s0}..{seeds_[-1]}", flush=True)

    if n_seeds > 1:                     # 用 fork 让子进程共享数据（`_PAR_DATA` 就是为它存在的）
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
            save_ckpt(net, out, cfg=cfg_net, z_spec=kw["z_spec"], seed=s, **m)
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
