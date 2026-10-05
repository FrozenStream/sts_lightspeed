"""`shape_floor`（**层数 → 发育状态**）的守卫 —— 「直接指定怪物」训练流程的地基。

用法：python _chk_shape.py [漫游局数=1]

查六件事（都是**不变量**，不是快照）：
  ① 调用方约定的**幕分界固定装置**（`_set_floor`）与引擎在漫游里给的 `gc.act` 逐点一致
  ② 定点复现：同 (层, 种子) 调两次 ⇒ 牌组 / 遗物 / 药水 / hp / 金币 逐项相同
  ③ 趋势与量级：层数↑ ⇒ 平均牌组张数 / 平均遗物件数 **单调↑**（牌组带容差），
     且**平均牌组张数 ≈ 表值**（±3 张）；平均**基础牌**（打击/防御）张数**单调↓**（删牌随层上升）
  ④ 取值域：药水 ≤ 3 瓶、`1 ≤ hp ≤ max_hp`、`gold ≥ 0`、且**层号/幕不被函数改写**
  ⑤ 端到端：**指定怪物 + 指定层数**能直接开局（怪数/血/总牌组/快照长度都对得上）
  ⑥ 发放池里没有引擎表达不了的内容（`BANNED_POTIONS` / `BANNED_RELICS`）

⚠️ **契约**：`floor_num` / `act` 是 `GameContext` 的状态，**`shape_floor` 不碰**（只读它们的值）。
   本守卫自己用 `_set_floor(gc, f)` 摆好（幕分界是实测出来的固定装置）再调它；
   指定怪物的训练流程也一样 —— 层号/幕由调用方设，`shape_floor` 只管那六样发育物资。

⚠️ 为什么值得有：这套状态现在同时供 **采集 / 评测 / 指定怪物训练** 三条路。
本轮之前它在三个调用点各抄了一遍（reset + 发牌 + 发资源 + hp/金币），
已经合并成 `scene.shape_floor` —— 这个守卫挡的就是"再抄一遍"。
"""
from __future__ import annotations

import sys

import numpy as np
import slaythespire as sts

import devshape
from scene import BANNED_RELICS, act_of, shape_floor
from walk import ASCENSION, CHARACTER

# 输出重定向（`| tee log`）会让 stdout 退化成 GBK；本文件打 ✅ ⇒ 不加这行会崩在输出上。
# （以前是靠 `walk` 顺带 `import runtime` 取得的副作用，那个依赖已随 `walk` 库化去掉。）
for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

FAILS: list[str] = []
_COUNTS_OFF = sts.snapshot_layout()["counts_off"]

# 牌组张数趋势的容差（张）：相邻段表值只差 0.7~4.8 张，
# 而单段采样 σ≈4 张、Boss 遗物还会 ±3 张地动牌组 ⇒ 严格单调不是这张表能支撑的断言。
DECK_TOL = 1.5
# 实测均值与**表值**对表的容差（张）：单段 σ/√24 ≈ 0.9，取 3 倍再多留一点给遗物扰动。
DECK_ABS_TOL = 3.0


def check(name, ok, detail=""):
    print(f"  {'✅' if ok else '❌'} {name}{('  ' + detail) if detail else ''}")
    if not ok:
        FAILS.append(name)


def _gc(seed=0):
    return sts.GameContext(CHARACTER, seed, ASCENSION)


# 幕分界（**调用方约定的固定装置**）：`face_floor` 不推 act，所以这里自己摆。
# 边界来自实测（2026-09-25，漫游 + 回血作弊走深）：act1 的战斗只出现在 floor 1..16，
# act2 首个战斗层 = 18（17 是幕间节点、不进战斗）⇒ 整点分界 17/34/51。
def _set_floor(gc, f):
    """把 gc 摆到第 `f` 层 —— **调用方的责任**（`shape_floor` 只读不写）。"""
    gc.floor_num = int(f)
    gc.act = act_of(f)


def state_of(gc):
    """把 gc 的发育状态压成可比较的元组（牌组/遗物/药水按内容排序，不含顺序）。"""
    deck = sorted(f"{int(c.id)}:{int(bool(c.upgraded))}" for c in gc.deck)
    relics = sorted(str(r.id.name) for r in gc.relics)
    empty = (int(sts.Potion.INVALID), int(sts.Potion.EMPTY_POTION_SLOT))
    pots = sorted(int(p) for p in gc.potions[:gc.potion_capacity]
                  if int(p) not in empty)
    return (tuple(deck), tuple(relics), tuple(pots), int(gc.cur_hp), int(gc.gold),
            int(gc.act), int(gc.floor_num))


