"""强度图：门控逐轮的胜率 → PNG。**每次覆盖同一路径**（不留档，没有按轮次命名的副本）。

用法：python _plot.py [chain 日志目录] [输出 png]
      默认 `data/logs` 下最新的 `chain_*` → `./data/plots/strength.png`（带 `./` 前缀，
      终端才能把它当可点击路径）

读 `eval_encs.py` 的门控 json（`items[*].per_spec`），不碰训练产物。
图上文字一律用 ASCII：远端没有中文字体，CJK 会渲染成方框。
"""
import glob
import json
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")                      # 无显示环境
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator

from zspec import Z_COLS, baseline_win_rate

ROW = ("seed",) + tuple(Z_COLS)
I_WIN = ROW.index("t_win")


def _rnd(path: str) -> int:
    m = re.search(r"gate_(\d+)\.json$", path)
    return int(m.group(1)) if m else 0


MARGIN = 0.01            # 升 best 的门槛，与 `_chain.sh` 的 GATE_MARGIN 同值


def rounds(chain_dir: str) -> list[dict]:
    """每轮一条：轮号、配对 Δ（候选−best）、Δ 标准误、配对场数、两臂绝对胜率、每项场数。

    Δ 是**同棋盘**配对量 ⇒ 棋盘难度互相抵消，跳轮可比；绝对胜率每轮换一批 seed 段，
    水平不可跳轮比，图上只当背景。只收 `eval_encs` 口径（旧 walk json 跳过）。
    """
    out = []
    for f in sorted(glob.glob(os.path.join(chain_dir, "gate_*.json")), key=_rnd):
        with open(f, encoding="utf-8") as fh:
            d = json.load(fh)
        if not isinstance(d, dict) or "items" not in d:      # 旧 walk schema
            continue
        spec_c, spec_b = d["specs"][0], d["specs"][-1]       # [0]=候选 [−1]=best
        only_c = only_b = pair = n_c = n_b = win_c = win_b = 0
        for it in d["items"]:
            rc = {r[0]: r[I_WIN] for r in it["per_spec"][spec_c]["rows"]}
            rb = {r[0]: r[I_WIN] for r in it["per_spec"][spec_b]["rows"]}
            n_c += len(rc)
            n_b += len(rb)
            win_c += sum(rc.values())
            win_b += sum(rb.values())
            for sd, x in rc.items():
                y = rb.get(sd)
                if y is None:            # 对面缺这一行（stall 未落行）⇒ 不算胜负
                    continue
                pair += 1
                only_c += x and not y
                only_b += y and not x
        out.append(dict(r=_rnd(f), d=(only_c - only_b) / max(pair, 1),
                        se=(only_c + only_b) ** 0.5 / max(pair, 1), pair=pair,   # McNemar
                        wc=win_c / max(n_c, 1), wb=win_b / max(n_b, 1),
                        per=round(n_c / max(len(d["items"]), 1))))
    return out


def main() -> int:
    arg = sys.argv[1:]
    if arg:
        chain = arg[0]
    else:
        cand = sorted(glob.glob("data/logs/chain_*"), key=os.path.getmtime)
        if not cand:
            print("（data/logs 下没有 chain_*）")
            return 1
        chain = cand[-1]
    out = arg[1] if len(arg) > 1 else "./data/plots/strength.png"

    rs = rounds(chain)
    if len(rs) < 2:
        print(f"（{chain} 只有 {len(rs)} 轮门控结果，画不出图）")
        return 1

    x = [p["r"] for p in rs]
    dd = [p["d"] * 100 for p in rs]
    se = [p["se"] * 196 for p in rs]               # 95% 置信区间（百分数）
    fig, ax = plt.subplots(figsize=(9, 4.2), dpi=110)
    ax.axhline(0, color="0.65", lw=1)
    ax.axhline(MARGIN * 100, color="C3", lw=1, ls="--", label=f"accept bar ({MARGIN:.0%})")
    ax.errorbar(x, dd, yerr=se, fmt="none", color="C0", lw=1.4, capsize=2, zorder=3,
                label="paired delta, 95% CI")
    ax.plot(x, dd, "-", color="C0", lw=1.2, zorder=3)
    for xi, di, p in zip(x, dd, rs):               # 达标（会升 best）实心，未达标空心
        ax.plot([xi], [di], "o", ms=4.5, color="C0", zorder=4,
                markerfacecolor="C0" if p["d"] >= MARGIN else "white")
    # 绝对胜率退成背景（右轴、细灰线）：每轮换一批 seed 段，水平不可跨轮比
    ax2 = ax.twinx()
    ax2.plot(x, [p["wc"] * 100 for p in rs], color="0.78", lw=1, label="candidate abs")
    ax2.plot(x, [p["wb"] * 100 for p in rs], color="0.55", lw=1, ls="--", label="best abs")
    bl = baseline_win_rate()                           # 教师基准：固定 seed 段的参照水平
    if bl:
        ax2.axhline(bl[1] * 100, color="0.3", lw=1.2, ls="-.", label=f"{bl[0]} (baseline)")
    ax2.set_ylabel("absolute win rate (%), background only", color="0.45", fontsize=9)
    ax2.tick_params(colors="0.45", labelsize=8)
    # 场数口径变过的地方画条竖线：跨口径的点不能连成一条线读（变更内容写进标题）
    per = [p["per"] for p in rs]
    chg = []
    for i in range(1, len(rs)):
        if per[i] != per[i - 1]:
            ax.axvline(x[i] - 0.5, color="C3", lw=1, ls=":")
            chg.append(f"round {rs[i]['r']}+: {per[i - 1]} -> {per[i]} games/item")
    ax.set_xlabel("round")
    ax.set_ylabel("paired delta, candidate - best (pp)")
    ax.set_ylim(min(dd + [0.0, MARGIN * 100]) - 0.6, max(dd + [0.0, MARGIN * 100]) + 0.6)
    tail = "  [" + "; ".join(chg) + "]" if chg else ""
    ax.set_title(f"{os.path.basename(chain)}  bare net, no search, {per[-1]} games/item{tail}")
    ax.xaxis.set_major_locator(MaxNLocator(integer=True))
    ax.grid(alpha=0.25)
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=8, ncol=2, framealpha=0.9)
    fig.tight_layout()

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    fig.savefig(out)                       # 覆盖，不留档
    plt.close(fig)
    print(f"已覆盖 {out}（{len(rs)} 轮：Δ 平均 {sum(dd) / len(dd):+.2f}pp"
          f"，末轮 {dd[-1]:+.2f}pp，配对 {rs[-1]['pair']} 场）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
