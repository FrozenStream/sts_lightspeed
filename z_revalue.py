"""战损（HP 消耗）度量 + **零搜索**的重放核对。

## 为什么可以零搜索

重放只需要缓存的 π 列：`teacher_action` 执行的就是 `np.argmax(pi)`，而 `pi` 是归一化后的
float32 访问分布，缓存里存的就是**同一个数组**（`assemble` 只做了 `np.asarray(..., float32)`）。
⇒ 逐局**重放**即可复原整场轨迹，不跑任何搜索。

## 重放核对是设计的一部分（不是可选项）

重放出来的**终局列**必须与缓存里那 6 列**逐位相同**。不一致 ⇒ 轨迹分叉（平局
tie-break 不同）⇒ 立刻暴露，而不是悄悄产出一份「看着一样、其实是另一条轨迹」的数据。
它同时验证缓存的 `seed`/`enc`/`enc_floor` 三列确实指向同一场景。

## 为什么先做度量

项目里**从来没有量过战损**：所有指标都是胜率（`_rollout` 只返回 `(win, steps)`）。
要谈「在胜率不掉的前提下减少耗油」，先得有这个数，否则改奖励就是凭感觉。

用法：
  python z_revalue.py stats <缓存> [前半场数]      # 战损 + 重放核对（零搜索）
  python z_revalue.py zsplit <缓存>                # z 的方差分配对比
"""
import os
import sys

import numpy as np
import slaythespire as sts

from collect import make_stage_battle
from zspec import Terminal, Z_COLS
from aznet import END_TURN_IDX, POTION_BASE, legal_actions_and_idx, terminal_state

MAX_STEPS = 3000


def replay(pi_act, pi_sel, seed, enc, floor):
    """用缓存里的 π（argmax）重放一局。返回 (终局读数, n_potion_acts, n_steps, err)。"""
    _, b, _ = make_stage_battle(seed, enc, floor)
    npot, steps, err = 0, 0, None
    for pa, ps in zip(pi_act, pi_sel):
        if b.is_battle_over():
            err = "缓存行数多于实际步数（数据比轨迹长）"
            break
        if steps >= MAX_STEPS:
            err = "超过 MAX_STEPS"
            break
        # 选择态 / 非选择态各占一半：`_assemble` 把另一半填零
        row = ps if ps.sum() > 0 else pa
        if row.sum() <= 0:
            err = "该行 π 全零（不是合法决策点）"
            break
        k = int(np.argmax(row))
        # ⚠️ 用 `legal_actions_and_idx` 当守卫，**不要**盲信 `idx_to_action`：
        #    后者对未映射的下标返回 default-constructed Action（**非法哨兵**）而不报错，
        #    而执行一个非法 Action 会把引擎打成段错误（不是抛异常）。
        legal = {i: a for a, i in legal_actions_and_idx(b)}   # ⚠️ 下标当键（别写反）
        if k not in legal:
            err = f"argmax 下标 {k} 不是合法动作（π 与状态不匹配 ⇒ 轨迹已错位）"
            break
        a = legal[k]
        if b.action_to_idx(a) != k:
            err = f"idx {k} 回查不成立"
            break
        npot += int(POTION_BASE <= k < END_TURN_IDX)   # 药水段 = [POTION_BASE, END_TURN_IDX)
        a.execute(b)
        steps += 1
    if err is None and not b.is_battle_over():
        err = "重放结束时战斗未结束"
    return terminal_state(b), npot, steps, err


def blocks(bid):
    """每个 bid 的行区间。`collect.assemble` 按 seed 升序拼，所以 bid 非递减。"""
    nb = int(bid.max()) + 1
    st = np.searchsorted(bid, np.arange(nb), side="left")
    return [(b, int(st[b]), int(st[b + 1]) if b + 1 < len(st) else len(bid))
            for b in range(nb)]


def load(path):
    d = np.load(path)
    return {k: d[k] for k in d.files}


