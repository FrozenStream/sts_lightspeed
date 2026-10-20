"""一份采集产物（分片目录）—— 训练 / 统计 / 评估的**唯一**读入口。

生产在 C++（`sts.teacher_run` / `sts.selfplay_run`）：那边只管"把局面搜成 π 再落列"。
这里把裸列变成训练语义：**段**（= 一场）、值目标 `z`、策略目标、留出集、逐批张量。

    d = CollectData("data/xxx")
    d.seg, d.n_games           # 段（终局列连续相同的一段 = 一场）
    d.z("hp"), d.split(0.2, 0) # 值目标（现算）、按段划分留出集
    d.batch(idx)               # 只读这 idx 行的字节 → torch（行号被打乱也行）
    d.epoch_rows(idx, ep, 0)   # 一轮的行序（单份 = 一遍乱序；混训 = 按权重分位）
    d.summary()                # 一行统计

多份**加权混训**（回放缓冲）走 `CollectMix`：同一套出口 + 逐份权重，`parts[0]` 是"本轮"。
轮换自对弈的**开局种子段**见 `round_seed0(轮号, n_per)`（确定性、互不重叠、恒 < `EVAL_SEED0`）。
"""
from functools import cached_property
from math import gcd

import numpy as np

from collect_io import ShardSet, list_shards
from zspec import DEFAULT_SPEC, Z_COLS, z_of

COLLECT_SEED0 = 0
# 评估段起点：1e8 只是"远离采集段"，不是有语义的边界。评估工具拒绝低于它的 seed0。
EVAL_SEED0 = 100_000_000
# 自对弈**轮换段**的起点：`COLLECT_SEED0..SELFPLAY_SEED0` 留给教师 / 手工固定段（那一路还是
# `start_seed=0`）。轮换段从这儿起、到 `EVAL_SEED0` 为止 —— 必须 < 评估段（硬约束）。
SELFPLAY_SEED0 = 1_000_000
MAX_STEPS = 1000

# 1/φ：轮换段的走位步长取"黄金比 × 段数"（低差异 ⇒ 相邻轮跨得远、又不扎堆）。
_GOLDEN = 0.6180339887498949


def round_seed0(round_no: int, n_per: int, seed0: int = SELFPLAY_SEED0) -> int:
    """自对弈第 `round_no` 轮（1 起）的**开局种子段**起点：段 = `[S, S + n_per)`。

    为什么要换段：链条原来每轮都传 `start_seed=0`，而 `encJobs` 的种子是 `start_seed + i`
    ⇒ 每项恒用 seed `0..n_per-1`，**每轮都是同一批开局**（网络在这批上过拟合，覆盖率不随
    轮数涨）。现在每轮换一段，开局分布逐轮展开。代价：**绝对胜率跨轮不可比**（本来也只能看
    配对差 —— 评估段固定，见 `EVAL_SEED0`）。

    段号是轮号的**确定性函数**（不读状态文件）：

        seg = ((round_no - 1) * stride) % n_seg      # n_seg = (EVAL_SEED0 - seed0) // n_per
        gcd(stride, n_seg) == 1                      # ⇒ 走位是整个段空间的**全周期置换**

    ⇒ ① 段**互不重叠**（`n_seg` 轮之内不重复）；② 相邻轮跨得远（黄金比步长，不是逐段 +1）；
    ③ 断点续跑**不必**记"哪轮用了哪一段"（给 `round_no` 就能复算）；链条日志里那一行只是台账。
    ⚠️ 这不是密码学随机 —— 它是**低差异走位**，目的只是"每轮一批没见过的开局"，不是不可预测。
    """
    if round_no < 1:
        raise ValueError(f"轮号从 1 起：{round_no}")
    if n_per < 1:
        raise ValueError(f"每项场数至少 1：{n_per}")
    n_seg = (EVAL_SEED0 - seed0) // n_per
    if n_seg < 1:
        raise ValueError(f"种子段空间不足：seed0={seed0} n_per={n_per}（EVAL_SEED0={EVAL_SEED0}）")
    stride = max(1, int(n_seg * _GOLDEN))
    while gcd(stride, n_seg) != 1:      # 互质才走满一圈；否则会提前踩回已用过的段
        stride += 1
    return seed0 + ((round_no - 1) * stride % n_seg) * n_per

