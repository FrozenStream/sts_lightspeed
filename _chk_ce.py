"""策略交叉熵的**下限**体检：`ce` 不可能低于目标自身的熵，先算出下限才知道 ce 是高是低。

    python _chk_ce.py [npz ...] [--ce X] [--val-ce Y]

不给 npz = 最新一轮自对弈 + 教师锚点（当前训练集的两半）。训练侧按**行**取平均，
所以混合下限按行数加权。差 `ce − 下限` 就是策略头还剩多少可学空间：
  · 接近 0 ⇒ 这目标分布已经被学到头了，再涨只能靠**更好的目标**（更深的搜索/更少的噪声）或值头；
  · 还很大 ⇒ 策略头欠拟合，继续训有效。
⚠️ 别跨口径比 ce 的绝对值：软目标的下限是 H(π)、硬目标（one-hot）的下限是 0。
"""
import argparse
import glob
import sys

import numpy as np


def row_H(pi: np.ndarray) -> tuple[np.ndarray, int]:
    """每行的熵（只算非全零行）+ 有效行数。"""
    s = pi.sum(-1)
    k = s > 0
    q = pi[k] / s[k, None]
    return -(q * np.log(q + 1e-12)).sum(-1), int(k.sum())


def floor_of(path: str) -> tuple[float, int, str]:
    """→ `(按行平均的 ce 下限, 行数, 明细文字)`。"""
    d = np.load(path)
    ha, na = row_H(np.asarray(d["pi_act"], dtype=np.float64))
    hs, ns = row_H(np.asarray(d["pi_sel"], dtype=np.float64))
    n = len(d["pi_act"])
    f = (float(ha.sum()) + float(hs.sum())) / n
    det = (f"{path.split('/')[-1][:38]:<40} 行 {n:>7d}  act 有效 {na:>7d} sel {ns:>6d}  "
           f"熵(act {ha.mean():.3f} / sel {hs.mean():.3f})  ⇒ 下限 {f:.3f}")
    return f, n, det


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("npz", nargs="*", help="缓存（默认：最新一轮自对弈 + 教师锚点）")
    p.add_argument("--ce", type=float, default=None, help="对照的训练 ce")
    p.add_argument("--val-ce", type=float, default=None, help="对照的验证 ce")
    a = p.parse_args()

    srcs = a.npz or (sorted(glob.glob("data/it*_*.npz"))[-1:] +
                     sorted(glob.glob("data/encdata_*.npz"))[-1:])
    if not srcs:
        sys.exit("找不到缓存（给路径或先跑一轮采集）")
    tot_f, tot_n, dets = 0.0, 0, []
    for s in srcs:
        f, n, det = floor_of(s)
        tot_f, tot_n = tot_f + f * n, tot_n + n
        dets.append(det)
    mix = tot_f / tot_n
    print("\n".join(dets))
    print(f"\n混合下限（按行加权）≈ {mix:.3f}   行 {tot_n}")
    for tag, v in (("train ce", a.ce), ("val ce", a.val_ce)):
        if v is not None:
            print(f"  {tag} {v:.3f} → 距下限 {v - mix:+.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
