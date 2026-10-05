"""训练/迭代体检：读落盘读数（`metrics.py`）与迭代日志，打出曲线要点。

用法：
    python _progress.py [run 的 seed 目录 …]
    （不给参数 = `data/runs` 下最近改动过的那个 run）

打点粒度见 `train.train()`（落盘格式与绘图方式见 `metrics.py`）：
  · 每 batch：`train/loss`、`train/grad_norm`（横轴 = 全局 batch 号）
  · 每 epoch：`train/param_absmax`
  · 每 run 至少 8 个点（含末尾）：`train/ce`、`train/r2`、`val/ce`、`val/r2`

⚠️ run 名取自训练数据的短名，**同一份数据训两次会写进同一个 run**，曲线彼此叠加；
`recipe.json` 的 `rows` 出现两个不同值即为此征兆。
"""
import argparse
import glob
import json
import os
import re
import sys
import unicodedata

from zspec import Z_COLS, baseline_win_rate

SPARK = "▁▂▃▄▅▆▇█"      # 迷你曲线：把 n 个采样点压成固定宽度的一行
SPARK_N = 16            # 终端里每条的曲线宽度（字符）

# 逐场行的列 = `("seed",) + Z_COLS`（与 `eval_encs.ROW_COLS` 同源，不另抄字段名）
ROW = ("seed",) + tuple(Z_COLS)
I_WIN, I_HP, I_MAXHP = (ROW.index(c) for c in ("t_win", "t_hp", "t_max_hp"))


def latest_run(root: str) -> str | None:
    """`root` 下最近被写过的 run 目录（`points.jsonl` 的 mtime 最新者）。

    ⚠️ 不能用**目录** mtime：`points.jsonl` 是常开句柄、只在首次创建时改动目录项
    ⇒ 训练全程目录 mtime 不变，会挑到别的 run。
    """
    dirs = [d for d in glob.glob(os.path.join(root, "*", "*")) if os.path.isdir(d)]
    if not dirs:                       # `sub=None` 的 run 直接落在 root 下一层
        dirs = [d for d in glob.glob(os.path.join(root, "*")) if os.path.isdir(d)]

    def mt(d: str) -> float:
        p = os.path.join(d, "points.jsonl")
        return os.path.getmtime(p) if os.path.exists(p) else os.path.getmtime(d)

    return max(dirs, key=mt) if dirs else None


def _spark(vals: list[float]) -> str:
    """把一串值压成固定宽度的块字符迷你曲线（值域拉满；全等值给同高）。"""
    lo, hi = min(vals), max(vals)
    if hi == lo:
        return SPARK[3] * len(vals)
    return "".join(SPARK[min(7, int((v - lo) / (hi - lo) * 8))] for v in vals)


def dump(run_dir: str) -> None:
    """打出一个 run 的全部标量：每个 tag 一行（迷你曲线 + 首→末）。"""
    print(f"=== {run_dir} ===")
    path = os.path.join(run_dir, "points.jsonl")
    if not os.path.exists(path):
        print("  (没有 points.jsonl)")
        return
    series: dict[str, list[tuple[int, float]]] = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                r = json.loads(line)
            except json.JSONDecodeError:      # 半行（进程被杀）不该让体检整个失败
                continue
            series.setdefault(r["tag"], []).append((int(r["step"]), float(r["value"])))
    if not series:
        print("  (空)")
        return
    for tag in sorted(series):
        ev = series[tag]
        n = len(ev)
        k = max(1, n // SPARK_N)
        vs = [v for _, v in ev[::k]]
        if vs[-1] != ev[-1][1]:          # 采样可能不含末尾点
            vs.append(ev[-1][1])
        print(f"  {tag:20s} n={n:6d}  {_spark(vs)}  {vs[0]:.4g} → {vs[-1]:.4g}")


GCOL = (("轮次", 21, False), ("候选/best", 26, False), ("候选", 7, True),
        ("best", 7, True), ("差", 7, True), ("战损", 7, True),
        ("A赢B输", 8, True), ("B赢A输", 8, True))


def _pad(s, w: int, right: bool = False) -> str:
    """按**显示宽度**补齐：`str.ljust` 把 CJK 当 1 列 ⇒ 中文表头会与 ASCII 数据行错位。"""
    s = str(s)
    sp = " " * max(0, w - _w(s))
    return sp + s if right else s + sp


def _w(s: str) -> int:
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)


def _clip(s: str, w: int) -> str:
    """按显示宽度截断（超宽会把后面所有列顶掉，比截断更难发现）。"""
    if _w(s) <= w:
        return s
    out, cur = "", 0
    for c in s:
        cw = 2 if unicodedata.east_asian_width(c) in "WF" else 1
        if cur + cw > w - 1:
            break
        out, cur = out + c, cur + cw
    return out + "…"