def check_act_map(n_runs=1):
    """① `act_of` 与引擎逐点一致（走漫游取真实 (floor, act) 对）。"""
    want = {0: 1, 16: 1, 17: 2, 18: 2, 33: 2, 34: 3, 50: 3, 51: 4, 60: 4}
    got = {f: act_of(f) for f in want}
    check("`act_of` 的幕分界（0/16→1, 17..33→2, 34..50→3, 51+→4）", got == want, f"{got}")
    import walk
    seen = set()
    orig = walk.play_battle

    def patched(gc, choose, rng):
        gc.cur_hp = gc.max_hp            # 回血作弊：只为走深以覆盖 act2 的分界（不影响 act）
        seen.add((int(gc.floor_num), int(gc.act)))
        return orig(gc, choose, rng)

    walk.play_battle = patched
    try:
        for s in range(int(n_runs)):
            walk.run_one(s, walk.random_choose)
    finally:
        walk.play_battle = orig
    bad = sorted((f, a, act_of(f)) for f, a in seen if a != act_of(f))
    acts = sorted({a for _, a in seen})
    check(f"固定装置 == 引擎（{len(seen)} 个观测点，覆盖 act {acts}）", not bad,
          f"不符：{bad[:5]}" if bad else f"floor {min(f for f, _ in seen)}..{max(f for f, _ in seen)}")
    if max(acts) < 2:                 # 漫游能走多深是随机的 ⇒ 覆盖不足只提醒，不判负
        print(f"     ⚠️ 本局只走到 act {acts} —— act2+ 的分界这次没被实测覆盖"
              f"（加大漫游局数：python _chk_shape.py 6）")
    return acts


def check_reproducible():
    """② 同 (层, 种子) 逐项相同。"""
    gc1 = _gc(0)
    _set_floor(gc1, 37)
    shape_floor(gc1, 5)
    gc2 = _gc(0)
    _set_floor(gc2, 37)
    shape_floor(gc2, 5)
    check("定点复现：同 (层=37, 种子=5) 两次调用逐项相同", state_of(gc1) == state_of(gc2),
          f"牌组 {len(gc1.deck)} 张 / 遗物 {len(gc1.relics)} / hp {gc1.cur_hp}")


def check_trend(n_seeds=24):
    """③ 趋势与量级：均值随层数单调↑ + 牌组均值 ≈ 表值；④ 取值域。"""
    floors = (5, 15, 25, 37, 45)
    rows = []
    for f in floors:
        decks, relics, basics, pots_ok, hp_ok, gold_ok, act_ok, floor_ok = (
            [], [], [], True, True, True, True, True)
        for s in range(n_seeds):
            gc = _gc(s)
            _set_floor(gc, f)
            shape_floor(gc, s)
            decks.append(len(gc.deck))
            relics.append(len(gc.relics))
            basics.append(sum(1 for c in gc.deck if c.is_starter_strike_or_defend))
            n_pot = sum(1 for p in gc.potions[:gc.potion_capacity]
                        if int(p) not in (int(sts.Potion.INVALID), int(sts.Potion.EMPTY_POTION_SLOT)))
            pots_ok &= 0 <= n_pot <= 3
            hp_ok &= 1 <= int(gc.cur_hp) <= int(gc.max_hp)
            gold_ok &= int(gc.gold) >= 0
            act_ok &= int(gc.act) == act_of(f)      # ← 不该被函数改写
            floor_ok &= int(gc.floor_num) == f
        rows.append((f, sum(decks) / n_seeds, sum(relics) / n_seeds,
                     sum(basics) / n_seeds))
        check(f"floor {f:2d} 取值域（药水≤3 / 1≤hp≤max / gold≥0 / act/floor 未被改写）",
              pots_ok and hp_ok and gold_ok and act_ok and floor_ok, f"平均牌组 {rows[-1][1]:.1f} 张、遗物 {rows[-1][2]:.1f} 件")
    up_r = all(rows[i][2] < rows[i + 1][2] for i in range(len(rows) - 1))
    # 牌组张数：相邻段**只差 ~1 张**，而 `draw()` 的 σ 就有 ~4 张 ⇒ 十几个 seed 做严格单调
    # 等于掷硬币。所以拆成两句可判的话：趋势用容差式单调，量级改用**与表值对表**。
    # 对表同时还挡住了段号接错 / 缩放没接上这类“看趋势看不出来”的毛病。
    # 基线牌组张数从引擎读（本守卫走 `walk` 的 A0：5 打击 + 4 防御 + 痛击），不手抄。
    _base = _gc(0)
    _base.reset_run_resources()
    exp = [len(_base.deck) + sum(devshape.how_many_to_add(f))
           - devshape.REMOVALS[devshape.stage(f)] for f in floors]
    up_d = all(rows[i][1] < rows[i + 1][1] + DECK_TOL for i in range(len(rows) - 1))
    check(f"层数↑ ⇒ 平均牌组张数单调↑（容差 {DECK_TOL} 张）",
          up_d, " → ".join(f"{r[1]:.1f}" for r in rows))
    check(f"平均牌组张数 ≈ 表值（±{DECK_ABS_TOL} 张）",
          all(abs(rows[i][1] - exp[i]) <= DECK_ABS_TOL for i in range(len(exp))),
          " → ".join(f"{r[1]:.1f}/{e:.1f}" for r, e in zip(rows, exp)))
    check("层数↑ ⇒ 平均遗物件数单调↑", up_r,
          " → ".join(f"{r[2]:.1f}" for r in rows))
    down_b = all(rows[i][3] - rows[i + 1][3] > -0.4 for i in range(len(rows) - 1))
    check("层数↑ ⇒ 平均基础牌（打击/防御）张数单调↓（删牌随层上升）",
          down_b and rows[0][3] - rows[-1][3] >= 1.0,
          " → ".join(f"{r[3]:.1f}" for r in rows))


