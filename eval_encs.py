"""按怪组 × 出现层分别评测：每项固定场数，各策略同 seed 配对。

    python eval_encs.py <每组场数> <策略,策略,...> [seed0] [out.json]

构造照 `collect_encs.py`：一张 `(遭遇, 层)` 表（直接复用它的 `ENCS`，测试覆盖 == 采集覆盖）
+ 逐项一个子进程（隔离边界，也是「每项固定场数」的来源）+ 分片可复用 + 逐项战报；
差别只在**不产数据、只产读数**。

为什么需要它：`eval_walk.py` 按整局随机漫游统计，怪群那行格子的分母由走图运气决定 ——
act3 的怪要等整局活到 act3 才有样本，且各怪群拿到的牌组/资源分布互不相同，格与格不可比。
本脚本给每个怪组**单独造场**：层固定（`shape_floor` 按层发该档的牌组/遗物/hp/药水），
所有策略用**同一段 seed**（`generate_single_enc_jobs`）⇒ 同 seed 配对，格与格直接可比。

策略语法同 `walk.parse_spec`：`random` | `teacher:<sims>` | `net:<path>[:<sims>]`。

seed 纪律：`seed0` 默认 `EVAL_SEED0`（1e8），低于它直接退出 —— 训练见过的种子不能当评估。
"""
import argparse
import hashlib
import json
import math
import os
import random
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Sequence

import slaythespire as sts
from tqdm import tqdm

import azaction
from collect import EVAL_SEED0, MAX_STEPS, make_stage_battle
from collect_encs import ENCS, generate_single_enc_jobs
import runtime
from walk import _init_worker, make_choose, parse_spec
from runtime import short_name
from zspec import Z_COLS

KIND = "evalencs"
# 每场一行的列：seed + 终局六列。列名唯一来源 = `zspec.Z_COLS`（不另抄一份字段名）。
ROW_COLS = ("seed",) + tuple(Z_COLS)
I_WIN, I_HP, I_MAXHP, I_TURN, I_ENEMY, I_ESCORE = (
    ROW_COLS.index(c) for c in ("t_win", "t_hp", "t_max_hp", "t_turn", "t_enemy", "t_escore"))


# ---------------------------------------------------------------- 一场

def eval_battle(seed: int, enc: sts.MonsterEncounter, floor: int) -> tuple[list, bool]:
    """按 `(seed, 遭遇, 层)` 造一场并打完，返回 `(行, 是否撞上限)`。

    造环境走 `collect.make_stage_battle`（与采集**同一份实现** ⇒ 评的场就是训的场），
    策略走 `walk.make_choose()`（与 `eval_walk` 同一份 spec 装配）。不记录轨迹：评测只要终局读数。
    撞 `MAX_STEPS` 时终局列读的是当时状态、不是真终局，调用方需要单独数它。
    """
    _gc, b, _enc = make_stage_battle(seed, enc, floor)
    choose = make_choose()
    rng = random.Random(seed * 104729 + 31)      # 策略侧随机（只有 `random:` 用得上）
    steps = 0
    while not b.is_battle_over() and steps < MAX_STEPS:
        a = choose(b, rng)
        if a is None:                            # 无合法动作：选牌态取第 0 张，否则结束回合
            if b.input_state == sts.InputState.CARD_SELECT:
                b.select_card(0)
            else:
                b.end_turn()
        else:
            a.execute(b)
        steps += 1
    term = azaction.terminal_state(b)            # 终局读数只有一个构造点
    row = [seed] + [getattr(term, c) for c in Z_COLS]
    return row, not b.is_battle_over() and steps >= MAX_STEPS


# ---------------------------------------------------------------- 一项（子进程）

def evaluate_item(enc: sts.MonsterEncounter, floor: int, n_per: int, spec_strs: Sequence[str],
                  seed0: int, batch: int = 1, threads: int = 1) -> dict[str, Any]:
    """在本项上把每个策略跑一遍（各项共用同一段 seed），返回可落盘的分片 dict。"""
    jobs = generate_single_enc_jobs(enc, floor, n_per, seed0)
    per_spec: dict[str, Any] = {}
    for ss in spec_strs:
        _init_worker((parse_spec(ss), batch, threads))
        rows, stalls = [], 0
        for seed, _e, _f in jobs:
            row, stall = eval_battle(seed, enc, floor)
            rows.append(row)
            stalls += int(stall)
        per_spec[ss] = {"n": len(rows), "stalls": stalls, "rows": rows}
    return {"enc": enc.name, "enc_id": int(enc), "floor": int(floor), "n_per": int(n_per),
            "seed0": int(seed0), "batch": int(batch), "specs": list(spec_strs),
            "per_spec": per_spec}


