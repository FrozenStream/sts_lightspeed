"""缓存有效性检查：这份 obs 是不是**当前二进制**采的。

为什么必须有：快照布局一变（例如 `MonsterId` 从独立块并进怪物块的 field 0），
**长度不变（770）**、键不变、`fp`/`space` 也不变 ⇒ 旧缓存会以错误语义**静默**喂进网络，
一个错都不报。时间戳只能给出「可疑」，给不出结论（`.so` 比 `.npz` 新只说明次序，
不说明内容）。

判据（决定性，不依赖任何历史或记忆）：缓存里**每场第 0 行 = 该场初始状态**，
必须与当前二进制在同一 seed 上 `get_snapshot_np()` **逐位相同**。

与 `z_revalue.py stats` 互补，两者管的不是一回事：
  - 本脚本：obs **布局**对不对
  - `stats` 的重放核对：`pi_*` 的**动作语义**对不对（重放出的终局列必须与缓存逐位相同）

用法: python _chk_cache.py <缓存.npz> [抽查场数=8]
      python _chk_cache.py selftest          # 守卫自检（合成缓存，不需真数据）
"""
import sys

import numpy as np

import slaythespire as sts
from aznet import OBS_DIM
from collect import block_of, check_cache_obs, make_stage_battle

L = sts.snapshot_layout()
N_PROBE = 10          # 抽查场数（默认值，argv[2] 可改）


def check(d, label):
    """核心判据。返回 0 = 通过，1 = 布局不符，2 = 缓存本身不完整。"""
    need = ("bid", "obs")
    miss = [k for k in need if k not in d]
    if miss:
        print(f"❌ 缓存缺少 {miss}（files={sorted(d)}）", file=sys.stderr)
        return 2
    bid, obs = np.asarray(d["bid"]), np.asarray(d["obs"])
    nb = int(bid.max()) + 1
    print(f"{label}")
    print(f"  样本 {len(bid)}  场 {nb}  动作空间 "
          f"{int(d['space']) if 'space' in d else '?'}  obs {obs.shape}")

    if obs.shape[1] != L["len"]:
        print(f"  ❌ obs 宽度 {obs.shape[1]} != 布局 {L['len']} ⇒ 长度就对不上，"
              f"这份缓存肯定是旧的。", file=sys.stderr)
        return 2

    # 每场第 0 行（缓存记的是「动作前状态」⇒ 第 0 行 = 初始状态）
    first = np.full(nb, -1, dtype=np.int64)
    for i, b in enumerate(bid):
        if first[b] < 0:
            first[b] = i
    if (first < 0).any():
        print(f"  ❌ 有 {int((first < 0).sum())} 场在缓存里一行都没有", file=sys.stderr)
        return 2

    miss = [k for k in ("seed", "enc", "enc_floor") if k not in d]
    if miss:
        print(f"  ❌ 缓存缺 {miss} ⇒ 无法重放场景（旧格式，需重新采集）", file=sys.stderr)
        return 2
    seed_col, enc_col, flr_col = d["seed"], d["enc"], d["enc_floor"]
    rng = np.random.default_rng(0)
    probe = rng.choice(nb, size=min(N_PROBE, nb), replace=False)
    bad = 0
    for b in probe:
        j = int(first[b])
        enc = sts.MonsterEncounter(int(enc_col[j]))
        # 场景必须由缓存自带的三列重放（`make_stage_battle` 是造环境的唯一入口）。
        _, bb, _ = make_stage_battle(int(seed_col[j]), enc, int(flr_col[j]))
        snap = bb.get_snapshot_np().astype(np.float32)
        row = obs[j].astype(np.float32)
        diff = np.flatnonzero(snap != row)
        bad += diff.size > 0
        extra = "" if diff.size == 0 else \
            f"  不同 {diff.size} 项  max|Δ|={np.abs(snap - row).max():.0f}"
        print(f"  {'✅' if diff.size == 0 else '❌'} 场 {int(b):>5} "
              f"(seed {int(seed_col[j]):>5} {enc.name}@{int(flr_col[j])}){extra}")
        if diff.size:
            print(f"      差异落在这些块：{sorted({block_of(int(i)) for i in diff})}")

    if bad:
        print(f"\n❌ {bad}/{len(probe)} 场不一致 ⇒ **这份缓存不是当前二进制采的**。"
              f"用错误布局训练不会报错，只会悄悄训出错的模型 —— 必须重采。", file=sys.stderr)
        return 1
    print(f"\n✅ {len(probe)}/{len(probe)} 场逐位一致 ⇒ obs 布局与当前二进制相符，缓存可用。")
    print("   （这只保证**布局**；动作语义由 `z_revalue.py rew` 的重放自检验证。）")
    return 0