def _row(cells) -> str:
    return "  " + "".join(_pad(_clip(str(s), w), w, r) for s, (_, w, r) in zip(cells, GCOL))


def _short(spec: str) -> str:
    """`net:/…/train_1-e2-lr0.0001-….pt` → `train_1`（剥方案前缀，只取权重名第一段）。"""
    s = re.sub(r"[\w./-]*/([\w.-]+?)\.pt", lambda m: m.group(1).split("-")[0], spec)
    return re.sub(r"^net:", "", s)


def _gate_key(path: str) -> tuple[str, int]:
    """`…/chain_X/gate_7.json` → `("chain_X", 7)`；按**轮号**排（字符串序会把 g10 排在 g2 前）。"""
    m = re.search(r"gate_(\d+)\.json$", path)
    return os.path.basename(os.path.dirname(path)), int(m.group(1)) if m else 0


def _rows_of(gate: dict, spec: str) -> list:
    """一轮门控里某策略的全部逐场行（`eval_encs.py`：`items[*].per_spec[spec].rows`）。"""
    return [r for it in gate["items"] for r in it["per_spec"][spec]["rows"]]


def _agg(rows) -> tuple[int, float, float]:
    """（场数，胜率，战损。口径同 `eval_encs.summarize`：战损只算胜局。）"""
    n = len(rows)
    won = [r for r in rows if r[I_WIN]]
    hp = 1.0 - sum(r[I_HP] / r[I_MAXHP] for r in won) / len(won) if won else float("nan")
    return n, len(won) / max(n, 1), hp


def chart(log_root: str, max_rounds: int = 60) -> None:
    """强度一行摘要：每轮**配对差** Δ = 候选−best（同棋盘，跳轮可比）。

    绝对胜率不跳轮可比（每轮换一批 seed 段，同一冻结权重实测能差 ~2pp）⇒ 只给 Δ。
    正式折线图（Δ + 95% 区间 + 达标线，绝对胜率当背景）是 `_plot.py` 出的 PNG。
    """
    pts: list[tuple[int, float, int]] = []
    for f in sorted(glob.glob(os.path.join(log_root, "chain_*", "gate_*.json")), key=_gate_key):
        with open(f, encoding="utf-8") as fh:
            d = json.load(fh)
        if not isinstance(d, dict) or "items" not in d:      # 旧 walk schema：跳过
            continue
        n, wc, _ = _agg(_rows_of(d, d["specs"][0]))          # [0]=候选 [−1]=best
        _, wb, _ = _agg(_rows_of(d, d["specs"][-1]))
        pts.append((_gate_key(f)[1], wc - wb, round(n / max(len(d["items"]), 1))))
    if len(pts) < 2:
        print(f"（只有 {len(pts)} 轮门控结果，画不出曲线）")
        return
    pts = pts[-max_rounds:]
    per = sorted({p[2] for p in pts})
    cal = f"每项 {per[-1]} 场" + (f"，第 {pts[0][0]} 轮前为 {per[0]} 场" if len(per) > 1 else "")
    dl = [p[1] * 100 for p in pts]
    print(f"=== 强度（配对差 候选−best · encs · 第 {pts[0][0]}~{pts[-1][0]} 轮 · {cal}）===")
    print(f"  {_spark(dl)}  平均 {sum(dl) / len(dl):+.2f}pp  末轮 {dl[-1]:+.2f}pp"
          f"   图：  ./data/plots/strength.png")
    bl = baseline_win_rate()
    if bl:
        print(f"  基准（固定 seed 段）: {bl[0]} = {bl[1]:.1%}")


