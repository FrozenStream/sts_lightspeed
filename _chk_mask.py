"""掩码语义检查：`m_act` 到底是「真实合法动作」还是「搜索树访问过的边」？

缓存里 **23.5% 的样本只有 1 个合法动作**。
`azaction.teacher_action` 里掩码是从 `st.policy`（**UCT 搜索树的边**）里 `n > 0` 的那些下标
构造的，而**不是** `b.enumerate_actions()`。若两者不等，那么：

  · 掩码是真实合法集的**真子集** ⇒ 策略头**永远学不到**没被探过的动作
    （与 `action_to_idx == -1 被静默丢弃` 是**不同**的机制，那道守卫抓不到它）；
  · 「只 1 个合法动作」= 搜索只探了 1 个 ⇒ 该样本对策略头**零信息**
    （标签被掩码逼成 one-hot，而不是因为真的只有一个选择）。

本脚本直接对拍二者，给出联合分布。

用法：
    python _chk_mask.py [局数] [sims]
"""
import random
import sys
from collections import Counter

import numpy as np
import slaythespire as sts

import azaction as A
from collect import make_stage_battle

# 场景：act 1 精英房（三哨兵）—— 多怪才能把目标掩码测到位。
SCENE = (sts.MonsterEncounter.THREE_SENTRIES, 12)

N = int(sys.argv[1]) if len(sys.argv) > 1 else 5
SIMS = int(sys.argv[2]) if len(sys.argv) > 2 else 500


def main():
    joint = Counter()          # (真实合法数, 掩码大小)
    is_sel_ct = Counter()
    endturn_missing = 0
    subset_ok = True
    n_sel_cov = 0

    for seed in range(N):
        _, b, _ = make_stage_battle(seed, *SCENE)
        for _ in range(3000):
            if b.is_battle_over():
                break
            legal = b.enumerate_actions()
            n_legal = len(legal)
            idx = {b.action_to_idx(a) for a in legal}
            got = A.teacher_action(b, SIMS, random.Random(0))
            if got is None:
                break
            pi, mask, is_sel, act, _wr = got
            mset = set(np.nonzero(mask)[0].tolist())
            if is_sel:
                is_sel_ct["select 帧"] += 1
                n_sel_cov += 1
            else:
                is_sel_ct["动作帧"] += 1
                joint[(n_legal, len(mset))] += 1
                if not mset <= idx:
                    subset_ok = False
                if A.END_TURN_IDX in idx and A.END_TURN_IDX not in mset:
                    endturn_missing += 1
            act.execute(b)

    tot = sum(joint.values())
    print(f"{N} 局 / {SIMS} sims   帧数：{dict(is_sel_ct)}")
    print(f"  动作帧 {tot} 个")
    if not tot:
        return 1
    print(f"  掩码 ⊆ 真实合法集 ？ {subset_ok}"
          f"   {'✓（只是子集，不是错误）' if subset_ok else '✗ 掩码里有非法下标！'}")
    print(f"  END_TURN 合法但未被掩码覆盖的次数：{endturn_missing} "
          f"（占动作帧 {endturn_missing / tot:.1%}）")

    nl = Counter(k[0] for k in joint.elements())
    ms = Counter(k[1] for k in joint.elements())
    mean = lambda c: sum(k * v for k, v in c.items()) / max(sum(c.values()), 1)  # noqa: E731
    print(f"\n  真实合法动作数：均值 {mean(nl):.2f}   分布 {sorted(nl.items())[:8]}")
    print(f"  掩码大小      ：均值 {mean(ms):.2f}   分布 {sorted(ms.items())[:8]}")

    print(f"\n  联合分布（真实合法数, 掩码大小）→ 次数   [只看掩码 == 1 的行]")
    for (a, bb), v in sorted(joint.items(), key=lambda kv: -kv[1])[:14]:
        flag = "  ← 只探了 1 个" if bb == 1 else ""
        print(f"    ({a}, {bb}) : {v:6d}  ({v / tot:5.1%}){flag}")

    ones = sum(v for (a, bb), v in joint.items() if bb == 1 and a > 1)
    print(f"\n  ⚠️ 掩码==1 但**真实合法 >= 2** 的帧：{ones} / {tot} = {ones / tot:.1%}")
    print("    这些样本的标签被掩码逼成 one-hot，对策略头**零信息**。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