def check_targeted_battle(enc_name="THREE_SENTRIES", floor=37, seed=5):
    """⑤ 端到端：**指定怪物 + 指定层数** 能直接开局（这就是重点训练流程的第一步）。"""
    gc = _gc(seed)
    _set_floor(gc, floor)                # ← 层号/幕由**调用方**设（shape_floor 不碰）
    shape_floor(gc, seed)
    enc = getattr(sts.MonsterEncounter, enc_name)
    b = sts.BattleContext()
    b.init(gc, enc)                                  # ← 直接指定怪物（不需要漫游）
    snap = b.get_snapshot_np()
    print(f"  {enc_name}@{floor}: 怪数={b.monster_count} hp={b.player_hp}/{b.player_max_hp} "
          f"手牌={len(b.get_hand())} 总牌组={b.master_deck_size}（gc.deck={len(gc.deck)}）")
    check("战斗接到的血量 == `shape_floor` 摆好的血量", int(b.player_hp) == int(gc.cur_hp),
          f"{int(b.player_hp)} vs {int(gc.cur_hp)}")
    check("战斗接到的总牌组 == `gc.deck`（引擎真值 `master_deck_size`，快照里那是 ÷30 的输入）",
          int(b.master_deck_size) == len(gc.deck))
    check("开局就能拿快照（长度 == OBS_DIM）", int(snap.size) == int(sts.snapshot_layout()["len"]),
          f"{int(snap.size)}")


def check_no_unsupported(n_seeds=150, floors=(4, 12, 30, 50)):
    """⑦ 发放池里不许出现**引擎表达不了的内容**（`BANNED_POTIONS` / `BANNED_RELICS`）。

    判据两条（同一轮扫描里取）：
      · 快照的遗物多热块里不含 `BANNED_RELICS`；
      · 开局输入态不是 `CARD_SELECT` —— `GAMBLING_CHIP`（回合开始强制进弃牌多选屏）
        会让引擎只枚举出一个 `MULTI_CARD_SELECT` 动作，而统一下标里没有它
        （`actionToIdx` 的 `default:` 当场抛）⇒ 采集**整项作废**。`TOOLBOX`（开局三选一）
        的屏**编码得出来**，挡它只是为了坐住「开局观测必须是 `PLAYER_NORMAL`」这条口径。

    ⚠️ **没扫到的东西**：这只查开局。中局才出选牌屏的遗物（如 `NILRYS_CODEX`，回合末）
    这一项看不出来 —— 那种情况会在采集时由 `actionToIdx` 当场抛（响亮失败，不静默）。

    ⚠️ **必须扫 seed**：资源按 seed 发，单 seed 只覆盖一套遗物。灵敏度实测（清空禁用表）：
    floor 4/12/30/50 各 400 个种子里分别有 1/5/10/25 个开局即多选屏。
    """
    L = sts.snapshot_layout()
    off, n_rel = L["relic_off"], L["relic_len"]
    got_relics: set[str] = set()
    n_screen = 0
    for f in floors:
        for s in range(n_seeds):
            gc = _gc(s)
            _set_floor(gc, f)
            shape_floor(gc, s)
            b = sts.BattleContext()
            b.init(gc, sts.MonsterEncounter.THREE_SENTRIES)
            blk = b.get_snapshot_np()[off:off + n_rel]
            got_relics.update(sts.RelicId(int(i)).name for i in np.flatnonzero(blk))
            if b.input_state == sts.InputState.CARD_SELECT:
                n_screen += 1
    banned = [r.name for r in BANNED_RELICS if r.name in got_relics]
    check("⭐ 禁用遗物一件都没发出去", not banned, f"{banned}")
    check("⭐ 开局不出现「弃牌多选屏」（引擎对它只给 `MULTI_CARD_SELECT`）",
          n_screen == 0, f"{n_screen}/{n_seeds * len(floors)}")


def main():
    n_runs = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    print("① 幕分界固定装置对答案（漫游）")
    check_act_map(n_runs)
    print("② 定点复现")
    check_reproducible()
    print("③④ 趋势与取值域")
    check_trend()
    print("⑤ 指定怪物直接开局（端到端）")
    check_targeted_battle()
    print("⑥ 发放池里没有引擎表达不了的内容")
    check_no_unsupported()
    print()
    if FAILS:
        print(f"❌ {len(FAILS)} 项失败：{FAILS}")
        return 1
    print("✅ 全通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