def _item_line(shard: dict[str, Any]) -> str:
    """一项一行的战报（父进程逐项打印）。"""
    cells = []
    for ss in shard["specs"]:
        m = summarize(shard["per_spec"][ss]["rows"])
        st = shard["per_spec"][ss]["stalls"]
        loss = f"{m['hp_loss']:>5.1%}" if m["wins"] else "    —"
        cells.append(f"{m['win_rate']:>4.0%}({m['wins']}/{m['n']}) 损{loss}"
                     + (f" 撞限{st}" if st else ""))
    return f"{shard['enc']}@{shard['floor']:<3d} " + "  ".join(cells)


def _launch_item(enc: sts.MonsterEncounter, floor: int, n_per: int, spec_strs: Sequence[str],
                 seed0: int, batch: int, part: str) -> tuple[str, int, str]:
    """③ 拉起一项的子进程，只回小结果（不传读数）。

    返回 `(分片路径, 退出码, 子进程输出原文)`；判读交给 `_report`（完成一项就判一项）。
    `enc` 是枚举，argv 里只能传它的 `int`（子进程再转回枚举）。
    """
    cmd = [sys.executable, "-u", os.path.abspath(__file__), str(n_per), ",".join(spec_strs),
           str(seed0), "--one", str(int(enc)), "--floor", str(floor),
           "--mcts-batch", str(batch), "--part", part]
    p = subprocess.run(cmd, cwd=os.path.dirname(os.path.abspath(__file__)),
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    raw = ((p.stderr or "") + (p.stdout or "")).splitlines()
    text = "\n".join(ln for ln in raw if ln.strip() and "%|" not in ln and "场/s]" not in ln)
    return part, p.returncode, text


# ---------------------------------------------------------------- 读数

def summarize(rows: Sequence[Sequence[float]]) -> dict[str, float]:
    """一组场 → 胜率 / 战损(仅胜场) / 敌方留存 / 回合 / 终局分。终局列口径见 `zspec`。

    战损**只统计胜场**：败场的 `t_hp = 0` 必然算成 100% 损失，把它混进来会让这一列变成胜率的
    复述（而且一列两个含义）。要「输得多惨」看胜率那一列就够了。
    """
    n = len(rows)
    if not n:
        return {"n": 0, "wins": 0, "win_rate": float("nan"), "hp_loss": float("nan"),
                "enemy_left": float("nan"), "turns": float("nan"), "escore": float("nan")}
    mean = lambda vs: sum(vs) / len(vs)                                       # noqa: E731
    won = [r for r in rows if r[I_WIN] > 0]
    hp_w = [r[I_HP] / max(1, r[I_MAXHP]) for r in won]
    return {"n": n, "wins": len(won),
            "win_rate": len(won) / n,
            "hp_loss": 1.0 - mean(hp_w) if hp_w else float("nan"),
            "enemy_left": mean([r[I_ENEMY] for r in won]) if won else float("nan"),
            "turns": mean([r[I_TURN] for r in rows]),
            "escore": mean([r[I_ESCORE] for r in rows])}


def paired(rows_a: Sequence[Sequence[float]], rows_b: Sequence[Sequence[float]]
           ) -> tuple[int, int, int, int, float, float, float]:
    """同 seed 配对：返回 `(共同场, A赢B输, B赢A输, 都赢, Δ剩血, Δ标准误, McNemar p)`。

    Δ剩血只在「两策略都赢」的场上算 —— 输赢不同的场拿来比剩血会把胜负差重复计入。
    """
    a = {r[0]: r for r in rows_a}
    b = {r[0]: r for r in rows_b}
    both = sorted(set(a) & set(b))
    oa = ob = 0
    dh = []
    for k in both:
        aw, bw = a[k][I_WIN] > 0, b[k][I_WIN] > 0
        if aw and bw:
            dh.append(a[k][I_HP] / max(1, a[k][I_MAXHP]) - b[k][I_HP] / max(1, b[k][I_MAXHP]))
        elif aw:
            oa += 1
        elif bw:
            ob += 1
    m = len(dh)
    mean = sum(dh) / m if m else float("nan")
    se = (sum((x - mean) ** 2 for x in dh) / (m * (m - 1))) ** 0.5 if m > 1 else float("nan")
    n = oa + ob
    p = 1.0
    if n:                                        # 双侧精确二项（McNemar 的精确版）
        hi = max(oa, ob)
        p = sum(math.comb(n, i) for i in range(n + 1)
                if abs(i - n / 2) >= abs(hi - n / 2)) / 2 ** n
    return len(both), oa, ob, m, mean, se, p


def _wid(s: str) -> int:
    return sum(2 if ord(c) > 0x2E80 else 1 for c in s)


def _pad(s: str, n: int) -> str:
    return s + " " * max(0, n - _wid(s))


def _lead(s: str, n: int) -> str:
    """首列：至少留一个空格（怪组名可能比列宽长，`_pad` 不截断也不补救）。"""
    return s + " " * max(1, n - _wid(s))


def spec_label(ss: str) -> str:
    """显示名：路径只留文件名（与 `eval_walk._label` 同口径）。权重名带全部超参，
    显示时用 `runtime.short_name` 截到轮数段为止 —— 否则一张表的列宽会被撞爆。"""
    spec = parse_spec(ss)
    if spec[0] == "random":
        return "随机"
    if spec[0] == "teacher":
        return f"教师{spec[1]}"
    return short_name(spec[1]) + (f" MCTS({spec[2]})" if spec[2] else " 裸网络")


# ---------------------------------------------------------------- 分片

def parts_dir_of(out: str) -> str:
    d = os.path.splitext(out)[0] + "_parts"
    os.makedirs(d, exist_ok=True)
    return d


def default_out(n_per: int, spec_strs: Sequence[str], seed0: int) -> str:
    """默认结果名：参数指纹进文件名 ⇒ 换策略/换场数不会复用上一次的分片。"""
    h = hashlib.sha1("|".join([str(n_per), str(seed0)] + list(spec_strs))
                     .encode("utf-8")).hexdigest()[:8]
    return runtime.cache_path(f"{KIND}_{n_per}_{h}.json")


def load_shard(part: str, n_per: int, spec_strs: Sequence[str], seed0: int,
               batch: int) -> dict[str, Any] | None:
    """分片已存在则校验参数一致后返回，否则 `None`。

    参数不一致必须报错而不是复用：分片里的读数是**按当时那套策略/搜索口径**算的，
    换策略或换 `--mcts-batch` 续跑会得到半新半旧的混合结果，而控制台上看不出来
    （`mcts_batch` 尤其阴：它不改任何形状，只改搜索展开的顺序）。
    """
    if not os.path.exists(part):
        return None
    with open(part, encoding="utf-8") as f:
        d = json.load(f)
    mine = (int(n_per), int(seed0), int(batch), list(spec_strs))
    key = (int(d.get("n_per", -1)), int(d.get("seed0", -1)), int(d.get("batch", -1)),
           list(d.get("specs", [])))
    if key != mine:
        raise RuntimeError(f"{part} 的参数与本次不符：分片 {key} ≠ 本次 {mine}；换参数请用 --force")
    return d


# ---------------------------------------------------------------- 回收与汇总

def _report(r: tuple[str, int, str], shards: list[dict], failed: list[str],
            n_per: int, spec_strs: Sequence[str], seed0: int, batch: int) -> None:
    """完成一项就判一次：成功打一项战报，失败把子进程输出原文全打出来（现场只此一份）。"""
    part, rc, text = r
    if rc != 0:
        failed.append(part)
        tqdm.write(f"  {part} 失败（退出码 {rc}）：\n"
                   f"{text or '（子进程无任何输出 —— 原生终止？）'}")
        return
    d = load_shard(part, n_per, spec_strs, seed0, batch)
    shards.append(d)
    tqdm.write("  " + _item_line(d))


def _matrices(shards: Sequence[dict], spec_strs: Sequence[str]) -> None:
    """逐怪组矩阵：胜率与战损各一张（行 = 怪组@层，列 = 策略）。"""
    labels = [spec_label(s) for s in spec_strs]
    # 列宽跟着策略名走：写死会在长名字（如 `g1_spencs_n1000_s500_g0_e16_soft1.pt MCTS(500)`）上错位。
    w = max(14, max(_wid(l) for l in labels) + 2)
    head = "  " + _lead("怪组@层", 32) + "".join(_lead(l, w) for l in labels)
    for title, fmt in (("胜率", lambda m: f"{m['win_rate']:.0%}({m['wins']}/{m['n']})"),
                       ("战损（仅胜场）", lambda m: f"{m['hp_loss']:.1%}" if m["wins"] else "—")):
        print(f"\n== 逐怪组{title} ==", flush=True)
        print(head, flush=True)
        for d in sorted(shards, key=lambda x: (x["floor"], x["enc_id"])):
            row = "  " + _lead(f"{d['enc']}@{d['floor']}", 32)
            for ss in spec_strs:
                row += _pad(fmt(summarize(d["per_spec"][ss]["rows"])), w)
            print(row, flush=True)
        tot = "  " + _lead("合计", 32)
        for ss in spec_strs:
            rows = [r for d in shards for r in d["per_spec"][ss]["rows"]]
            tot += _pad(fmt(summarize(rows)), w)
        print(tot, flush=True)


def _report_paired(shards: Sequence[dict], spec_strs: Sequence[str]) -> None:
    """每条臂与基准臂（第 1 条）配对；基准自己不出现在表里。"""
    base = spec_strs[0]
    print(f"\n== 配对（基准 = {spec_label(base)}；同 (怪组, 层, seed)）==", flush=True)
    print("  " + _lead("对照", 26) + _pad("共同场", 9) + _pad("A赢B输", 9) + _pad("B赢A输", 9)
          + _pad("McNemar p", 12) + _pad("都赢", 8) + "Δ剩血(A−B)", flush=True)
    for ss in spec_strs[1:]:
        n = oa = ob = m = 0
        dh = []
        for d in shards:
            r = paired(d["per_spec"][base]["rows"], d["per_spec"][ss]["rows"])
            n += r[0]
            oa += r[1]
            ob += r[2]
            m += r[3]
            a = {x[0]: x for x in d["per_spec"][base]["rows"]}
            b = {x[0]: x for x in d["per_spec"][ss]["rows"]}
            dh += [a[k][I_HP] / max(1, a[k][I_MAXHP]) - b[k][I_HP] / max(1, b[k][I_MAXHP])
                   for k in set(a) & set(b) if a[k][I_WIN] > 0 and b[k][I_WIN] > 0]
        mean = sum(dh) / m if m else float("nan")
        se = (sum((x - mean) ** 2 for x in dh) / (m * (m - 1))) ** 0.5 if m > 1 else float("nan")
        nn = oa + ob
        p = 1.0
        if nn:
            hi = max(oa, ob)
            p = sum(math.comb(nn, i) for i in range(nn + 1)
                    if abs(i - nn / 2) >= abs(hi - nn / 2)) / 2 ** nn
        print("  " + _lead(spec_label(ss), 26) + _pad(f"{n}", 9) + _pad(f"{oa}", 9)
              + _pad(f"{ob}", 9) + _pad(f"{p:.3f}", 12) + _pad(f"{m}", 8)
              + f"{mean * 100:+.2f}±{1.96 * se * 100:.2f}pp", flush=True)


def evaluate_encs(n_per: int, spec_strs: Sequence[str], seed0: int = EVAL_SEED0,
                  out: str | None = None, force: bool = False, batch: int = 1,
                  workers: str | int | None = None) -> list[dict]:
    """整张表跑完，返回各项分片（同时落盘）。"""
    for ss in spec_strs:
        parse_spec(ss)                            # 参数错误在起子进程之前就报
    # 绝对化：子进程的 cwd 是仓库目录，父进程可能是别处；相对路径会让两边指向不同文件。
    out = os.path.abspath(out or default_out(n_per, spec_strs, seed0))
    parts_dir = parts_dir_of(out)
    pairs = [(e, int(f)) for e, f in ENCS]
    t0 = time.perf_counter()
    print(f"按怪组×层评测：{len(pairs)} 项 × 每项 {n_per} 场 × {len(spec_strs)} 策略  "
          f"seed0={seed0}（每项一个子进程；分片 {parts_dir}）", flush=True)
    print("  策略：" + " | ".join(f"{spec_label(s)} = {s}" for s in spec_strs), flush=True)

    # 分片可复用（中断后重跑即续跑），但**复用也要过参数闸**：分片里的读数是按当时那套
    # 策略/场数算的，参数变了还复用会得到半新半旧的混合结果，而控制台看不出来。
    items = [(e, f, os.path.join(parts_dir, f"{int(e)}@{f}.json")) for e, f in pairs]
    todo = [it for it in items if force or not os.path.exists(it[2])]
    shards: list[dict] = [d for d in (load_shard(it[2], n_per, spec_strs, seed0, batch)
                                     for it in items if it not in todo) if d]
    if shards:
        print(f"  复用已有分片 {len(shards)} 项，本次实跑 {len(todo)} 项", flush=True)

    failed: list[str] = []
    w = runtime.workers(workers)
    if w and w > 1 and len(todo) > 1:
        with ThreadPoolExecutor(max_workers=min(w, len(todo))) as ex:
            futs = [ex.submit(_launch_item, e, f, n_per, spec_strs, seed0, batch, part)
                    for e, f, part in todo]
            for fut in tqdm(as_completed(futs), total=len(futs), desc="评测（按怪组×层）",
                            unit="项", dynamic_ncols=True, mininterval=0.4):
                _report(fut.result(), shards, failed, n_per, spec_strs, seed0, batch)
    else:
        for e, f, part in tqdm(todo, desc="评测（按怪组×层）", unit="项", dynamic_ncols=True):
            _report(_launch_item(e, f, n_per, spec_strs, seed0, batch, part),
                    shards, failed, n_per, spec_strs, seed0, batch)
    if failed:
        print(f"{len(failed)}/{len(todo)} 项没跑成，已排除：{failed}", flush=True)
    if not shards:
        raise SystemExit("所有项都失败，见上面逐项报错")

    _matrices(shards, spec_strs)
    _report_paired(shards, spec_strs)
    data = {"n_per": int(n_per), "seed0": int(seed0), "batch": int(batch),
            "specs": list(spec_strs), "items": sorted(shards, key=lambda d: (d["floor"], d["enc_id"]))}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    print(f"\n已写出 {out}（{time.perf_counter() - t0:.0f}s）", flush=True)
    return shards


def main() -> int:
    p = argparse.ArgumentParser(description="按怪组×出现层分别评测（每项固定场数、同 seed 配对）",
                                epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("n_per", type=int, help="每组场数")
    p.add_argument("specs", help="策略，逗号(或分号)分隔：random | teacher:N | net:path[:sims]")
    p.add_argument("seed0", type=int, nargs="?", default=None, help=f"种子起点（默认 {EVAL_SEED0}）")
    p.add_argument("out", nargs="?", default=None, help="结果 json（默认按参数指纹命名）")
    p.add_argument("--one", type=int, default=None,
                   help="内部：只跑这一项（遭遇 id；父进程逐项拉起子进程）")
    p.add_argument("--floor", type=int, default=None, help="内部：`--one` 的分布层")
    p.add_argument("--part", default=None, help="内部：分片路径")
    p.add_argument("--force", action="store_true", help="忽略已有分片，重跑")
    p.add_argument("--mcts-batch", type=int, default=1,
                   help="MCTS 叶批量（1 = 与历史等价；影响数值可比性）")
    p.add_argument("--rollout-threads", type=int, default=1, help="每项子进程内 torch 线程数")
    p.add_argument("--workers", default=None,
                   help="并行项数；'serial'/0 串行；不给 = 按 cgroup 配额")
    a = p.parse_args()
    specs = [s for s in a.specs.replace(";", ",").split(",") if s]
    if not specs:
        raise SystemExit("没有策略（`specs` 为空）")
    seed0 = EVAL_SEED0 if a.seed0 is None else a.seed0
    if a.one is not None:
        if a.floor is None or a.part is None:
            raise SystemExit("`--one` 必须配 `--floor` 与 `--part`")
        shard = evaluate_item(sts.MonsterEncounter(a.one), a.floor, a.n_per, specs, seed0,
                              batch=a.mcts_batch, threads=a.rollout_threads)
        with open(a.part, "w", encoding="utf-8") as f:
            json.dump(shard, f, ensure_ascii=False)
        print("  " + _item_line(shard), flush=True)
        return 0
    if seed0 < EVAL_SEED0:
        raise SystemExit(f"评测种子必须 ≥ EVAL_SEED0={EVAL_SEED0}（held-out 区间才能评）——"
                         f"收到 seed0={seed0}")
    evaluate_encs(a.n_per, specs, seed0=seed0, out=a.out, force=a.force,
                  batch=a.mcts_batch, workers=a.workers)
    return 0


if __name__ == "__main__":
    sys.exit(main())