# 一份数据必须有的列（缺了当场报错 —— 别等训练到一半才 KeyError）。
REQUIRED = ("pi_act", "pi_sel", "m_act", "m_sel") + tuple(Z_COLS)


class CollectData:
    """一份采集产物（分片目录）。构造只解析分片头 —— 数据一行都不读。"""

    def __init__(self, path: str):
        self.path = path
        paths = list_shards(path)
        if not paths:
            raise ValueError(f"{path}：没有分片（*.bin）—— 采集产物是**目录**，不是单个文件")
        self.shards = ShardSet(paths)
        s = self.shards
        self.n_rows, self.obs_dim = s.n_rows, s.obs_dim
        self.space = s.space
        self.cols = sorted(s.cols)          # 列名（别的都是能派生的，不存）
        self.check()

    def check(self) -> None:
        """列清单 + obs 宽 + 动作空间标记，不对当场报错。全在分片头/列目录里 ⇒ 不读数据。"""
        from aznet import OBS_DIM, POLICY_DIM

        miss = [c for c in REQUIRED if c not in self.cols]
        if miss:
            raise ValueError(f"{self.path}: 缺列 {miss}（有 {self.cols}）")
        if "obs_raw" not in self.cols and "obs" not in self.cols and not any(
                c.startswith("obs_") for c in self.cols):
            raise ValueError(f"{self.path}: 缺观测列（obs_raw / obs / obs_*）")
        if self.obs_dim != OBS_DIM:
            raise ValueError(f"{self.path}: obs 宽 {self.obs_dim}，当前 {OBS_DIM}")
        if self.space != POLICY_DIM:
            raise ValueError(f"{self.path}: space={self.space}（= `pi_act` 列宽），当前 {POLICY_DIM}")

    # ---------------------------------------------------------------- 段
    @cached_property
    def divisors(self) -> np.ndarray:
        """观测每一列的除数（0 = 类别列，原样保留）。"""
        import slaythespire as sts

        return np.asarray(sts.snapshot_layout()["divisors"], dtype=np.float32)

    @cached_property
    def obs_blocks(self) -> list[str]:
        """v4 观测块（名/顺序；v3 没有，回退单列）。"""
        if "obs_raw" in self.cols or "obs" in self.cols:
            return []
        import slaythespire as sts

        return list(sts.snapshot_layout()["obs_blocks"])
    @cached_property
    def seg(self) -> np.ndarray:
        """每行的**段号**：终局列连续相同的一段 = 一场 = 共享同一个 `z`。

        ⚠️ 落盘里没有场身份（训练不需要它）⇒ 用终局列现推（写侧按场落盘、终局列逐行复制）。
        相邻两场终局列**完全相同**时会被并成一段：值目标逐行算、留出集同段整体归一侧 ⇒ 不受影响。
        """
        term = np.stack([self.shards.col(c) for c in Z_COLS], axis=1)
        if not len(term):
            return np.zeros(0, np.int64)
        return np.concatenate(([0], np.cumsum((term[1:] != term[:-1]).any(axis=1), dtype=np.int64)))

    @property
    def n_games(self) -> int:
        return int(self.seg[-1]) + 1 if len(self.seg) else 0

    def z(self, spec: str = DEFAULT_SPEC, idx=None) -> np.ndarray:
        """值目标 `z`（口径见 `zspec`）。`idx=None` = 整列，否则只算这些行。

        ⚠️ 只读终局列 ⇒ 换口径**既不必重采、也不必重放**。
        """
        return z_of({c: self.shards.col(c, idx) for c in Z_COLS}, spec)

    def split(self, val_frac: float = 0.2, seed: int = 0) -> tuple[np.ndarray, np.ndarray]:
        """按**段**划分留出集（一场整体归一侧；否则同一场的行跨两侧 ⇒ val 指标被自己泄露）。"""
        return split_by_seg(self.seg, val_frac, seed)

    def epoch_rows(self, idx, epoch: int, seed: int = 0):
        """一轮的**行序**：`idx` 的一遍乱序（加权混训见 `CollectMix.epoch_rows`）。

        `epoch` / `seed` 在本类用不上（乱序来自 `train()` 里 `torch.manual_seed` 定的全局流，
        逐位不变）；这两个形参是给混训那一路留的接口 —— `train()` 只认这一个出口。
        """
        import torch

        return idx[torch.randperm(len(idx), device=getattr(idx, "device", None))]

    def batch(self, idx, soft: bool = False, temp: float = 1.0) -> dict[str, object]:
        """`idx` 那些行 → 训练口径的张量（留 host）。**只读这些行、只在这些行上算目标**。

        逐批是必须的：`obs` 每行 6.4 KB，整份物化会吃掉几 GB；而整列算 π 的软目标 / `z`
        会变成"每个 batch 一次 O(N)"。
        """
        import torch

        col = self.shards.col
        if "obs_raw" in self.cols:
            raw_obs = col("obs_raw", idx).astype(np.float32)
            obs = raw_obs.copy()
            nz = self.divisors > 0
            obs[:, nz] /= self.divisors[nz]
        elif self.obs_blocks:
            raw = np.concatenate([col(nm, idx) for nm in self.obs_blocks], axis=1)
            if raw.dtype.kind == "f":
                # v5：写侧已归一化 float。
                obs = raw.astype(np.float32)
            else:
                # v4：i16 原始值 + 除数表。
                obs = raw.astype(np.float32)
                nz = self.divisors > 0
                obs[:, nz] /= self.divisors[nz]
        else:
            obs = col("obs", idx)
        out: dict[str, object] = {
            "obs": torch.as_tensor(obs),
            "m_act": torch.as_tensor(col("m_act", idx).astype(bool)),
            "m_sel": torch.as_tensor(col("m_sel", idx).astype(bool)),
            "pi_act": torch.as_tensor(_targets(col("pi_act", idx), soft, temp)),
            "pi_sel": torch.as_tensor(_targets(col("pi_sel", idx), soft, temp)),
            "z": torch.as_tensor(self.z(DEFAULT_SPEC, idx).astype(np.float32)),
        }
        if "a_act" in self.cols:            # 只有新版采集有（`pg` 口径要用 `gather` ⇒ int64）
            out["a_act"] = torch.as_tensor(col("a_act", idx).astype(np.int64))
            out["a_sel"] = torch.as_tensor(col("a_sel", idx).astype(np.int64))
        return out

    # ---------------------------------------------------------------- 统计
    def summary(self) -> str:
        """一行统计（采集脚本打完就完 —— 不必各自写一遍）。"""
        z = self.z()
        win = float((self.shards.col("t_win") > 0).mean())
        return (f"{self.n_rows} 行 / {self.n_games} 段 · "
                f"space={self.space} obs={self.obs_dim} · "
                f"胜行占比 {win:.1%} · z({DEFAULT_SPEC}) mean {z.mean():+.3f} var {z.var():.3f}")


