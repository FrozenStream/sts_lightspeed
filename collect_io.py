"""自建分片容器（`collect/shardio.h`）的**字节层**读侧 —— 采集产物是纯 C++ 写的裸格式，
不是 npz（C++ 侧不链 numpy/zlib，也不需要 GIL）。

格式（小端）：
    0   8   magic = "STSSHRD1"
    8   4   version
    12  4   ncols
    16  8   nrows
    24  8   space
    32  8   obs_dim
    40  8   task（ASCII，NUL 填充）
    48  8   src（ASCII，NUL 填充）
    56  36*ncols  列目录：[name[16]][dtype[4] 含NUL][width u64][offset u64]
    ...  每列的原始数据（C 序、逐列连续）

`dtype` 用 numpy 的 `descr` 短码（`<f4` / `<f8` / `<u4` / `<i4` / `<u8` / `<i8` / `|b1`；端序显式写）
—— 只为让这里一句 `np.memmap` / `np.frombuffer` 就能用。

⚠️ **分工**：本模块只管"字节在哪、怎么读"；"数据怎么用"（段 / 值口径 / 目标分布 / 留出集 /
逐批张量 / 统计）全在 `collect.CollectData`（训练、统计、评估的唯一入口）。

⚠️ 落盘里**只有训练要用的列**（观测 / 策略 / 掩码 / 实际动作 / 行为概率 / 终局列），没有场身份；
段号由终局列现推（`collect.CollectData.seg`）⇒ 这里不认识"场"。

⚠️ **什么都不常驻**：头解析只读那几十字节、读完即丢；数据靠 `np.memmap` 按页取，`col()` 返回的是
**拷贝**（所以外面不会挂着一个 memmap —— Windows 上那会让文件删不掉）。
"""
import os
import struct
from typing import NamedTuple

import numpy as np

MAGIC = b"STSSHRD1"
HDR = struct.Struct("<8sIIQQQ8s8s")     # magic, version, ncols, nrows, space, obs_dim, task, src
COLDIR = struct.Struct("<16s4sQQ")      # name[16], dtype[4]（含 NUL）, width u64, offset u64


def list_shards(dir_path: str) -> list[str]:
    """一个目录里的分片路径（**按名字序** —— 写侧的分片号补零 ⇒ 名字序 == 行序 == 任务序）。"""
    if not os.path.isdir(dir_path):
        return []
    return sorted(os.path.join(dir_path, n) for n in os.listdir(dir_path) if n.endswith(".bin"))


class Col(NamedTuple):
    """一列的落盘描述：`dtype` = numpy descr 短码、`width` = 每行元素数、`offset` = 片内字节偏移。"""
    dtype: str
    width: int
    offset: int


class Shard(NamedTuple):
    """一个分片的头（**不含数据**）。"""
    path: str
    nrows: int
    space: int
    obs_dim: int
    task: str
    src: str
    cols: dict[str, Col]


def _read_head(path: str) -> Shard:
    """解析一个分片的头 + 列目录 —— **只读那几十字节**，数据一行都不碰。"""
    with open(path, "rb") as fh:
        magic, version, ncols, nrows, space, obs_dim, task, src = HDR.unpack(fh.read(HDR.size))
        dir_bytes = fh.read(COLDIR.size * ncols)
    if magic != MAGIC:
        raise ValueError(f"{path}: 不是本采集容器（magic={magic!r}）")
    if version != 1:
        raise ValueError(f"{path}: 版本 {version} 不认识")
    cols: dict[str, Col] = {}
    for k in range(ncols):
        name, dt, width, off = COLDIR.unpack_from(dir_bytes, k * COLDIR.size)
        cols[name.split(b"\0", 1)[0].decode()] = Col(dt.decode().strip("\0"), int(width), int(off))
    return Shard(path, int(nrows), int(space), int(obs_dim),
                 task.split(b"\0", 1)[0].decode(), src.split(b"\0", 1)[0].decode(), cols)


class ShardSet:
    """一组分片的**只读视图**（按名字序 = 块序；写侧按场序落盘 ⇒ 同一场的行连续）。

        ds = ShardSet(list_shards(dir))
        ds.n_rows, ds.cols, ds.space, ds.obs_dim, ds.task, ds.src
        ds.col("obs")             # 整列（拷进内存；只给小的终局/掩码/π 列用）
        ds.col("obs", idx)        # 只取这些**全局行号**的那些行（乱序 ⇒ 按 idx 序返回）
    """

    def __init__(self, paths: list[str]):
        if not paths:
            raise ValueError("ShardSet：没有分片")
        self.heads = [_read_head(p) for p in paths]
        h0 = self.heads[0]
        for h in self.heads[1:]:
            if {n: c[:2] for n, c in h.cols.items()} != {n: c[:2] for n, c in h0.cols.items()}:
                raise ValueError(f"{h.path} 的列名/dtype/宽度与 {h0.path} 不同")
            if (h.space, h.obs_dim, h.task, h.src) != (h0.space, h0.obs_dim, h0.task, h0.src):
                raise ValueError(f"{h.path} 的 space/obs_dim/task/src 与 {h0.path} 不同")
        self.cols, self.space = h0.cols, h0.space
        self.obs_dim, self.task, self.src = h0.obs_dim, h0.task, h0.src
        self.offs = np.cumsum([0] + [h.nrows for h in self.heads])
        self.n_rows = int(self.offs[-1])

    def col(self, name: str, idx=None) -> np.ndarray:
        """列 `name`：`idx=None` = 整列；否则只取这些**全局行号**（乱序 ⇒ 按 idx 序返回）。

        ⚠️ 只读命中的片 × 命中的行（`np.memmap` 让 OS 按页取 —— `obs` 每行 6.4 KB，整列有几 GB）。
        """
        if name not in self.cols:
            raise KeyError(f"分片里没有列 {name!r}（有：{sorted(self.cols)}）")
        if idx is None:
            parts = [np.array(self._view(h, name)) for h in self.heads]
            return parts[0] if len(parts) == 1 else np.concatenate(parts)
        i = np.asarray(idx, dtype=np.int64).ravel()     # torch 张量 / list / ndarray 都收
        if len(self.heads) == 1:
            return self._view(self.heads[0], name)[i]
        c = self.cols[name]
        out = np.empty((i.size,) + ((c.width,) if c.width > 1 else ()), np.dtype(c.dtype))
        for k, h in enumerate(self.heads):
            lo, hi = int(self.offs[k]), int(self.offs[k + 1])
            pos = np.nonzero((i >= lo) & (i < hi))[0]     # 记的是**在 idx 里的位置** ⇒ 回填天然保序
            if pos.size:
                out[pos] = self._view(h, name)[i[pos] - lo]
        return out

    @staticmethod
    def _view(h: Shard, name: str) -> np.ndarray:
        """该片该列的整列视图（`np.memmap` ⇒ 不常驻；临时对象，返回后由 GC 释放）。

        `memmap` 的 `offset` 必须整除 itemsize（写侧逐列连续 ⇒ 一般成立）；不成立就退回
        `frombuffer`（那一片常驻，不影响正确性）。
        """
        c = h.cols[name]
        dt = np.dtype(c.dtype)
        n = h.nrows * c.width
        if c.offset % dt.itemsize:
            with open(h.path, "rb") as fh:
                fh.seek(c.offset)
                a = np.frombuffer(fh.read(n * dt.itemsize), dt)
        else:
            a = np.memmap(h.path, dt, "r", offset=c.offset, shape=(n,))
        return a.reshape(h.nrows, c.width) if c.width > 1 else a
