"""漫游场景的**同分布胜率评测** —— 检验训练成果的第一站。

## 它测什么

「把策略放进与训练数据**相同的随机化模式**里打整局」：每场开打前
`reset_run_resources()` + 按层缩放的随机卡组/遗物/药水 + hp=u(35–95%)、金币随机
（与 `scene.shape_floor` 逐位同分布）；整局流程（走图/跳过非战斗房/结局）与本文件自带的
`run_one_collect` 同源，只把战斗换成**不记录**的评测版（`_eval_battle`）。

⚠️ **采集侧不走图**（按 `(怪组, 层)` 直接造一场，见 `collect_encs`）⇒ 漫游 harness 的唯一
消费者就是本文件，所以它定义在这里。

⇒ 这是「训练分布内的直输」：既不是自然进程下的整局（卡组从 10 张长起来、血量随战斗延续
—— 分布外），也不是离线 val 指标（只看单步预测）。**同 seed 的两个
策略逐位配对**（同一批遭遇、同一套随机资源；直到胜负分叉前完全同轨迹），比较公平。

## 用法

    python eval_walk.py <局数> <策略1[,策略2…]> [seed0=20000] [out.json]

策略 spec 与 `walk.parse_spec` 完全一致：`random` | `teacher:<sims>` | `net:<path>[:<sims>]`
（裸网络 = 省略 sims 或 0；>0 = PUCT(sims)，batch 走 `--mcts-batch`）。例：

    python eval_walk.py 200 "teacher:4000,net:data/walk_v3_n800.pt"
    python eval_walk.py 200 "net:data/walk_v3_n800.pt:100"

⚠️ **用逗号分隔策略，不要用分号**：经 `./sync.sh start` 传参时参数会穿过三层 shell
（ssh → tmux → `script -c`），**分号会被当命令分隔符吃掉**（2026-09-23 实测：
`"a;b"` 到远端只剩 `a`，且后半段被当命令乱跑）。分号仅在本地直跑时可用（也接）。
⚠️ 种子纪律：seed0 必须 ≥ `EVAL_SEED0`（held-out 区间才能评 —— 被训练见过
的种子一度都不能碰）。
⚠️ 每个策略各起一次进程池（worker 自己 `torch.load`），策略间**串行**跑同一批种子。

输出：场胜率 ±95%CI（朴素二项 SE，未计局内相关 ⇒ 偏窄，当粗略参考）＋**战损**
（1 − 胜局平均剩血比，历史口径）＋**终局得分**（引擎 `evaluateEndState` 的均值
= `zspec.Terminal.t_escore`）＋按幕/按房间（MONSTER/ELITE/BOSS）/按**怪群**
（`MonsterEncounter` 枚举名，如 `THREE_SENTRIES`）拆表，末尾给「怪群 × 策略」对照矩阵
（只列总场数 ≥60 的怪群，小样本不列、防过度解读）。

JSON：`per_act` / `per_room` / `per_encounter` = {键: [胜场, 总场]}（速览视图）＋
**`battle_rows`** = 逐场一行（字段 = `seed/idx/floor/act/room/enc` + `zspec.Z_COLS` 六列）
⇒ 胜率、战损、终局得分、按 (怪群, 层) 拆表、跨策略**逐场配对**全从它现算：
口径只有 `zspec` 一份，harness 不再另存一套汇总口径。

⚠️ **战损只统计胜局**（历史口径）⇒ 有选择效应：胜率更高时「边际胜局」是更难的场次，
会把战损**拉高**（不是打法变费血）。跨策略比战损必须胜率接近，否则用 `battle_rows`
取两策略**都赢**的交集局做配对。同理「终局得分」含败局的低分 ⇒ 高胜率策略自动更高。

⭐ 大 n 用法：裸网络一次 200 局只要 ~3 s ⇒ 想抹平「随机资源/初始状态」的取样噪声就直接
加局数（如 `1000~5000`），别改随机化本身（那部分方差是**要测的分布**的一部分）。

⚠️ **口径声明（2026-09-23 定调）**：模型**输入侧不含隐藏信息**（牌堆只给集合：
`aznet.card_feats` 置换不变池化，守卫 `_chk_pool.py`；
raw 快照保留的堆序/抽牌顺序**不喂模型**）。但 `teacher:` 与 `net:<path>:<sims>`（搜索）
都是**「开天眼」口径**——模拟器内的搜索看得到本局真实的未来抽牌顺序：同预算对照仍然有效，
但**不等于公平代理的强度**。只有**裸网络**（无搜索）是公平口径。引用数字必须带口径。
"""
from __future__ import annotations