def gates(log_root: str, brief: bool = False) -> None:
    """门控逐轮：候选 vs 历史 best（裸网络、encs 同分布、同 seed 配对）。

    `A赢B输/B赢A输` = 同一场（同 seed）上的配对计数；两列胜率看着相等时，配对计数才说得上话。
    p 值 / 逐项战报在 `gate_*.log`（每轮也追加到 `summary.txt`）。
    `brief` = 监控循环用：不重印 `gate_history.txt`（表格已有），逐怪群只列前 5。
    """
    files = sorted(glob.glob(os.path.join(log_root, "chain_*", "gate_*.json")), key=_gate_key)
    note = ""
    keep = 6 if brief else len(files)     # 轮数会无限长 ⇒ 监控帧里只留最近 6 轮
    if brief and len(files) > keep:
        note = f"，前 {len(files) - keep} 轮略"
        files = files[-keep:]
    if not files:
        print(f"（{log_root} 下还没有门控结果）")
    else:
        print(f"=== 门控逐轮（裸网络 · encs{note}）===")
        print(_row([c[0] for c in GCOL]))
        for f in files:
            with open(f, encoding="utf-8") as fh:
                d = json.load(fh)
            if not isinstance(d, dict) or "items" not in d:      # 旧 walk schema：跳过
                continue
            cand, best = d["specs"][0], d["specs"][-1]
            _, wc, hc = _agg(_rows_of(d, cand))
            _, wb, _ = _agg(_rows_of(d, best))
            # 配对按 **(项, seed)**：同一段 seed 每项共用 ⇒ 只按 seed 配会跨项串味
            a_only = b_only = 0
            for it in d["items"]:
                rc = {r[0]: r[I_WIN] for r in it["per_spec"][cand]["rows"]}
                rb = {r[0]: r[I_WIN] for r in it["per_spec"][best]["rows"]}
                for sd, x in rc.items():
                    y = rb.get(sd)
                    if y is None:            # 对面缺这一行（stall 未落行）⇒ 不算胜负
                        continue
                    a_only += x and not y
                    b_only += y and not x
            tag = os.path.basename(os.path.dirname(f))
            rnd = os.path.basename(f)[len("gate_"):-len(".json")]
            cb = f"{_short(cand)}/{_short(best)}"
            print(_row([f"{tag[:16]}/g{rnd}", cb, f"{wc:.1%}", f"{wb:.1%}",
                        f"{wc - wb:+.1%}", f"{hc:.1%}", a_only, b_only]))
    if not brief:
        for f in sorted(glob.glob(os.path.join(log_root, "chain_*", "gate_history.txt"))):
            print(f"=== {f} ===")
            with open(f, encoding="utf-8") as fh:
                for ln in fh:
                    print("  " + _short(ln).rstrip("\n"))
    per_enc(log_root, top=3 if brief else 10)


def per_enc(log_root: str, top: int = 10) -> None:
    """逐怪群跨轮胜率变化：找"平均值看不出来、但某一项在烂"的退化。

    每个 json 取**最后一行**（当前 best，不是候选）当该轮的能力快照，按"最早轮 → 最近轮"
    的胜率差排序，只列变化最大的 `top` 项。样本太少的怪群不参与（噪声会盖过信号）。
    """
    snaps = []
    for f in sorted(glob.glob(os.path.join(log_root, "chain_*", "gate_*.json")), key=_gate_key):
        with open(f, encoding="utf-8") as fh:
            d = json.load(fh)
        if not isinstance(d, dict) or "items" not in d:      # 旧 walk schema：跳过
            continue
        spec = d["specs"][-1]
        agg = {}
        for it in d["items"]:
            n, w, _ = _agg(it["per_spec"][spec]["rows"])
            if n >= 20:
                agg[it["enc"]] = (w, n)
        snaps.append((f"g{_gate_key(f)[1]} {_short(spec)}", agg))
    if len(snaps) < 2:
        print(f"（只有 {len(snaps)} 轮门控结果，跳轮对照需要 ≥2 轮）")
        return
    first, last = snaps[0], snaps[-1]
    deltas = []
    for k, (w0, n0) in first[1].items():
        if k in last[1]:
            w1, n1 = last[1][k]
            deltas.append((w1 - w0, k, w0, w1, n0, n1))
    deltas.sort()
    # 端点写**轮号 + 该轮 best 的名**：链条目录两端永远同名（同一轮次目录）⇒ 写链名等于没说。
    print(f"=== 逐怪群退化最大者（{first[0]} → {last[0]}，每项 ≥20 场）===")
    for dlt, k, a, b, n0, n1 in deltas[:top]:
        print(f"  {k:<28}{a:>7.1%} → {b:>7.1%}  {dlt:>+7.1%}   (n {n0}→{n1})")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("runs", nargs="*", help="run 的 seed 目录（默认：最近改动的那个）")
    p.add_argument("--root", default="data/runs", help="run 根目录")
    p.add_argument("--logs", default="data/logs", help="迭代日志根目录")
    p.add_argument("--brief", action="store_true",
                   help="只打门控要点（监控循环用）：不印曲线块、逐怪群只列前 5")
    a = p.parse_args()
    runs = list(a.runs)
    if not runs:
        last = latest_run(a.root)
        if last:
            runs = [last]
    if not runs and not a.brief:
        print(f"（{a.root} 下没有 run）")
    for d in runs:
        if not a.brief:
            dump(d)
    chart(a.logs, max_rounds=40 if a.brief else 60)
    gates(a.logs, brief=a.brief)
    return 0


if __name__ == "__main__":
    sys.exit(main())