def per_seg(rows: list[Terminal]):
    """一段的胜率 + 战损。全部只依赖终局列。"""
    if not rows:
        return None
    w = np.array([t.t_win for t in rows])
    hp = np.array([t.t_hp for t in rows])
    mx = np.array([t.t_max_hp for t in rows])
    turn = np.array([t.t_turn for t in rows])
    en = np.array([t.t_enemy for t in rows])
    ratio = hp / np.maximum(1, mx)
    wins = w > 0
    return dict(n=len(rows), win=w.mean(), hp_win=hp[wins].mean() if wins.any() else 0.0,
                hp_med=float(np.median(hp[wins])) if wins.any() else 0.0,
                hp_all=hp.mean(), loss_rate=1 - ratio[wins].mean() if wins.any() else 1.0,
                low20=float((hp[wins] <= 20).mean()) if wins.any() else 0.0,
                low10=float((hp[wins] <= 10).mean()) if wins.any() else 0.0,
                high60=float((hp[wins] >= 60).mean()) if wins.any() else 0.0,
                turn_win=turn[wins].mean() if wins.any() else 0.0,
                turn_loss=turn[~wins].mean() if (~wins).any() else 0.0,
                enemy_loss=en[~wins].mean() if (~wins).any() else 0.0,
                hp_max=int(mx.max()) if len(mx) else 0)


# ⚠️ 药水槽没绑到 Python（只有 `drink_potion`/`discard_potion` 两个**动作**），
#    所以只能从「下标落在药水段 [60,95)」反推动作类型 —— 恰好够用，不用加绑定。
def cmd_stats(path, half=None):
    d = load(path)
    bid = d["bid"]
    nb = int(bid.max()) + 1
    half = half or nb
    print(f"── {path}（{len(bid)} 样本 / {nb} 场；前半 {half} 场当教师段）")

    seg_rows, errs, npot_all, seg_ok = {0: [], 1: []}, [], 0, {0: [0, 0], 1: [0, 0]}
    for b, i0, i1 in blocks(bid):
        # 场景由缓存自带的三列重放（`seed` + `enc` + `enc_floor`）—— 不再按 bid 推断 seed：
        # 按怪组采的缓存里 seed 会**跳组重复**，推断必错。
        s, npot, _steps, err = replay(d["pi_act"][i0:i1], d["pi_sel"][i0:i1],
                                      int(d["seed"][i0]),
                                      sts.MonsterEncounter(int(d["enc"][i0])),
                                      int(d["enc_floor"][i0]))
        npot_all += npot
        seg = 0 if b < half else 1
        if err:
            errs.append((b, err))
            seg_ok[seg][1] += 1
            continue
        ok = all(getattr(s, c) == d[c][i0] for c in Z_COLS)   # 终局列按场复制，看首行即可
        seg_ok[seg][0] += int(ok)
        seg_ok[seg][1] += 1
        seg_rows[seg].append(s)

    print("  重放自检（终局列必须与缓存逐位一致；这同时验证 seed/enc/floor 三列）：")
    for seg, (ok, tot) in sorted(seg_ok.items()):
        tag = "教师段" if seg == 0 else "自对弈段"
        mark = "✅" if ok == tot else "❌"
        print(f"    {mark} {tag}  {ok}/{tot}")
    if errs:
        print(f"    ⚠️ {len(errs)} 场重放失败，前 3 条：{errs[:3]}")
    if npot_all:
        print(f"  药水：{npot_all} 个决策点上有合法药水动作")
    else:
        print("  药水：**0** 个决策点上有合法药水动作 ⇒ 本场景根本没有药水")
        print("        （药水槽没绑到 Python；且这套场景从 0 药水开局）")

    print(f"\n  ⭐ 战损（这是项目里第一次量它；只统计自检通过的那一段）")
    for seg in (0, 1):
        st = per_seg(seg_rows[seg])
        if st is None:
            continue
        ok, tot = seg_ok[seg]
        ver = ok == tot
        tag = "教师段" if seg == 0 else "自对弈段"
        print(f"    [{tag}]（重放{'已' if ver else '未'}验证，{st['n']} 场）")
        if not ver:
            print("      ⚠️ 这一段轨迹已分叉，下面的数只能当粗略参照")
        print(f"      胜率            {st['win']:6.1%}")
        print(f"      平均剩余 HP     赢局 {st['hp_win']:5.1f} / 全局 {st['hp_all']:5.1f}"
              f"（上限 {st['hp_max']}）")
        print(f"      中位剩余 HP     赢局 {st['hp_med']:5.1f}")
        print(f"      **战损率**      赢局 {st['loss_rate']:6.1%}")
        print(f"      赢局剩余≤20/≤10 {st['low20']:6.1%} / {st['low10']:6.1%}"
              f"     ≥60 {st['high60']:6.1%}")
        print(f"      平均回合数      赢局 {st['turn_win']:5.1f} / 败局 {st['turn_loss']:5.1f}")
        print(f"      败局时敌方剩余血比 {st['enemy_loss']:6.1%}"
              f"   ⇒ 引擎对败局是**分级**的，我们的 z 把它压成 −1")