import json
import os
import random
import re
import sys
import time
from collections import Counter

import argparse

import numpy as np
import slaythespire as sts
from tqdm import tqdm

import train as T
import runtime
import azaction
from scene import shape_floor
from collect import EVAL_SEED0, MAX_STEPS
from walk import (ASCENSION, BATTLE_ROOMS, CHARACTER, MAX_LOOP, _init_worker, make_choose,
                   parse_spec, step_map)
from zspec import DEFAULT_SPEC, Terminal, Z_COLS, z_from


# ---------------------------------------------------------------- 整局漫游 harness
#
# 每场开打前由 `battle_fn` 把状态重造，所以走图只负责「遭遇 + 幕/层 + 结局」。

NON_BATTLE_SCREENS = (sts.ScreenState.REWARDS, sts.ScreenState.BOSS_RELIC_REWARDS,
                      sts.ScreenState.EVENT_SCREEN, sts.ScreenState.SHOP_ROOM,
                      sts.ScreenState.REST_ROOM, sts.ScreenState.TREASURE_ROOM)


def _sub_seed(seed, battle_idx):
    """(整局 seed, 第几场) → 派生 seed。整局可复现的最小充分键。"""
    return (seed * 1_000_003 + battle_idx * 7919 + 13) & 0x7FFF_FFFF


def run_one_collect(seed, battle_fn):
    """整局漫游，每场交给 `battle_fn(gc, seed, battle_idx)`；返回 `(battles, stats)`。

    `battles` = `[(第几场, battle_fn 的返回值), ...]`，按发生顺序。

    非战斗房与战后奖励屏一律跳过：只调 `gc.regain_control()` 推进状态，不领任何奖励
    （每场开打前资源由 `battle_fn` 重造）。
    不能改成直接置 `MAP_SCREEN`：`regainControlAction` 带着推进必需的转换（回地图、进 Boss
    宝箱、换幕），省掉它整局走不下去。
    """
    rng = random.Random(seed)
    gc = sts.GameContext(CHARACTER, seed, ASCENSION)
    gc.choose_event_option(0)                    # 涅奥：取第 0 个（与 `walk.run_one` 一致）

    battles, wins, loops = [], 0, 0
    rooms: dict[str, int] = {}
    while gc.outcome == sts.GameOutcome.UNDECIDED and loops < MAX_LOOP:
        loops += 1
        s = gc.screen_state
        if s == sts.ScreenState.MAP_SCREEN:
            room = step_map(gc, rng)
            if room is None:
                break
            rooms[room.name] = rooms.get(room.name, 0) + 1
            if room in BATTLE_ROOMS:
                item = battle_fn(gc, seed, len(battles))
                battles.append((len(battles), item))
                wins += int(item[1] > 0)
        elif s in NON_BATTLE_SCREENS:
            gc.regain_control()
        elif s == sts.ScreenState.CARD_SELECT:
            gc.choose_select_card_screen_option(0)
        elif s == sts.ScreenState.BATTLE:
            # 意外形态：screen=BATTLE 但房间不在 BATTLE_ROOMS —— 照打别丢局（同 `walk`）。
            item = battle_fn(gc, seed, len(battles))
            battles.append((len(battles), item))
            wins += int(item[1] > 0)
        else:
            raise RuntimeError(f"seed {seed}: 未处理的屏幕 {s!s}")

    stats = {"seed": seed, "outcome": gc.outcome.name, "act": gc.act, "floor": gc.floor_num,
             "battles": len(battles), "wins": wins, "rooms": rooms}
    return battles, stats


# ---------------------------------------------------------------- 单场战斗（评测版）

