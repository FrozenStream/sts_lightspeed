"""守卫：**「死了」和「要复活」必须分得开**，以及 `monstersAlive` / `deckSize`。

## 怎么分

生死/去留**只有 `targetable`（= `!isDeadOrEscaped()`）一列**；`MonsterGroup::monsterCount`
又**只增不减**（真正死掉才 `--monstersAlive`），死怪一直占着槽 ⇒ 光看 `targetable` 分不出
「永久尸体」和「下回合满血站起来」。**「为什么不能打」写在意图 id 里**：

    AWAKENED_ONE 一阶段倒下当帧 → intent = AWAKENED_ONE_REBIRTH
    DARKLING 倒下当帧            → intent = DARKLING_REGROW →（它那回合空过）DARKLING_REINCARNATE
    逃跑的 Looter / Mugger      → intent = LOOTER_ESCAPE / MUGGER_ESCAPE
    永久尸体                     → intent 停在它最后一次滚出的招式

这个守卫就是那件事的**唯一**证据来源。断言分三组：

  1. `counts` 块：`monstersAlive` == 逐槽「**可选中**」数（判据 `!isDeadOrEscaped()`，半死不算）
     ⇒ 只有在**没有 halfDead 怪**的普通战里它才等于「`curHp>0` 的个数」（本节跑的就是普通战）。
     ⚠️ 它**不是**判胜用的量：判胜看 `areMonstersBasicallyDead()`（同一槽位上把 halfDead 算活着），
     两者在觉醒者半死、暗灵待复活窗口里会差 1。
     `monsterCount` 允许 > `monstersAlive`（尸体占槽），且**只增不减**；
     `deckSize` 必须等于牌组块的非零槽数（牌组块走均值池化 ⇒ 张数只能从这一维读）。
  2. 觉醒者：倒下那一帧必须 `curHp<=0 & targetable=0` 且 **intent == AWAKENED_ONE_REBIRTH**，
     随后**真的复活**（二阶段 320 血、意图换成 DARK_ECHO）；那一帧的 `(curHp, targetable)`
     与一个**永久尸体**完全相同 ⇒ 只有意图 / `miscInfo` 能分开它们。
  3. 暗灵：倒下当帧 **intent == DARKLING_REGROW**，随后转 `DARKLING_REINCARNATE`，
     复活时 `curHp == maxHp // 2`（半数血，与觉醒者的满血复活不同）；而**最后一只**暗灵
     死时 `die()` 提前 return（`areMonstersBasicallyDead()`）⇒ 不进待复活。
  4. ⭐ **判胜的边界**（对应原版 `cannotLose`）：一阶段觉醒者死后**同一回合内**清掉小怪
     ⇒ **不许**判胜（原版它的 `die()` 在 `cannotLose` 期间是空操作，`isDying` 从不置位）；
     三小黑**同一回合内**清空 ⇒ **必须**判胜（原版 `Darkling.damage()` 同种全 halfDead 时
     人人 `die()`）。这两条一起钉住"用通配「有 halfDead 就不判胜」把暗灵改成打不死"的回归。

用法：`python _chk_revive.py`
"""
import random
import sys

import numpy as np

import slaythespire as sts
from azlayout import (COUNTS_DECK_SIZE_IDX, COUNTS_MONSTER_ALIVE_IDX, LAYOUT as L,
                      MONSTER_SLOTS, REVIVE_MOVES)
from aznet import teacher_action
from collect import make_stage_battle
from scene import ascension_of


def _monster_field(field):
    """怪物块第 `field` 个字段在 5 个槽上的下标（字段清单见 `azlayout.MONSTER_FIELDS` 上方）。

    ⚠️ 这里**故意**只有本守卫用这些字段。`azlayout` 里的 `*_IDX` 只留**模型/采集真有消费者**
    的那些（`monster_id`、`intent_move_id`、`targetable`…）—— 给测试专用的列也起个全局别名
    就是过度设计。
    """
    return [L["monster_off"] + L["monster_fields"] * i + field for i in range(MONSTER_SLOTS)]


MONSTER_INTENT_IDX = _monster_field(1)