def selftest():
    """守卫自检。三种情形，其中第三种才是**真正要防的那种**：

    ⚠️ 光测「交换两场的行」不够 —— 种子 0/1 是同怪不同卡组，差异落在
    `counts/draw/hand`，测的是「行错位」而不是「布局错位」。
    """
    import numpy as np

    snaps = []
    for b in range(2):
        _, bb, _ = make_stage_battle(b, sts.MonsterEncounter.THREE_SENTRIES, 12)
        snaps.append(bb.get_snapshot_np().astype(np.float32))
    good = np.stack(snaps)

    # 块内列语义漂移的**等价模拟**：把 field0 / field1 两个名义列互换 —— 列数、快照总长、
    # 键集合全不变，这正是「静默错配」的样子。
    old_sem = good.copy()
    mo, mf = L["monster_off"], L["monster_fields"]
    for k in range(L["monster_slots"]):
        o = mo + mf * k
        old_sem[:, o + 0], old_sem[:, o + 1] = good[:, o + 1], good[:, o + 0]

    # 合成缓存的列必须够 `check()` 的判据用：重放场景靠 `seed` + `enc` + `enc_floor` 三列。
    base = dict(bid=np.array([0, 1]), space=np.array(sts.BattleContext.unified_action_count),
                obs_dim=np.array(OBS_DIM), seed=np.array([0, 1]),
                enc=np.full(2, int(sts.MonsterEncounter.THREE_SENTRIES)),
                enc_floor=np.full(2, 12))
    cases = [("正确缓存", good, 0),
             ("交换两场的行（行错位）", good[::-1], 1),
             ("怪物块 field0/field1 互换（列语义漂移）", old_sem, 1)]
    fails = []
    for label, ob, want in cases:
        print(f"\n--- 自检：{label}（期望 rc={want}）---")
        rc = check(dict(base, obs=ob), f"<内存> {label}")
        # 同一份合成缓存也喂给**加载闸**（`collect.check_cache_obs`）：它的判据就是重放对账，
        # 所以必须与上面的守卫同判（三种情形都带 `seed`/`enc`/`enc_floor`，闸会真重建场景）。
        try:
            check_cache_obs(dict(base, obs=ob), f"<内存> {label}")
            gate = 0
        except RuntimeError as e:
            gate = 1
            print(f"     闸的判词：{e}")
        ok = rc == want and gate == want
        print(f"  ⇒ 守卫 rc={rc} 闸={'拒' if gate else '过'} "
              f"{'✅' if ok else '❌ 守卫/闸没按预期动作'}")
        if not ok:
            fails.append(label)
    print(f"\n自检{'通过' if not fails else '失败：' + str(fails)}")
    return 1 if fails else 0


def main():
    global N_PROBE
    if sys.argv[1:2] == ["selftest"]:
        return selftest()
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    if len(sys.argv) > 2:
        N_PROBE = int(sys.argv[2])
    return check(np.load(sys.argv[1]), sys.argv[1])


if __name__ == "__main__":
    sys.exit(main())