def _eval_battle(gc, seed, battle_idx):
    """与采集同场景同随机化，但策略来自 `walk` 的 spec、且**不记录**轨迹
    （不复制快照 / 不存教师 π）—— 返回 `(seed, z, (act, won, enc, room, floor), term)`：
    `z` = 历史 `hp` 口径（赢=剩血比、负=−1），供 `run_one_collect` 数胜负；
    `term` = `zspec.Terminal` 终局六列 = 战损/终局得分的唯一来源。
    """
    choose = make_choose()
    sub = _sub_seed(seed, battle_idx)
    act, floor = gc.act, gc.floor_num     # 本场在哪一幕/哪一层（怪群 × 层拆表用）
    enc = getattr(gc.encounter, "name", str(gc.encounter))   # 怪群标签（MonsterEncounter）
    room = getattr(gc.cur_room, "name", str(gc.cur_room))
    shape_floor(gc, sub)                 # 状态生成只有一份实现（与采集共用）

    b = sts.BattleContext()
    b.init(gc)
    rng = random.Random(sub * 104729 + 31)   # 策略侧随机（只有 random: 用得上）
    steps = 0
    while not b.is_battle_over() and steps < MAX_STEPS:
        a = choose(b, rng)
        if a is None:
            if b.input_state == sts.InputState.CARD_SELECT:
                b.select_card(0)
            else:
                b.end_turn()
            steps += 1
            continue
        a.execute(b)
        steps += 1
    term = azaction.terminal_state(b)        # 终局读数（唯一构造点）必须 `exit_battle` 之前取
    b.exit_battle(gc)                        # 引擎正常出口（换幕/通关/败北全自走；奖励屏被外层跳过）
    return (seed, z_from(term, "hp"), (act, term.t_win > 0, enc, room, floor), term)


def _pool_job(seed):
    return (seed,) + run_one_collect(seed, _eval_battle)


# ---------------------------------------------------------------- 一个策略的评测

def _term_of(row: dict) -> Terminal:
    """`battle_rows` 的一行 → `Terminal`（列名唯一来源 = `zspec.Z_COLS`）。"""
    return Terminal(**{c: row[c] for c in Z_COLS})


def _label(spec):
    if spec[0] == "random":
        return "随机"
    if spec[0] == "teacher":
        return f"教师{spec[1]}"
    return os.path.basename(spec[1]) + (f" MCTS({spec[2]})" if spec[2] else " 裸网络")


def _postfix(pg, results):
    nb = sum(len(bl) for _, bl, _ in results)
    wins = sum(1 for _, bl, _ in results for _i, it in bl if it[1] > 0)
    pg.set_postfix_str(f"场胜率 {wins / max(1, nb):.1%}  {nb / max(1, len(results)):.1f} 场/局",
                       refresh=False)


