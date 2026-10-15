"""门控触发器：配对评估多个策略，报胜率（评估全在 C++ 的 `sts.eval_run`）。

    python eval_gate.py <每组场数> <策略,策略,…> [seed0] [out.json] [--games N]

策略：`random` | `teacher:<sims>` | `net:<path>[:<sims>]`（权重自动 trace 成 TorchScript；
**不给 `:sims` = 裸网络**（逐决策点取网络先验 argmax，无搜索遮挡），给 `:sims`(≥2) 才走它自己的 MCTS）。
任务表 / 造局面 / 搜索 / 统计都在 C++，同一批 `(遭遇, 层, seed)` 喂给所有策略 ⇒ 胜率差 = 策略差。
"""
import argparse
import json
import os
import sys
import time

import torch  # noqa: F401  ⚠️ 必须在 `slaythespire` 之前（.pyd 静态链 libtorch）
import slaythespire as sts

import ckpt
from collect import EVAL_SEED0, MAX_STEPS
from tsnet import ensure_ts
from zspec import DEFAULT_SPEC


def parse_spec(spec: str, device: str = "cpu") -> sts.EvalSpec:
    """`random` | `teacher:<sims>` | `net:<path>[:<sims>]` → `sts.EvalSpec`（看不懂就报错）。"""
    p = spec.split(":")
    e = sts.EvalSpec()
    e.label, e.device, e.z_spec = spec, device, DEFAULT_SPEC
    if p[0] == "random" and len(p) == 1:
        e.kind = "random"
    elif p[0] == "teacher" and len(p) == 2:
        e.kind, e.sims = "teacher", int(p[1])
    elif p[0] == "net" and len(p) in (2, 3):
        if not os.path.exists(p[1]):
            raise FileNotFoundError(f"{spec!r}：找不到权重 {p[1]}")
        e.kind, e.ts_path = "net", ensure_ts(p[1], device)   # ⚠️ 存档按**设备**各一份
        e.sims = int(p[2]) if len(p) == 3 else 0
    else:
        raise ValueError(f"看不懂的策略 {spec!r}（random / teacher:<sims> / net:<path>[:<sims>]）")
    return e


def main() -> int:
    ap = argparse.ArgumentParser(description="配对评估（C++ `sts.eval_run`）")
    ap.add_argument("n_per", type=int, help="每个 (遭遇, 层) 项跑几场")
    ap.add_argument("specs", help="逗号分隔：random / teacher:<sims> / net:<path>[:<sims>]")
    ap.add_argument("seed0", type=int, nargs="?", default=EVAL_SEED0, help="种子段起点")
    ap.add_argument("out", nargs="?", default=None, help="读数 json（不给 = 只打印）")
    ap.add_argument("--games", type=int, default=8, help="一次并肩几棵树（只改墙钟与批宽）")
    ap.add_argument("--device", default="cpu", help="网络策略的前向设备（cpu / cuda；存档按设备各一份）")
    ap.add_argument("--max-steps", type=int, default=MAX_STEPS, help="单场步数上限")
    a = ap.parse_args()

    if a.seed0 < EVAL_SEED0:
        sys.exit(f"❌ seed0={a.seed0} 在训练/采集段内（< {EVAL_SEED0}）—— 评估不能用见过的种子")
    specs = [parse_spec(s, a.device) for s in a.specs.split(",") if s]
    if not specs:
        sys.exit("❌ 至少要一个策略")
    if len({s.label for s in specs}) != len(specs):
        sys.exit(f"❌ 策略名重复：{[s.label for s in specs]}")

    t0 = time.perf_counter()
    stats = sts.eval_run(specs, a.n_per, a.seed0, a.games, a.max_steps)
    rows = [{"spec": s.label, "kind": s.kind, "sims": s.sims, "games": int(v.games),
             "wins": int(v.wins), "undecided": int(v.undecided), "win_rate": float(v.win_rate)}
            for s, v in zip(specs, stats)]
    print(f"配对评估：{a.n_per} 场/项 × 全表；seed0={a.seed0}；{time.perf_counter() - t0:.0f}s",
          flush=True)
    for r in rows:
        print(f"  {r['spec']:40s} {r['win_rate']:5.1%}（{r['wins']}/{r['games']}"
              f"，未决 {r['undecided']}）", flush=True)
    if a.out:
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        with open(a.out, "w", encoding="utf-8") as fh:
            json.dump({"n_per": a.n_per, "seed0": a.seed0, "games": a.games, "device": a.device,
                       "rows": rows}, fh, ensure_ascii=False, indent=2)
        print(f"读数 → {a.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