def cmd_zsplit(path):
    """`z` 与引擎 `evaluateEndState` 的**方差分配**对比。

    为什么关心：MSE 训练的 value 头会把容量花在方差大的那一项上。若 `z` 的方差几乎
    全来自「赢没赢」，「赢的时候剩多少血」就只是个小残差 ⇒ value 头学不到它，
    MCTS 也就不会为了省血而调整选点。

    做法：把 `x = 1{win} * 赢局分数` 拆成
      `Var(u * c)`（把「赢时分数」压成它的条件均值 c ⇒ 只剩 win/loss 的波动）
    与 `Var(x) - Var(u*c)`（纯粹来自「赢时分数差异」= HP）。
    ⚠️ 引擎那一侧把**败局分数当常数**（实际在 [-5, 15]，不到赢局尺度的 0.1%）；
       我们这一侧的 z 败局本来就是常数 −1，所以两边同口径。
    """
    d = load(path)
    win = d["t_win"] > 0
    r = (d["t_hp"] / np.maximum(1, d["t_max_hp"]))[win]
    p = float(win.mean())

    def split(score):
        """score = 赢局分数数组；返回 (Var 总量, 其中来自 win/loss 的部分)。"""
        ex = p * score.mean()
        v = p * (score ** 2).mean() - ex ** 2
        v0 = score.mean() ** 2 * p * (1 - p)
        return v, v0

    v_z, v0_z = split(1.0 + r)
    v_e, v0_e = split(3500.0 + 8000.0 * r)

    print(f"  ── {os.path.basename(path)}")
    print(f"  p(win) = {p:.3f}   E[hp_ratio|win] = {r.mean():.3f}   "
          f"std = {r.std():.3f}   （{len(r)} 场胜局）")
    print(f"\n  {'口径':<22s} {'Var 总量':>12s} {'win/loss 占':>12s} {'HP 占':>8s}")
    for lab, v, v0 in (("我们的 z（hp 口径）", v_z, v0_z),
                       ("引擎 3500+8000r", v_e, v0_e)):
        print(f"  {lab:<22s} {v:12.4g} {v0 / v:12.1%} {(1 - v0 / v):8.1%}")
    hp_z, hp_e = 1 - v0_z / v_z, 1 - v0_e / v_e
    print(f"\n  ⇒ 引擎的 HP 注意力是我们的 **{hp_e / hp_z:.2f}×**"
          f"（{hp_e:.1%} vs {hp_z:.1%}）")
    print("     MSE 的 value 头按方差分配容量 ⇒ 剩下那点 HP 残差基本学不到，")
    print("     于是 MCTS 不会为了省血而改选点。")


def main():
    a = sys.argv[1:]
    mode = a[0] if a else "stats"
    if mode == "stats" and len(a) > 1:
        cmd_stats(a[1], int(a[2]) if len(a) > 2 else None)
    elif mode == "zsplit" and len(a) > 1:
        cmd_zsplit(a[1])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