def _evaluate(spec_s, n, seed0, verbose, workers=None, mp_start=None, batch=1, threads=1):
    spec = parse_spec(spec_s)
    label = _label(spec)
    cfg = (spec, batch, threads)          # `_init_worker` 的实参（每个进程各自建网）
    nw = runtime.workers(workers)
    use_pool = bool(nw) and nw > 1
    print(f"\n== 评测 {label} ==  {n} 局  seed {seed0}..{seed0 + n - 1}  "
          f"workers={nw if use_pool else 'serial'}", flush=True)
    t0 = time.perf_counter()
    results = []
    if use_pool:
        with runtime.mp_context(mp_start).Pool(nw, initializer=_init_worker,
                                            initargs=(cfg,)) as pool, \
                tqdm(total=n, desc=label, unit="局", dynamic_ncols=True, mininterval=0.4) as pg:
            for seed, battles, stats in pool.imap_unordered(_pool_job, range(seed0, seed0 + n),
                                                            chunksize=1):
                results.append((seed, battles, stats))
                _postfix(pg, results)
                if verbose:
                    print(f"  [{seed}] {stats['outcome']} Act{stats['act']} "
                          f"层{stats['floor']} 战斗={stats['battles']}(胜{stats['wins']})")
                pg.update()
    else:
        _init_worker(cfg)
        with tqdm(total=n, desc=label, unit="局", dynamic_ncols=True, mininterval=0.4) as pg:
            for seed in range(seed0, seed0 + n):
                results.append(_pool_job(seed))
                _postfix(pg, results)
                pg.update()
    dt = time.perf_counter() - t0

    nb = sum(len(bl) for _, bl, _ in results)
    wins = sum(1 for _, bl, _ in results for _i, it in bl if it[1] > 0)
    acts, act_wins, zs = Counter(), Counter(), []
    encs, rooms = Counter(), Counter()          # 怪群 / 房间：总数
    enc_w, room_w = Counter(), Counter()        # 怪群 / 房间：胜场
    floors, rows = [], []                       # `rows` = 逐场终局行（见模块头「JSON」）
    for seed, bl, stats in results:
        floors.append(stats["floor"])
        for i, (_sd, z, info, term) in bl:
            zs.append(z)
            a, won, enc, room, floor = info
            rows.append({"seed": seed, "idx": i, "floor": floor, "act": a,
                         "room": room, "enc": enc, **term._asdict()})
            acts[a] += 1
            act_wins[a] += int(won)
            encs[enc] += 1
            enc_w[enc] += int(won)
            rooms[room] += 1
            room_w[room] += int(won)

    # 战损 / 终局得分：只从 `rows` 现算，harness 不另存汇总口径
    wrows = [r for r in rows if r["t_win"] > 0]
    hp_left = float(np.mean([r["t_hp"] / max(1, r["t_max_hp"]) for r in wrows])) \
        if wrows else float("nan")
    escore = float(np.mean([r["t_escore"] for r in rows])) if rows else float("nan")
    zeng = float(np.mean([z_from(_term_of(r), DEFAULT_SPEC) for r in rows])) \
        if rows else float("nan")
    turns = float(np.mean([r["t_turn"] for r in rows])) if rows else float("nan")
    outcomes = Counter(stats["outcome"] for _, _, stats in results)
    p = wins / max(1, nb)
    se = (p * (1 - p) / max(1, nb)) ** 0.5     # 朴素二项 SE（未计局内相关，偏窄）
    d = {"label": label, "spec": spec_s, "runs": n, "battles": nb, "wins": wins,
         "win_rate": p, "se": se, "battles_per_run": nb / max(1, n),
         "per_act": {str(a): [act_wins[a], acts[a]] for a in sorted(acts)},
         "per_room": {k: [room_w[k], rooms[k]] for k in sorted(rooms)},
         "per_encounter": {k: [enc_w[k], encs[k]] for k in sorted(encs)},
         "outcomes": dict(outcomes), "mean_floor": float(np.mean(floors)),
         "z_mean": float(np.mean(zs)) if zs else 0.0,
         "z_var": float(np.var(zs)) if zs else 0.0, "seconds": round(dt, 1),
         "hp_left": hp_left, "hp_loss": 1.0 - hp_left, "escore": escore,
         "zeng": zeng, "turns": turns, "battle_rows": rows}
    act_s = "  ".join(f"act{a} {act_wins[a] / acts[a]:.1%}({act_wins[a]}/{acts[a]})"
                      for a in sorted(acts))
    room_s = "  ".join(f"{k} {room_w[k] / rooms[k]:.1%}({rooms[k]})" for k in sorted(rooms))
    print(f"  {label}: 场胜率 {wins}/{nb} = {p:.1%} ±{1.96 * se * 100:.1f}pp | {act_s}", flush=True)
    print(f"    房间：{room_s}", flush=True)
    print(f"    局结局 {dict(outcomes)}；平均 {nb / max(1, n):.1f} 场/局，"
          f"终局层 {d['mean_floor']:.1f}；z 均值 {d['z_mean']:+.3f}；{d['seconds']}s", flush=True)
    print(f"    战损 {d['hp_loss']:.1%}（{len(wrows)} 胜局，剩血 {d['hp_left']:.3f}）；"
          f"终局得分 {d['escore']:+.2f}（z 口径 {d['zeng']:+.3f}）；均回合 {d['turns']:.1f}",
          flush=True)
    # 怪群明细（只列 ≥10 场的；按场数降序）——噪声大的小样本不列，防止过度解读
    big = [(k, encs[k]) for k in sorted(encs, key=lambda k: -encs[k]) if encs[k] >= 10]
    if big:
        cells = " | ".join(f"{k} {enc_w[k] / encs[k]:.0%}({encs[k]})" for k, _ in big)
        print(f"    怪群（≥10 场）：{cells}", flush=True)
    return d


# ---------------------------------------------------------------- 主流程