def move_name(s, k):
    """槽 k 的意图招式名（`sts.MonsterMoveId`）—— 意图 id 是**名义列**（原样输出）⇒ 从快照读。"""
    return sts.MonsterMoveId(int(s[0][MONSTER_INTENT_IDX[k]])).name

for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

SIMS = 50
MAX_STEPS = 400
OK = FAIL = 0


def check(name, cond, extra=""):
    global OK, FAIL
    if cond:
        OK += 1
        print(f"  ✓ {name}")
    else:
        FAIL += 1
        print(f"  ✗ {name} {extra}")


def run_frames(enc, seed=30000, floor=12, max_steps=MAX_STEPS, sims=SIMS):
    """用教师跑一场指定 encounter 的战斗，**逐帧留快照**。

    留帧（而不是只留终局）是因为要抓「倒下那一帧」—— 它只存在一帧。
    """
    _gc, b, _enc = make_stage_battle(seed, enc, floor)
    frames = []
    steps = 0
    while not b.is_battle_over() and steps < max_steps:
        frames.append(frame(b))
        a = teacher_action(b, sims, random.Random(0))
        if a is None:
            break
        a[3].execute(b)
        steps += 1
    frames.append(frame(b))
    return b, frames


# 一帧 = `(快照, 逐槽真值, 计数)`。**数值列一律走引擎真值**：快照已归一化，而除数写在
# C++ 的赋值处、**还会被调** ⇒ 在 Python 里乘回一个写死的除数迟早出错：
# 实测除数从 ÷30 改成 ÷5 后，乘回 30 得到 6，拿它当循环上界就**读到怪物块外面**
# （读成第一个玩家属性列）⇒ 那一帧“多出”一个活怪，静默假数据。
# 快照仍要用：**索引 / 名义列是原样的**（÷1）—— cardId、monsterId、intentMoveId、targetable。
_HP, _MAXHP, _MISC = 0, 1, 2
_CNT_COUNT, _CNT_ALIVE, _CNT_DECK = 0, 1, 2


def frame(b):
    """抓一帧：`(快照, [(curHp, maxHp, miscInfo) 逐槽], (monsterCount, monstersAlive, deckSize))`。"""
    return (np.asarray(b.get_snapshot_np(), dtype=np.float64),
            [(int(m.cur_hp), int(m.max_hp), int(m.misc_info)) for m in b.monsters],
            (int(b.monster_count), int(b.monsters_alive), int(b.master_deck_size)))


def hp(f, k):
    """槽 k 的 curHp —— **引擎真值**（无需乘回任何除数）。"""
    return f[1][k][_HP]


def max_hp(f, k):
    return f[1][k][_MAXHP]


def misc_info(f, k):
    return f[1][k][_MISC]


def targetable(f, k):
    """`targetable` 在快照里是**名义列**（原样输出）⇒ 直接读。"""
    return int(f[0][L["monster_off"] + L["monster_fields"] * k + 5])


def mon_id(f, k):
    return int(f[0][L["monster_off"] + L["monster_fields"] * k + 0])


def alive_by_slots(f):
    """逐槽数 `curHp>0`（不信 `monstersAlive`，它是要被检查的对象）。

    ⚠️ 不再拿“槽数上限”当循环上界（那个值一旦是归一化的就退化成 0 或错值）——
    逐槽真值数组多长就数多长。
    """
    return sum(1 for t in f[1] if t[_HP] > 0)


