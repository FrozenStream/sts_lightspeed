"""动作空间完整性检查：哪些遭遇战有**无法编码**进策略头的合法动作。

## 背景（2026-09-13 发现）

`sts::py::actionToIdx`（`bindings/bindings-util.cpp`） 里目标字段只有 5 个取值：

    const int t = c.requiresTarget() ? target + 1 : 0;   // monster 0..3 -> 1..4
    if (t < 0 || t > 5) { return -1; }
    return hand * 6 + t;

所以**第 5 只怪无法被指定为目标** —— `action_to_idx` 返回 -1，动作被**静默丢弃**。

✅ **2026-09-13 已修**：目标字段 4→5（无目标 + monster 0..4），索引 81→96。
（96 = 手牌 10×6 + 药水 **5**×7 + END_TURN。药水槽要按**最大**容量 5 算，
不是默认的 3 —— 漏算这个会得出 89，容量 5 时第 5 个槽越界，又是一次静默丢弃。
`aznet.py` 里有断言把这三个数字钉死。）
（另一个更隐蔽的形态：**药水**打第 5 只怪会编成 `pt=5`，而旧编码里 5 就是 `discard`
—— 它**不返回 -1**，而是把「用药水打它」静默记成「丢弃这瓶药水」。所以守卫
必须查**单射性**，只查范围是挡不住的。）
本脚本现在验的也是单射性（不同合法动作 -> 不同下标）。
网路策略头 `POLICY_DIM = 81` 用的是同一个 `hand*5+t` 编码，同样受限。

## 为什么这个坑极其隐蔽

两条执行路径的后果不对称：

  - `teacher_action`：只累积 `0 <= i < dim` 的边 → **教师永远打不到第 5 只怪**（现在不合格的编码**当场抛**，不再放行）
  - `random_action`：走 `b.enumerate_actions()`（真实 Action 对象，不经索引映射）→ **完全没事**

实测（`LOTS_OF_SLIMES` = 5 只史莱姆，200 局，seeds 1000..，配对同种子）：

    随机     83.0%
    教师500  24.0%     ← 比随机差 59pp，因为 1/5 的目标它根本选不了

**教训：「教师弱」不等于「场景难」。** 我一开始把 24% 读成了"这个场景比 Boss 还难"，
还据此搭了泛化测试场景 —— 全错。做场景对比前先跑这个脚本。

用法：python _chk_actions.py
"""
import sys

import numpy as np
import slaythespire as sts

from aznet import END_TURN_IDX, POLICY_DIM
from collect import make_stage_battle



def _bad_count(b) -> int:
    """有几个合法动作编不出唯一的下标（**哨兵自备**：运行时没有这道守卫）。

    编不出来时 C++ 直接抛 ⇒ 这里把它计成一个「坏」；下标越界/撞车也计一个。
    """
    seen: set[int] = set()
    bad = 0
    for a in b.enumerate_actions():
        try:
            i = b.action_to_idx(a)
        except Exception:
            bad += 1
            continue
        if 0 <= i < POLICY_DIM and i not in seen:
            seen.add(i)
        else:
            bad += 1
    return bad

# 场景：act 1 精英房（三哨兵）。层号只影响发牌/资源，不影响动作编码。
SCENE = (sts.MonsterEncounter.THREE_SENTRIES, 12)


