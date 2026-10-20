"""门控触发器：配对评估多个策略，报胜率（评估全在 C++ 的 `sts.eval_run`）。

    python eval_gate.py <每组场数> <策略,策略,…> [seed0] [out.json] [--games N]

策略：`random` | `teacher:<sims>` | `net:<path>[:<sims>]`（权重自动导出成 ONNX；
**不给 `:sims` = 裸网络**（逐决策点取网络先验 argmax，无搜索遮挡），给 `:sims`(≥2) 才走它自己的 MCTS）。
任务表 / 造局面 / 搜索 / 统计都在 C++，同一批 `(遭遇, 层, seed)` 喂给所有策略 ⇒ 胜率差 = 策略差。
"""
import argparse
import json
import os
import sys
import time

import torch  # noqa: F401  ⚠️ 训练/导出侧要它（C++ 侧只链 ONNX Runtime，不依赖 torch）
import slaythespire as sts

import ckpt
from collect import EVAL_SEED0, MAX_STEPS
from netarchive import ensure_net
from runtime import workers as runtime_workers
from zspec import DEFAULT_SPEC


def parse_spec(spec: str, device: str = "cpu", workers: int = 0) -> sts.EvalSpec:
    """`random` | `teacher:<sims>` | `net:<path>[:<sims>]` → `sts.EvalSpec`（看不懂就报错）。"""
    p = spec.split(":")
    e = sts.EvalSpec()
    e.label, e.device, e.z_spec = spec, device, DEFAULT_SPEC
    e.workers = int(workers)
    if p[0] == "random" and len(p) == 1:
        e.kind = "random"
    elif p[0] == "simple" and len(p) == 1:
        e.kind = "simple"
    elif p[0] == "teacher" and len(p) == 2:
        e.kind, e.sims = "teacher", int(p[1])
    elif p[0] == "net" and len(p) in (2, 3):
        if not os.path.exists(p[1]):
            raise FileNotFoundError(f"{spec!r}：找不到权重 {p[1]}")
        e.kind, e.model_path = "net", ensure_net(p[1], device)   # ⚠️ 存档按**设备**各一份
        e.sims = int(p[2]) if len(p) == 3 else 0
        # 裸网（sims=0）是单 Session lockstep；拆多 Session 会把 batch 拆碎，反而更慢。
        if e.sims < 2:
            e.workers = 0
        # MCTS 尾段：worker 就地前向（空存档 = 用主存档 + CPU）；裸网（sims=0）不走这条路。
        if e.sims >= 2 and str(device).lower() not in ("", "cpu"):
            e.model_path_cpu = ensure_net(p[1], "cpu")
    else:
        raise ValueError(f"看不懂的策略 {spec!r}（random / teacher:<sims> / net:<path>[:<sims>]）")
    return e


def main() -> int:
    ap = argparse.ArgumentParser(description="配对评估（C++ `sts.eval_run`）")
    ap.add_argument("n_per", type=int, help="每个 (遭遇, 层) 项跑几场")
    ap.add_argument("specs", help="逗号分隔：random / simple / teacher:<sims> / net:<path>[:<sims>]")
    ap.add_argument("seed0", type=int, nargs="?", default=EVAL_SEED0, help="种子段起点")
    ap.add_argument("out", nargs="?", default=None, help="读数 json（不给 = 只打印）")
    ap.add_argument("--games", type=int, default=512,
                    help="MCTS 池宽 / 裸网 batch 宽度。裸网千万别用默认 8：会退化成 4032 个小 batch")
    ap.add_argument("--device", default="cpu", help="网络策略的前向设备（cpu / cuda；存档按设备各一份）")
    ap.add_argument("--workers", type=int, default=runtime_workers() or 0,
                    help="MCTS worker 数；0 = 硬件并发。裸网固定单 Session（多 Session 实测更慢）")
    ap.add_argument("--max-steps", type=int, default=MAX_STEPS, help="单场步数上限")
    ap.add_argument("--hard-only", action="store_true",
                    help="只统计难关（强怪/精英/Boss/Act4/事件），跳过三幕弱怪")
    a = ap.parse_args()

    if a.seed0 < EVAL_SEED0:
        sys.exit(f"❌ seed0={a.seed0} 在训练/采集段内（< {EVAL_SEED0}）—— 评估不能用见过的种子")
    specs = [parse_spec(s, a.device, a.workers) for s in a.specs.split(",") if s]
    if not specs:
        sys.exit("❌ 至少要一个策略")
    if len({s.label for s in specs}) != len(specs):
        sys.exit(f"❌ 策略名重复：{[s.label for s in specs]}")

    t0 = time.perf_counter()
    stats = sts.eval_run(specs, a.n_per, a.seed0, a.games, a.max_steps, a.hard_only)
    rows = [{"spec": s.label, "kind": s.kind, "sims": s.sims, "games": int(v.games),
             "wins": int(v.wins), "undecided": int(v.undecided), "win_rate": float(v.win_rate),
             "hp_ratio": float(v.hp_ratio), "avg_hp": float(v.avg_hp),
             "damage_ratio": float(v.damage_ratio), "avg_damage": float(v.avg_damage),
             "win_damage_ratio": float(v.win_damage_ratio),
             "avg_win_hp_ratio": float(v.avg_win_hp_ratio), "avg_turns": float(v.avg_turns),
             "per_enc": [{"name": e.name, "hard": bool(e.hard), "games": int(e.games),
                          "wins": int(e.wins), "undecided": int(e.undecided),
                          "win_rate": float(e.win_rate), "hp_ratio": float(e.hp_ratio),
                          "damage_ratio": float(e.damage_ratio),
                          "avg_damage": float(e.avg_damage),
                          "win_damage_ratio": float(e.win_damage_ratio),
                          "avg_win_hp_ratio": float(e.avg_win_hp_ratio),
                          "avg_turns": float(e.avg_turns)}
                         for e in v.per_enc if e.games > 0]}
            for s, v in zip(specs, stats)]
    print(f"配对评估：{a.n_per} 场/项 × 全表；seed0={a.seed0}；{time.perf_counter() - t0:.0f}s",
          flush=True)
    for r in rows:
        print(f"  {r['spec']:40s} {r['win_rate']:5.1%}（{r['wins']}/{r['games']}"
              f"，未决 {r['undecided']}） HP {r['hp_ratio']:5.1%}"
              f" 战损 {r['damage_ratio']:5.1%}（胜场战损 {r['win_damage_ratio']:5.1%}）", flush=True)
    if a.out:
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        with open(a.out, "w", encoding="utf-8") as fh:
            json.dump({"n_per": a.n_per, "seed0": a.seed0, "games": a.games, "device": a.device,
                       "rows": rows}, fh, ensure_ascii=False, indent=2)
        print(f"读数 → {a.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
