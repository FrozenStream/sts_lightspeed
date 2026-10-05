"""`data/` 体检：逐个文件读出**自描述字段**，判它是现役还是过期 —— **不靠文件名**。

用法：
    python audit_data.py [目录]        # 默认 data/

判据（全部来自文件内容；名字只作展示）：
  · `.npz` 缺 `obs_dim`            ⇒ 加标记之前采的 ⇒ 过期（`check_cache_obs` 同判据）
  · `.npz` 的 `obs_dim` ≠ 当前值    ⇒ 布局变过 ⇒ 过期
  · `.npz` 的 `fp` ≠ 当前对应指纹   ⇒ 分布/语义变过 ⇒ 过期（`src` 列说明它是哪条线）
  · `.pt` 的键集 ≠ 当前 `AZNet` 键集 ⇒ 键名变过 ⇒ **死权重**（连 load 都过不去）
  · `.pt` 键集相同但形状不同        ⇒ 只是配置不同（换 `h`/`depth`），不是过期

⚠️ 为什么按内容判：2026-09-25 清 `data/` 时发现名字**根本分不出**版本 ——
`walkdata_n800_s4000`（无版本）/ `walkdata_v3_*` / `walkdata_v4_*` / `_walkv5_n500`
并存，而文件里其实写着 `fp` 与 `obs_dim`。删数据是不可逆的，所以判据必须可复核。
"""
from __future__ import annotations

import os
import sys

import numpy as np

from runtime import DATA_DIR

MB = 1024 * 1024


def _cur_keys():
    """当前默认建网配置的 state_dict 键集（判死权重用，与配置无关）。"""
    try:
        import torch
        from train import net_cfg
        from aznet import AZNet
        return set(AZNet(**net_cfg()).state_dict().keys()), torch
    except Exception as e:                      # noqa: BLE001
        print(f"⚠️ 拿不到当前键集: {e}", file=sys.stderr)
        return None, None


def audit(root=None):
    root = root or DATA_DIR
    cur_keys, torch = _cur_keys()
    obs_dim = policy_dim = None
    try:
        from aznet import OBS_DIM, POLICY_DIM
        obs_dim, policy_dim = OBS_DIM, POLICY_DIM
    except Exception:                           # noqa: BLE001
        pass

    rows = []
    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name)
        if not os.path.isfile(p):
            continue
        size = os.path.getsize(p) / MB
        if name.endswith(".npz"):
            try:
                with np.load(p, allow_pickle=False) as z:
                    keys = set(z.files)
                    got = int(z["obs_dim"]) if "obs_dim" in keys else None
                    sp = int(z["space"]) if "space" in keys else None
                    task = str(z["task"]) if "task" in keys else ""
                    n = len(z["bid"]) if "bid" in keys else None
            except Exception as e:              # noqa: BLE001
                rows.append((size, name, "??", "-", f"读不了: {type(e).__name__}"))
                continue
            if got is None:
                why = "过期: 无 obs_dim 标记"
            elif obs_dim is not None and got != obs_dim:
                why = f"过期: obs_dim {got}≠{obs_dim}"
            elif sp is None:
                why = "过期: 无 space 标记"
            elif policy_dim is not None and sp != policy_dim:
                why = f"过期: 动作空间 {sp}≠{policy_dim}"
            else:
                why = "现役 ✅"
            rows.append((size, name, str(n) if n else "-", task or "-", why))
        elif name.endswith(".pt"):
            if torch is None or cur_keys is None:
                rows.append((size, name, "-", "w", "?（无 torch）"))
                continue
            try:
                from ckpt import weights_of   # 单文件存档要在这里解包（权重 + 配置同档）
                sd = weights_of(p)
                ks = set(sd.keys()) if isinstance(sd, dict) and sd else None
                if not ks:
                    why = "非 state_dict（先验/其它）"
                elif ks == cur_keys:
                    why = "现役 ✅（键集同当前）"
                else:
                    miss = len(cur_keys - ks)
                    extra = len(ks - cur_keys)
                    why = f"死权重: 缺{miss} 多{extra}"
            except Exception as e:              # noqa: BLE001
                why = f"读不了: {type(e).__name__}"
            rows.append((size, name, "-", "w", why))
    return rows


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else None
    rows = audit(root)
    rows.sort(key=lambda r: -r[0])
    print(f"{'MB':>9}  {'文件':<42} {'n':>7}  {'线':<7} 结论")
    dead = 0.0
    for size, name, n, src, why in rows:
        print(f"{size:9.1f}  {name:<42} {n:>7}  {src:<7} {why}")
        if not why.startswith("现役"):
            dead += size
    print(f"\n共 {len(rows)} 个文件；非现役合计 {dead:.0f} MB")


if __name__ == "__main__":
    main()