def main():
    p = argparse.ArgumentParser(description="漫游评测（同 seed 配对：n 局 × 多策略）",
                                epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("n", type=int, help="局数")
    p.add_argument("specs", help="策略，逗号(或分号)分隔：random | teacher:N | net:path[:sims]")
    p.add_argument("seed0", type=int, nargs="?", default=None, help="种子起点（默认 20000）")
    p.add_argument("out", nargs="?", default=None, help="结果 json")
    p.add_argument("--workers", default=None,
                   help="并行 worker 数；'serial'/0 关闭并行；不给 = 按 cgroup 配额")
    p.add_argument("--mp-start", default=None, help="multiprocessing 启动方式（fork/spawn）")
    p.add_argument("--mcts-batch", type=int, default=1,
                   help="MCTS 叶批量（1 = 与历史等价；影响数值可比性）")
    p.add_argument("--rollout-threads", type=int, default=1, help="每个 worker 的 torch 线程数")
    a = p.parse_args()
    n = a.n
    seed0 = 20000 if a.seed0 is None else a.seed0
    out = a.out
    specs = [s for s in re.split(r"[,;]", a.specs) if s]
    if seed0 < EVAL_SEED0:
        sys.exit(f"评测种子必须 ≥ EVAL_SEED0={EVAL_SEED0}（held-out 区间才能评）——"
                 f"收到 seed0={seed0}；训练区间是 0..{T.EVAL_SEED0 - 1}，被见过就不能当评估"
                 f"（边界可用 `--eval-seed0` 改）。")
    print(f"漫游评测（同分布随机模式）：{n} 局 × {len(specs)} 策略  seed0={seed0}", flush=True)
    all_d = [_evaluate(s, n, seed0, verbose=n <= 5, workers=a.workers, mp_start=a.mp_start,
                       batch=a.mcts_batch, threads=a.rollout_threads) for s in specs]

    print("\n== 汇总（同 seed 配对）==", flush=True)
    print(f"  {'策略':<30}{'场胜率':>9}{'±':>6}{'act1':>13}{'act2':>13}{'act3':>11}"
          f"{'场/局':>7}{'终局层':>7}{'z均值':>8}{'战损':>7}{'终局分':>8}{'回合':>6}")
    for d in all_d:
        pa = d["per_act"]

        def _cell(a):
            w, t = pa.get(str(a), [0, 0])
            return f"{w / t:.1%}({w}/{t})" if t else "—"
        print(f"  {d['label']:<30}{d['win_rate']:>9.1%}{1.96 * d['se'] * 100:>5.1f}pp{_cell(1):>13}"
              f"{_cell(2):>13}{_cell(3):>11}"
              f"{d['battles_per_run']:>7.1f}{d['mean_floor']:>7.1f}{d['z_mean']:>+8.3f}"
              f"{d['hp_loss']:>7.1%}{d['escore']:>+8.2f}{d['turns']:>6.1f}")

    # ---- 怪群对照矩阵：只取总场数 ≥60 的怪群（大 n 才值得看），行=怪群，列=策略 ----
    tot = Counter()
    for d in all_d:
        for k, (_w, t) in d["per_encounter"].items():
            tot[k] += t
    common = [k for k, _ in tot.most_common() if tot[k] >= 60]
    if common and len(all_d) > 1:
        head = "  " + f"{'怪群(场)':<26}" + "".join(f"{d['label'][:14]:>15}" for d in all_d)
        print("\n== 怪群对照（总场数 ≥60；格=胜率(场数)）==", flush=True)
        print(head, flush=True)
        for k in common:
            row = f"  {f'{k} ({tot[k]})':<26}"
            for d in all_d:
                w, t = d["per_encounter"].get(k, [0, 0])
                row += f"{(f'{w / t:.0%}({t})' if t else '—'):>15}"
            print(row, flush=True)
    if out:
        with open(out, "w", encoding="utf-8") as f:
            json.dump(all_d, f, ensure_ascii=False, indent=1)
        print(f"\n已写出 {out}", flush=True)


def _int(arg, what):
    try:
        return int(arg)
    except ValueError:
        sys.exit(f"{what} 必须是整数，收到 {arg!r}")


if __name__ == "__main__":
    main()