def _targets(p: np.ndarray, soft: bool, temp: float) -> np.ndarray:
    """策略目标（`soft=False` = 硬 argmax one-hot；`soft=True` = `π**(1/temp)` 归一化）。

    v4 的 `pi_act/pi_sel` 是**未归一访问数**；v3 是已归一 float。先按行和归一，
    两种情况都成立。
    """
    p = np.asarray(p, dtype=np.float32)
    s = p.sum(-1, keepdims=True)
    nz = s[..., 0] > 0
    p = p.copy()
    p[nz] /= s[nz]
    if not soft:
        out = np.zeros_like(p)
        rows = p.sum(-1) > 0
        out[rows, p[rows].argmax(-1)] = 1.0
        return out
    out = np.array(p, dtype=np.float32, copy=True)
    if temp != 1.0:
        np.power(out, 1.0 / temp, out=out)
    s = out.sum(-1, keepdims=True)
    nz = s[..., 0] > 0
    out[nz] /= s[nz]
    return out


def split_by_seg(seg: np.ndarray, val_frac: float, seed: int) -> tuple[np.ndarray, np.ndarray]:
    """按**段**划分留出集：`(训练掩码, 留出掩码)`。一场整体归一侧（同场的行跨两侧 = 自己泄露
    给自己）。`CollectData` / `CollectMix` 共用（后者的段号跨份唯一 ⇒ 同一份内部照样按场分）。
    """
    keys = np.unique(seg)
    rng = np.random.default_rng(seed)
    rng.shuffle(keys)
    val = keys[:max(1, int(round(len(keys) * val_frac)))]
    m = np.isin(seg, val)
    return ~m, m