def main():
    print("[布局连续性]")
    check("runCtx 后面直接是遗物块（中间没有地图块）",
          L["relic_off"] == L["run_ctx_off"] + L["run_ctx_len"],
          (L["run_ctx_off"], L["run_ctx_len"], L["relic_off"]))

    # ---- 1. counts 块：monstersAlive / deckSize --------------------------
    print("[counts：monstersAlive / deckSize 逐帧自洽]")
    b_std, frames_std = run_frames(sts.MonsterEncounter.CULTIST, seed=30001, floor=4)
    def alive_of(f):
        return f[2][_CNT_ALIVE]

    bad = [(i, alive_of(f), alive_by_slots(f))
           for i, f in enumerate(frames_std)
           if alive_of(f) != alive_by_slots(f)]
    check(f"普通战 {len(frames_std)} 帧：monstersAlive == 逐槽 curHp>0", not bad, bad[:3])
    mc = [f[2][_CNT_COUNT] for f in frames_std]
    check("monsterCount 只增不减（尸体占槽，不压缩）",
          all(x <= y for x, y in zip(mc, mc[1:])), mc)
    # ⭐ 顺手在这一场里挑一个**永久尸体**（自然打出来的，不是造的）—— 第 2 节要拿它对照。
    corpse_src = None
    for i, f in enumerate(frames_std):
        for k in range(MONSTER_SLOTS):
            if (mon_id(f, k) > 0 and hp(f, k) <= 0 and targetable(f, k) == 0
                    and move_name(f, k) not in REVIVE_MOVES):
                corpse_src = (i, k, f)
                break
        if corpse_src is not None:
            break
    check("普通战里出现过 monsterCount > monstersAlive（即真有过尸体占槽）",
          corpse_src is not None, mc)
    if corpse_src is not None:
        ic, kc, fc = corpse_src
        print(f"     自然人打出来的永久尸体：帧 {ic} 槽{kc} id={mon_id(fc, kc)} "
              f"curHp={hp(fc, kc)} targetable={targetable(fc, kc)} "
              f"intent={move_name(fc, kc)} "
              f"miscInfo={misc_info(fc, kc)}")
    # `deckSize` 与牌组块（非零槽数）必须一致 —— 牌组块走均值池化
    # ⇒ 张数只能从这一维读，而它决定抽到关键牌的概率。
    dsz = [f[2][_CNT_DECK] for f in frames_std]
    n_slot = [int((f[0][L["deck_off"]:L["deck_off"] + L["deck_slots"]] > 0).sum())
              for f in frames_std]
    check("deckSize == 牌组块里的非零槽数（全帧）", dsz == n_slot, (dsz[:3], n_slot[:3]))
    check("deckSize 战斗内不变（牌组是 run 级不变量）", len(set(dsz)) == 1, dsz)

    # ---- 2. ⭐ 觉醒者：倒下当帧的意图 = 复活招式 --------------------------
    print("[⭐ 觉醒者一阶段：意图把「待复活」从「尸体」里分出来]")
    # ⚠️ 不去「公平打一场」：这件事靠教师打赢不保证（它可能被小怪磨死、或干脆不打 Boss）。
    #    转阶段这条路径本身是**可确定**的 —— `Monster.cur_hp` 是 readwrite：
    #    把它压到 1 血，再挑一个 "真能打掉它" 的动作，一阶段必倒。
    #
    # ⚠️ 而且**别写死 slot 0**：这个遭遇（Asc 20）的编成实测是
    #    `[CULTIST(14), CULTIST(14), AWAKENED_ONE(4)]` —— 觉醒者在 **slot 2**
    #    （`probe_imit.py` 里 "4+14+14" 那个指纹就是它）。按 MonsterId 找，
    #    免得槽序一变就静默打错人（那样会"通过"一个什么都没测的检查）。
    _gc, b, _e = make_stage_battle(30000, sts.MonsterEncounter.AWAKENED_ONE, 50)
    s0 = frame(b)
    comp = [sts.MonsterId(mon_id(s0, k)).name for k in range(b.monsters.count)]
    boss_slot = next((k for k in range(b.monsters.count)
                      if sts.MonsterId(mon_id(s0, k)) == sts.MonsterId.AWAKENED_ONE), None)
    print(f"     遭遇编成 = {comp}，觉醒者在 slot {boss_slot}")
    check("遭遇里能找到 AWAKENED_ONE（否则下面的断言全是空转）", boss_slot is not None,
          comp)
    b.monsters[boss_slot].cur_hp = 1
    # ⚠️ **不能指望教师去补那一刀**：打死 1 血的觉醒者 = 它下回合满血复活，
    #    MCTS 完全看得懂这件事 ⇒ 它会**故意不碰** Boss（实测：教师场场 8 帧被小怪打死）。
    #    ⇒ 在**克隆**上扫一遍合法动作，直接挑出那个真的能打掉一阶段的动作。
    killer = None
    for a in b.enumerate_actions():
        c = b.clone()
        a.execute(c)
        cs = frame(c)
        if move_name(cs, boss_slot) == "AWAKENED_ONE_REBIRTH":
            killer = a
            break
    check("在克隆上扫合法动作，能找到「打死一阶段」的那个动作", killer is not None,
          f"合法动作 {len(b.enumerate_actions())} 个")
    frames = []
    if killer is not None:
        killer.execute(b)
        frames.append(frame(b))
        # ⚠️ 只 `end_turn`，**不请教师**：教师（C++ MCTS）的动作序列**跨机器不保证一致**
        #    （实测本地/远端在同一步就分叉）。拿它的轨迹当期待值是脆的：远端那条轨迹把
        #    剩下的小怪打光了 ⇒ `monstersAlive == 0` ⇒ 当场判胜 ⇒ 觉醒者不再复活
        #    （引擎既有行为：`die()` 里 `monstersAlive == 0` 的提前 return）。
        #    结束回合 = 玩家不出牌 ⇒ 不会误杀小怪，复活路径必然走到。
        for _ in range(6):                       # 每轮结束一次，观察复活
            if b.is_battle_over():
                break
            b.end_turn()
            frames.append(frame(b))
    down = [(i, k) for i, f in enumerate(frames) for k in range(MONSTER_SLOTS)
            if move_name(f, k) == "AWAKENED_ONE_REBIRTH"]
    check("抓到「倒下当帧意图 = AWAKENED_ONE_REBIRTH」", bool(down),
          f"outcome={int(b.outcome)} frames={len(frames)}")
    if down:
        i0, k0 = down[0]
        f0 = frames[i0]
        check("倒下当帧 curHp<=0（倒下但占着槽）", hp(f0, k0) <= 0, hp(f0, k0))
        check("倒下当帧 targetable=0（生死/去留的全部信息就这一位）",
              targetable(f0, k0) == 0, targetable(f0, k0))
        check("倒下当帧 id>0（**槽位没被压缩**，身份还在）", mon_id(f0, k0) > 0,
              mon_id(f0, k0))
        print(f"     倒下帧 {i0} 槽{k0}: id={mon_id(f0, k0)} curHp={hp(f0, k0)} "
              f"targetable={targetable(f0, k0)} intent={move_name(f0, k0)} "
              f"miscInfo={misc_info(f0, k0)}")
        # 关键对照：拿第 1 节那场**自然战斗**里打出来的永久尸体看它的 (curHp, targetable)。
        # 跨场比对照反而更有说服力：同一个 `(curHp=0, targetable=0)` 组合在**两场不同的战斗**
        # 里意味着两件完全不同的事 —— 分得开它们的只有意图 / `miscInfo`。
        if corpse_src is not None:
            _ic, kc, fc = corpse_src
            print(f"     瞬时对照（另一场自然战斗里的永久尸体）："
                  f"curHp={hp(fc, kc)} targetable={targetable(fc, kc)} "
                  f"intent={move_name(fc, kc)}")
            check("⭐ (curHp, targetable) 与永久尸体**逐位相同**",
                  (hp(fc, kc), targetable(fc, kc)) == (hp(f0, k0), targetable(f0, k0)),
                  ((hp(fc, kc), targetable(fc, kc)), (hp(f0, k0), targetable(f0, k0))))
            check("⭐ 而意图不同：待复活 = REBIRTH，永久尸体 = 它最后一次滚出的招式",
                  move_name(fc, kc) not in REVIVE_MOVES, move_name(fc, kc))
        else:
            check("需要那场自然战斗里的永久尸体做对照（上一节没采到）", False)
        # 复活：同一槽后来回到 curHp>0（意图已在它自己的回合换成 DARK_ECHO）
        revived = [i for i in range(i0 + 1, len(frames)) if hp(frames[i], k0) > 0]
        check("同一槽随后**真的复活**（curHp>0）", bool(revived),
              f"倒下于帧 {i0}，终局 outcome={int(b.outcome)}")
        if revived:
            i1 = revived[0]
            mx = max_hp(frames[i1], k0)
            # 引擎里复活就是**满血**（`MonsterSpecific.cpp` 的 `AWAKENED_ONE_REBIRTH`：`curHp = maxHp`），
            # 但**观测帧**往往已经跨过一次回合开始（如 Mercury Hourglass 的回合开始伤害）⇒ 只断言
            # 「二阶段上限 + 接近满血」，逐位满血那条由引擎实现保证。
            # 上限随进阶变（`MonsterSpecific.cpp`：`maxHp = asc9 ? 320 : 300`）⇒ 期望值按本场进阶取。
            want_mx = 320 if ascension_of(30000) >= 9 else 300
            check(f"复活后 maxHp 升到二阶段的 {want_mx}、curHp 接近满血、意图换成 DARK_ECHO",
                  mx == want_mx and hp(frames[i1], k0) >= 0.9 * mx
                  and move_name(frames[i1], k0) == "AWAKENED_ONE_DARK_ECHO",
                  (hp(frames[i1], k0), mx, move_name(frames[i1], k0)))

    # ---- 3. ⭐ 暗灵：REGROW → REINCARNATE → 半数血复活 ---------------------
    print("[⭐ 暗灵：倒下当帧 REGROW，随后 REINCARNATE，复活时半数血]")
    _gc2, b2, _e2 = make_stage_battle(30000, sts.MonsterEncounter.THREE_DARKLINGS, 34)
    # 测试夹具：把玩家血量抬高，免得它在暗灵复活前就被打死（那会让断言变成运气）。
    b2.player.max_hp = b2.player.cur_hp = 999
    s2 = frame(b2)
    live = [k for k in range(b2.monsters.count) if hp(s2, k) > 0]
    check("三暗灵场里至少有 2 只活怪（好让倒下的那只不是最后一只）", len(live) > 1, live)
    victim = live[0] if len(live) > 1 else None
    frames2 = []
    if victim is not None:
        b2.monsters[victim].cur_hp = 1
        b2.monsters[victim].block = 0
        # ⚠️ 只杀**非最后一只**：`die()` 里 `areMonstersBasicallyDead()` 的提前 return 在 REGROW 分支之前
        #    ⇒ 最后一只死不会进待复活（直接 PLAYER_VICTORY）。
        killer2 = None
        for a in b2.enumerate_actions():
            c = b2.clone()
            a.execute(c)
            cs = frame(c)
            if move_name(cs, victim) == "DARKLING_REGROW":
                killer2 = a
                break
        check("在克隆上扫合法动作，能找到「打死非最后一只暗灵」的那个动作",
              killer2 is not None, f"合法动作 {len(b2.enumerate_actions())} 个")
        if killer2 is not None:
            killer2.execute(b2)
            frames2.append(frame(b2))
            for _ in range(8):                   # 同上：不请教师，只结束回合
                if b2.is_battle_over():
                    break
                b2.end_turn()
                frames2.append(frame(b2))
        seq = [move_name(f, victim) for f in frames2]
        check("倒下当帧意图 = DARKLING_REGROW", bool(seq) and seq[0] == "DARKLING_REGROW",
              seq[:3])
        check("随后转 DARKLING_REINCARNATE（REGROW 那回合空过，rollMove 只可能滚出复活）",
              "DARKLING_REINCARNATE" in seq, sorted(set(seq)))
        back = [i for i, f in enumerate(frames2) if hp(f, victim) > 0]
        check("同一槽后来真的复活", bool(back), f"outcome={int(b2.outcome)} steps={len(frames2)}")
        if back:
            i1 = back[0]
            mx = max_hp(frames2[i1], victim)
            check("复活时 curHp == maxHp // 2（半数血，与觉醒者满血复活不同）",
                  hp(frames2[i1], victim) == mx // 2,
                  (hp(frames2[i1], victim), mx))

    # ---- 4. ⭐ 判胜边界：一阶段死 + 同回合清小怪 ⇒ 不许判胜 ------------------
    # 原版：一阶段觉醒者的 `die()` 在 `cannotLose` 期间是**空操作**（`isDying` 从不置位）⇒
    #       `areMonstersBasicallyDead()` 恒 false ⇒ 打光小怪也赢不了。
    # 端口曾把它简化成 `--monstersAlive`，于是这一条直接判胜、二阶段永不出现。
    print("[⭐ 觉醒者：一阶段死 + 同回合清小怪 ⇒ 不许判胜]")
    _gc, bx, _e = make_stage_battle(30000, sts.MonsterEncounter.AWAKENED_ONE, 50)
    bossx = next((k for k in range(bx.monsters.count)
                  if int(bx.monsters[k].id) == int(sts.MonsterId.AWAKENED_ONE)), None)
    kx = None
    if bossx is not None:
        bx.monsters[bossx].cur_hp = 1
        for a in bx.enumerate_actions():
            c = bx.clone()
            a.execute(c)
            if move_name(frame(c), bossx) == "AWAKENED_ONE_REBIRTH":
                kx = a
                break
    check("能找到「打掉一阶段」的动作", kx is not None)
    if kx is not None:
        kx.execute(bx)
        # 一阶段觉醒者半死 ⇒ 掉出「可选中」集合 ⇒ 计数器 -1（不是判胜）。
        check("⭐ 一阶段死后 `monstersAlive` 减 1（半死不算可选中）",
              int(bx.monsters_alive) == int(bx.monster_count) - 1,
              f"alive={int(bx.monsters_alive)} count={int(bx.monster_count)}")
        for m in bx.monsters:                 # 其余小怪压到 1 血
            if int(m.cur_hp) > 0:
                m.cur_hp = 1
                m.block = 0
        bx.player.energy = 99                 # 夹具：保证同回合内有资源把小怪清完
        killed = 0
        for _ in range(4):                    # 同回合内逐个打死（**不结束回合**）
            before = sum(1 for m in bx.monsters if int(m.cur_hp) > 0)
            if before == 0 or bx.is_battle_over():
                break
            for a in bx.enumerate_actions():
                c = bx.clone()
                a.execute(c)
                after = sum(1 for m in c.monsters if int(m.cur_hp) > 0)
                if after < before:            # 这一下真的打死了怪（AoE 可能一发多只）
                    bx = c
                    killed += before - after
                    break
            else:
                break
        check(f"同回合内打死小怪 {killed} 只", killed == int(bx.monster_count) - 1, killed)
        check("⭐ 计数器归零也**不许**判胜（半死靠状态算活，不看计数器）",
              int(bx.monsters_alive) == 0
              and int(bx.outcome) != int(sts.Outcome.PLAYER_VICTORY),
              f"outcome={int(bx.outcome)} alive={int(bx.monsters_alive)}")

    # ---- 5. ⭐ 判胜边界：三小黑**同回合**清空 ⇒ 必须判胜 ----------------------
    # 原版 `Darkling.damage()`：同种全 halfDead 时 `cannotLose=false` 且人人 `die()` ⇒ 集体真死。
    # 这条是防"用通配『有 halfDead 就不判胜』把它改成打不死"的闸。
    print("[⭐ 暗灵：同回合清空三只 ⇒ 必须判胜]")
    _gc, bz, _e = make_stage_battle(30000, sts.MonsterEncounter.THREE_DARKLINGS, 34)
    bz.player.max_hp = bz.player.cur_hp = 999
    for m in bz.monsters:
        m.cur_hp = 1
        m.block = 0
    bz.player.energy = 99                     # 夹具：同回合清三只需要三下（或一发 AoE）
    zkilled = 0
    for _ in range(6):
        before = sum(1 for m in bz.monsters if int(m.cur_hp) > 0)
        if before == 0 or bz.is_battle_over():
            break
        for a in bz.enumerate_actions():
            c = bz.clone()
            a.execute(c)
            after = sum(1 for m in c.monsters if int(m.cur_hp) > 0)
            if after < before:                # AoE 一发可能同时打死多只（那正是这条要测的路径）
                bz = c
                zkilled += before - after
                break
        else:
            break
    check(f"同回合内打死暗灵 {zkilled} 只", zkilled == 3, zkilled)
    check("⭐ 三只都死后必须是 PLAYER_VICTORY（不许变成打不死）",
          int(bz.outcome) == int(sts.Outcome.PLAYER_VICTORY), int(bz.outcome))

    print(f"\n{OK} 通过 / {FAIL} 失败")
    raise SystemExit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