def scan(seed=1000):
    """返回 [(名字, 无法编码的动作数, 合法动作总数, 怪物数)]。"""
    out = []
    for name in sorted(n for n in dir(sts.MonsterEncounter) if not n.startswith("_")):
        if name == "INVALID":
            continue        # 不是真遭遇战：`createMonsters` 会走到 default 分支硬崩
        # 逐项打点：某些遭遇战会让 C++ 侧硬崩（无 traceback），靠最后一行定位
        print(f"     scan {name}", file=sys.stderr, flush=True)
        enc = getattr(sts.MonsterEncounter, name)
        try:
            _, b, _ = make_stage_battle(seed, enc, SCENE[1])
            acts = b.enumerate_actions()
        except Exception as e:            # 抓 Python 层异常；C++ 硬崩靠上面的打点定位
            print(f"  ⚠️ {name}: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
            continue
        if not acts:
            continue
        bad = _bad_count(b)
        out.append((name, bad, len(acts), int(b.monster_count)))
    return out


def _mk(enc_name_):
    """按固定 seed 造一个遭遇战（自检共用）。"""
    _, b, _ = make_stage_battle(1000, getattr(sts.MonsterEncounter, enc_name_), SCENE[1])
    return b


def _fix_selftest(enc_name_="LOTS_OF_SLIMES"):
    """回归自检：**修好之后**，5 怪场景必须能干净编码，且 5 个目标都出现。

    历史上这个场景会让 `legal_actions_and_idx` 抛（那时是 `ActionEncodingError`）
    （第 5 只怪编不出下标）。所以「它现在不抛了」本身就是修复的证据；
    再顺手确认目标集合里真的有 monster idx=4。
    """
    from aznet import legal_actions_and_idx
    b = _mk(enc_name_)
    try:
        got = legal_actions_and_idx(b)
    except Exception as e:
        print(f"  ❌ 修复回归失败：{enc_name_} 仍然抛 {type(e).__name__}")
        return False
    tgts = {a.target_idx for a, _ in got if a.action_type == sts.ActionType.CARD}
    ok = 4 in tgts
    print(f"  {'✅' if ok else '❌'} 修复回归：{enc_name_} 编码成功（{len(got)} 个动作），"
          f"卡目标集合={sorted(tgts)}，含第 5 只怪(idx=4)={4 in tgts}")
    return ok



def _expand_selftest() -> bool:
    """`expand_children` 必须与「枚举 + 逐个 clone/execute/action_to_idx」**逐位等价**。

    MCTS 每步展开都走这个绑定（`azsearch.mcts_search` 的 ③）⇒ 它若与旧路径有差，搜索会在
    **没有任何报错**的情况下走到别的分支。按遭遇全扫一遍，比三项：下标序列、孩子快照、终局标记。
    """
    bad: list[str] = []
    for name in sorted(n for n in dir(sts.MonsterEncounter) if not n.startswith("_")):
        if name == "INVALID":
            continue
        try:
            _, b, _ = make_stage_battle(1000, getattr(sts.MonsterEncounter, name), SCENE[1])
            cands, kids = b.expand_children()
            manual = []
            for a in b.enumerate_actions():
                c = b.clone()
                a.execute(c)
                manual.append((b.action_to_idx(a), c))
        except Exception as e:            # 与 `scan` 同口径：个别遭遇战构造不出来就跳过
            print(f"  ⚠️ {name}: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
            continue
        same = ([i for i, _ in manual] == [i for i, _ in cands]
                and len(manual) == len(kids)
                and all(np.array_equal(c.get_snapshot_np(), k.get_snapshot_np())
                        for (_, c), k in zip(manual, kids))
                and all(c.is_battle_over() == k.is_battle_over()
                        for (_, c), k in zip(manual, kids)))
        if not same:
            bad.append(name)
    print(f"  {'❌' if bad else '✅'} `expand_children` 与「枚举 + 逐个 clone/execute/编码」逐位等价"
          f"（不一致 {len(bad)} 个：{bad[:5]}）")
    return not bad


def _batch_snapshot_selftest() -> bool:
    """`batch_snapshot` 必须与逐叶 `get_snapshot_np` + `np.stack` + astype **逐位等价**。

    批量前向的 obs 由它一次填出（`azsearch.mcts_search` 的 ②）；不一致就是静默换观测量。
    行序、形状、dtype（int32 → float32）都要对得上。
    """
    bad: list[str] = []
    for name in sorted(n for n in dir(sts.MonsterEncounter) if not n.startswith("_")):
        if name == "INVALID":
            continue
        try:
            _, b, _ = make_stage_battle(1000, getattr(sts.MonsterEncounter, name), SCENE[1])
            ctxs = [b] + [k for _, k in b.expand_children()]
            got = sts.batch_snapshot(ctxs)
            want = np.stack([c.get_snapshot_np() for c in ctxs]).astype(np.float32)
        except Exception as e:            # 与 `scan` 同口径：个别遭遇战构造不出来就跳过
            print(f"  ⚠️ {name}: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
            continue
        if got.dtype != np.float32 or got.shape != want.shape or not np.array_equal(got, want):
            bad.append(name)
    print(f"  {'❌' if bad else '✅'} `batch_snapshot` 与逐叶 `get_snapshot_np` 逐位等价"
          f"（不一致 {len(bad)} 个：{bad[:5]}）")
    return not bad


def main():
    print(f"策略头维度 POLICY_DIM = {POLICY_DIM}"
          f"（目标字段 0..5 = 无目标 + monster 0..4；END_TURN_IDX = {END_TURN_IDX}）\n")
    rows = scan()
    bad = [r for r in rows if r[1]]
    for name, n_bad, tot, n_mon in bad:
        print(f"  ❌ {name:26s} 怪数={n_mon}  {n_bad:3d}/{tot:3d} 个合法动作无法唯一编码")
    print(f"\n共扫描 {len(rows)} 个遭遇战，其中 {len(bad)} 个有编码失败。")
    if bad:
        print(f"失败遭遇战的最大怪数 = {max(r[3] for r in bad)}")
        print("这些遭遇战对**网络**和**教师**都不可玩（动作被丢弃，或标签被静默污染）。")
    else:
        print("✅ 所有遭遇战的动作都能**唯一编码**"
              "（含历史上中招的 LOTS_OF_SLIMES / REPTOMANCER）。")

    ok_fix = _fix_selftest()
    ok_exp = _expand_selftest()
    ok_snap = _batch_snapshot_selftest()
    return 0 if (ok_fix and ok_exp and ok_snap and not bad) else 2


if __name__ == "__main__":
    sys.exit(main())