class CollectMix:
    """多份采集产物的**加权混训**（回放缓冲）：本轮 + 最近几轮（+ 教师数据，如果给）。

    动机：训练集只有本轮数据时，网络在**本轮这批开局**上过拟合、旧状态淡出（历史实测：小份
    等权每轮 −1.41pp；挂锚点/回放才止住 —— 见 `.agent/notes.md` §5-8/16）。这里把多份产物拼成
    一个**虚拟数据集**：全局行号 = 各份首尾相接（份内行号 + 全局偏移），`batch()` 按来源拆开
    再回填（与 `ShardSet.col` 的"命中片 × 命中行 + 位置回填"同形）。

    ⚠️ **`parts[0]` 是"本轮"**（权重恒 1.0，其余权重都相对它）：`[(cur, 1.0), (r-1, 0.6),
       (r-2, 0.36)]` 就是 50.9% / 30.5% / 18.5%（几何衰减 0.6 ⇒ 近三轮 ≈ 50/30/20）。
    ⚠️ **一轮的优化步数不变**：`epoch_rows` 给的行数 = **本轮**的训练行数（不是各份之和），各份
       按权重分这些行位 ⇒ 挂回放**不**等于偷偷多训几步（否则"回放"和"更多训练量"混在一起）。
       代价：权重 < 1 时本轮每轮只走一部分行（`EPOCHS=2` 下平均每行仍见到一次）。
    ⚠️ 各份必须同格式（列名/obs 宽/动作空间一致），否则当场报错 —— 半路 KeyError 更难查。
    """

    def __init__(self, parts: list[tuple[str, float]]):
        if len(parts) < 2:
            raise ValueError(f"CollectMix 至少要两份数据（单份请直接用 CollectData）：{parts!r}")
        self.paths = [p for p, _ in parts]
        self.parts = [CollectData(p) for p in self.paths]
        self.weights = np.asarray([float(w) for _, w in parts], dtype=np.float64)
        if not np.all(self.weights > 0):
            raise ValueError(f"权重必须为正：{list(self.weights)}")
        base = self.parts[0]
        for d in self.parts[1:]:
            if (d.obs_dim, d.space) != (base.obs_dim, base.space):
                raise ValueError(
                    f"{d.path} 与 {base.path} 不是同一种产物："
                    f"obs {d.obs_dim}/{base.obs_dim}、space {d.space}/{base.space}")
        # 份 → 全局行号偏移（`batch` / `epoch_rows` 靠它认来源）
        self.offs = np.cumsum([0] + [d.n_rows for d in self.parts])
        self.n_rows = int(self.offs[-1])
        self.obs_dim, self.space, self.cols = base.obs_dim, base.space, base.cols
        # 段号跨份唯一：递推加"前面各份的段数"
        segs, off = [], 0
        for d in self.parts:
            segs.append(d.seg + off)
            off += d.n_games
        self._seg = np.concatenate(segs)
        self.path = f"{base.path} + {len(self.parts) - 1} 份回放"

    # ---------------------------------------------------------------- 只读面（与 CollectData 同形）
    @property
    def seg(self) -> np.ndarray:
        return self._seg

    @property
    def n_games(self) -> int:
        return int(self._seg[-1]) + 1 if len(self._seg) else 0

    def split(self, val_frac: float = 0.2, seed: int = 0) -> tuple[np.ndarray, np.ndarray]:
        """按**段**划分（跨份统一划一次：各份内部的场也不拆开）。"""
        return split_by_seg(self._seg, val_frac, seed)

    def z(self, spec: str = DEFAULT_SPEC, idx=None) -> np.ndarray:
        """值目标（与 `CollectData.z` 同口径；`idx=None` = 整列）。"""
        zs = [d.z(spec) for d in self.parts]
        if idx is None:
            return np.concatenate(zs)
        return np.concatenate(zs)[np.asarray(idx, dtype=np.int64).ravel()]

    def batch(self, idx, soft: bool = False, temp: float = 1.0) -> dict[str, object]:
        """`idx` 那些行 → 训练张量；**按来源拆开取、再按位置回填**（保持 `idx` 的次序）。

        ⚠️ 位置回填是必须的：`idx` 是**乱序**的（一轮的行位混着各份）⇒ 逐份取出来的块必须
           放回它在 `idx` 里的原位，否则策略目标与观测错位。
        """
        import torch

        i = np.asarray(idx, dtype=np.int64).ravel()
        out: dict[str, object] = {}
        for k, loc, pos in self._route(i):
            tb = self.parts[k].batch(loc, soft, temp)
            if not out:
                out = {nm: torch.empty((len(i),) + tuple(v.shape[1:]), dtype=v.dtype)
                       for nm, v in tb.items()}
            elif set(tb) != set(out):
                raise RuntimeError(f"第 {k} 份的列与第 0 份不同：{sorted(tb)} vs {sorted(out)}")
            pp = torch.as_tensor(pos)
            for nm, v in tb.items():
                out[nm][pp] = v
        return out

    def summary(self) -> str:
        cur, rep = self.parts[0], self.n_rows - self.parts[0].n_rows
        w = self.weights / self.weights.sum()
        return (f"混训 {len(self.parts)} 份 · 本轮 {cur.n_rows} 行 + 回放 {rep} 行 · "
                f"占比 {'/'.join(f'{x:.0%}' for x in w)} · 共 {self.n_rows} 行 / {self.n_games} 段")

    # ---------------------------------------------------------------- 轮内行序
    def epoch_rows(self, idx, epoch: int, seed: int = 0):
        """一轮的**行序**（长度 = **本轮**的训练行数，各份按权重分位）。

        `idx` = 训练行的全局行号（`train()` 按段划完留出集给的）。每份取数 = 该份训练行的一遍
        乱序（份比配额小才循环取 ⇒ 回放份通常远大于配额）⇒ 一轮之内**不重复**（除非份太小）。
        """
        import torch

        a = np.asarray(idx, dtype=np.int64).ravel()
        per = self._by_part(a)
        if not len(per[0]):
            raise RuntimeError(
                f"本轮（{self.paths[0]}）没有训练行 —— 留出集把它切空了？段数 {self.parts[0].n_games}")
        n_ep = len(per[0])                      # 步数锚在本轮上（见类 docstring）
        w = self.weights / self.weights.sum()
        cnt = np.floor(w * n_ep).astype(np.int64)
        cnt[0] += n_ep - int(cnt.sum())         # 取整余数归本轮，总数恰好 = n_ep
        rng = np.random.default_rng([seed, epoch])
        rows = []
        for k, c in enumerate(cnt):
            if c <= 0:
                continue
            if not len(per[k]):
                raise RuntimeError(f"第 {k} 份（{self.paths[k]}）没有训练行")
            rows.append(np.resize(rng.permutation(per[k]), int(c)))
        out = np.concatenate(rows)
        rng.shuffle(out)
        return torch.as_tensor(out, dtype=torch.long, device=getattr(idx, "device", None))

    # ---------------------------------------------------------------- 行号路由
    def _part_of(self, i: np.ndarray) -> np.ndarray:
        """全局行号 → 来源下标（`offs` 是各份起点 ⇒ `searchsorted` 一趟分批）。"""
        return np.searchsorted(self.offs, i, side="right") - 1

    def _by_part(self, i: np.ndarray) -> list[np.ndarray]:
        """全局行号 → 每份一份（**保持 `i` 的次序**）。"""
        k = self._part_of(i)
        return [i[k == j] for j in range(len(self.parts))]

    def _route(self, i: np.ndarray):
        """`[(来源下标, 该份的局部行号, 在 `i` 里的位置)]`（空份不出现）。"""
        k = self._part_of(i)
        for j in range(len(self.parts)):
            pos = np.nonzero(k == j)[0]
            if pos.size:
                yield j, i[pos] - int(self.offs[j]), pos
