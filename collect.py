"""一份采集产物（分片目录）—— 训练 / 统计 / 评估的**唯一**读入口。

生产在 C++（`sts.teacher_run` / `sts.selfplay_run`）：那边只管"把局面搜成 π 再落列"。
这里把裸列变成训练语义：**段**（= 一场）、值目标 `z`、策略目标、留出集、逐批张量。

    d = CollectData("data/xxx")
    d.seg, d.n_games           # 段（终局列连续相同的一段 = 一场）
    d.z("hp"), d.split(0.2, 0) # 值目标（现算）、按段划分留出集
    d.batch(idx)               # 只读这 idx 行的字节 → torch（行号被打乱也行）
    d.summary()                # 一行统计
"""
from functools import cached_property

import numpy as np

from collect_io import ShardSet, list_shards
from zspec import DEFAULT_SPEC, Z_COLS, z_of

COLLECT_SEED0 = 0
# 评估段起点：1e8 只是"远离采集段"，不是有语义的边界。评估工具拒绝低于它的 seed0。
EVAL_SEED0 = 100_000_000
MAX_STEPS = 1000

# 一份数据必须有的列（缺了当场报错 —— 别等训练到一半才 KeyError）。
REQUIRED = ("obs", "pi_act", "pi_sel", "m_act", "m_sel") + tuple(Z_COLS)


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
        if self.obs_dim != OBS_DIM:
            raise ValueError(f"{self.path}: obs 宽 {self.obs_dim}，当前 {OBS_DIM}")
        if self.space != POLICY_DIM:
            raise ValueError(f"{self.path}: space={self.space}（= `pi_act` 列宽），当前 {POLICY_DIM}")

    # ---------------------------------------------------------------- 段
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
        keys = np.unique(self.seg)
        rng = np.random.default_rng(seed)
        rng.shuffle(keys)
        val = keys[:max(1, int(round(len(keys) * val_frac)))]
        m = np.isin(self.seg, val)
        return ~m, m

    def batch(self, idx, soft: bool = False, temp: float = 1.0) -> dict[str, object]:
        """`idx` 那些行 → 训练口径的张量（留 host）。**只读这些行、只在这些行上算目标**。

        逐批是必须的：`obs` 每行 6.4 KB，整份物化会吃掉几 GB；而整列算 π 的软目标 / `z`
        会变成"每个 batch 一次 O(N)"。
        """
        import torch

        col = self.shards.col
        out: dict[str, object] = {
            "obs": torch.as_tensor(col("obs", idx)),
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

    全零行（`pi_act` 的选牌行、非选牌行的 `pi_sel`）保持全零 —— 它们在交叉熵里贡献 0。
    """
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
