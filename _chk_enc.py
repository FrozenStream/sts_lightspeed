"""把缓存的决策点按**遭遇战**归因 —— 回答「多怪状态为什么这么少」。

    用法: _chk_enc.py <缓存.npz> [n_probe]

按 **Boss / 遭遇组** 分组产出：
  · 场次数、占比
  · **教师胜率**（直接读缓存的 `t_win`，零重放成本）
  · 平均决策点数/场（打得多长）
  · ⭐ **至少出现过一次多怪的场次占比** —— 关键：分裂到底发生了没有
  · 多怪行占该组全部行的比例、以及占**全部多怪行**的比例

每场自带遭遇（`assemble` 总是写 `enc` 列）⇒ 按 `bid` 分组，零重放，换机器照样能读。

两种可能的归因（本脚本用来分辨）：
  A. 史莱姆王出现得少 ⇒ 「占比」那一列小
  B. 出现了但活不到分裂 / 分裂后很快打完 ⇒ 「有多怪的场占比」或「多怪行占比」小
  C. 打不过所以早死 ⇒ 该 Boss 的教师胜率低 **且** 决策点/场少
"""
import collections
import os
import sys

import numpy as np
import slaythespire as sts

from aznet import MON_TARGETABLE_IDX


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    cache = sys.argv[1]
    d = dict(np.load(cache, allow_pickle=False))
    for k in ("seed", "obs", "t_win"):
        if k not in d:
            print(f"❌ 缓存缺 `{k}` 列（旧格式？先跑 `z_revalue.py rew`）", file=sys.stderr)
            return 2

    seed_row, tw = d["seed"], d["t_win"]
    ntg = np.asarray(d["obs"])[:, MON_TARGETABLE_IDX].sum(1)
    # 每场一行。分组键用 **`bid`** —— ⚠️ 在「按遭遇组采集」的缓存里 `seed` 会**跳组重复**
    #    （每组都从同一个 seed 起点采）⇒ 按 seed 分组会把不同场并成一场。
    if "enc" not in d:
        raise SystemExit("✗ 缓存没有 `enc` 列（旧格式）⇒ 认不出遭遇，请重新采集")
    idx = np.unique(d["bid"], return_inverse=True)[1]
    first = np.unique(idx, return_index=True)[1]
    per_unit = d["enc"][first]
    if not np.all(d["enc"] == per_unit[idx]):
        raise SystemExit("✗ 同一场里出现了多个 `enc` —— 缓存坏了")
    keys = [str(sts.MonsterEncounter(int(e))) for e in per_unit]
    who, src_note = "遭遇组", "每场自带 `enc`（零重放）"
    n_rows = np.bincount(idx)
    n_multi = np.bincount(idx, weights=(ntg >= 2).astype(float))
    win = np.bincount(idx, weights=tw) / n_rows              # 同场每行的 t_win 相同

    n_units = len(keys)
    n_probe = int(sys.argv[2]) if len(sys.argv) > 2 else n_units
    if n_probe < n_units:
        pick = np.sort(np.random.default_rng(0).choice(n_units, size=n_probe, replace=False))
    else:
        pick = np.arange(n_units)

    print(f"缓存 {os.path.basename(cache)}：{n_units} 场 / {len(seed_row)} 行（{src_note}）")
    print(f"探测 {len(pick)} 场（{'全量' if len(pick) == n_units else '随机抽样，n 受限'}）",
          flush=True)
    encs = [keys[j] for j in pick]

    agg: dict = collections.defaultdict(
        lambda: dict(n=0, rows=0, multi_rows=0, wins=0.0, multi_battles=0, fail=0))
    for j, e in zip(pick, encs):
        a = agg[e]
        a["n"] += 1
        a["rows"] += int(n_rows[j])
        a["multi_rows"] += int(n_multi[j])
        a["wins"] += float(win[j])
        a["multi_battles"] += int(n_multi[j] > 0)
    tot_multi = sum(a["multi_rows"] for a in agg.values()) or 1
    tot_rows = sum(a["rows"] for a in agg.values())

    print(f"\n{who:<30s}{'场次':>6s}{'占比':>7s}{'教师胜率':>9s}{'决策点/场':>10s}"
          f"{'出现过怪的场':>12s}{'多怪行/该Boss行':>16s}{'占全部多怪行':>12s}")
    for e, a in sorted(agg.items(), key=lambda kv: -kv[1]["n"]):
        print(f"{e:<30s}{a['n']:>6d}{a['n'] / len(pick) * 100:>6.1f}%"
              f"{a['wins'] / max(a['n'], 1) * 100:>8.1f}%"
              f"{a['rows'] / max(a['n'], 1):>10.1f}"
              f"{a['multi_battles'] / max(a['n'], 1) * 100:>11.1f}%"
              f"{a['multi_rows'] / max(a['rows'], 1) * 100:>15.2f}%"
              f"{a['multi_rows'] / tot_multi * 100:>11.1f}%")
    print(f"{'合计':<30s}{len(pick):>6d}{100.0:>6.1f}%"
          f"{sum(a['wins'] for a in agg.values()) / max(len(pick), 1) * 100:>8.1f}%"
          f"{tot_rows / max(len(pick), 1):>10.1f}"
          f"{sum(a['multi_battles'] for a in agg.values()) / max(len(pick), 1) * 100:>11.1f}%"
          f"{sum(a['multi_rows'] for a in agg.values()) / max(tot_rows, 1) * 100:>15.2f}%"
          f"{100.0:>11.1f}%")
    print("\n读法：若「出现过怪的场」很高而「多怪行/该Boss行」很低 ⇒ 分裂**发生了但很快打完**；"
          "\n      若前者本身就低 ⇒ **活不到分裂**（多因早死）；若某 Boss 的「占比」低 ⇒ 只是出现得少。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
