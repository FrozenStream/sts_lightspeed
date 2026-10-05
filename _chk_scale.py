"""归一化体检：直接看**已归一化**的缓存，判据只依赖产出数据。

为什么要它：除表现在写在 C++ 的赋值处（`SnapWriter::push<D>`，索引列走 `push_index`，
后者**根本没有除数参数**），Python 侧**没有除数表**可查 ⇒ 体检只能看产出：
  · **索引 / 名义列必须无小数部分**。这条直接抓「把查表下标也除了」：
    旧表就这么错过 —— `card_select` 的除数列表比字段少一个，`np.tile` 逐槽轮转，
    把 `cardId` 除以了 5、把 `costForTurn` 除以了 999。
  · **每个标量列的分布**（p50 / p99 / p99.9 / max）：
      `p99.9 ≥ 3`          ⇒ 该列量程不够（除数本身可疑）
      `max ≥ 10 × p99.9`   ⇒ 只有极少数行离群（通常是触到步数上限的场次，不是除数的问题）

⚠️ 不再自己重算除数。旧版的 `fields()` 假设「除数表长 == 块内字段数」，**与 bug 同源**
   ⇒ 它对那个轮转错位完全瞎（守卫和缺陷共享同一个前提 = 守卫看不见缺陷）。
   现在只依赖 `snapshot_layout()` 的块宽 + 产出数组本身。

缓存里存的是**已归一化**的值 ⇒ 改除数必须重采缓存。

用法: _chk_scale.py <缓存.npz> [离群倍数，默认 10]
"""
import sys

import numpy as np

from azlayout import CARD_ID_IDX, CAT_IDX, LAYOUT, POTION_SLOT_IDX, SCALAR_IDX

# Windows 控制台默认 GBK ⇒ 打印勾/叉得先换 UTF-8（同 `_chk_res.py` 等）。
for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")


def _entries():
    """产出 (标签, 列下标)；同一条目的各槽共用一个标签（除数相同、分布同源）。

    块宽 / 槽数一律取自 `snapshot_layout()` —— 布局的唯一权威。
    """
    L = LAYOUT

    def blk(off, width, n_items, names, tag):
        for f in range(width):
            yield (names[f] if names and f < len(names) else f"{tag}[{f}]",
                   np.arange(off + f, off + width * n_items, width))

    yield from blk(L["player_off"], 1, L["player_len"],
                   ["curHp", "maxHp", "block", "energy", "turn",
                    "inputState", "cardSelectTask"], "player")
    yield from blk(L["counts_off"], 1, L["counts_len"],
                   ["monsterCount", "monstersAlive", "handSize", "drawSize",
                    "discardSize", "exhaustSize", "deckSize"], "counts")
    yield from blk(L["hand_off"], L["hand_fields"], L["hand_slots"],
                   ["hand.cardId", "hand.costForTurn", "hand.specialData", "hand.playable"], "hand")
    yield from blk(L["monster_off"], L["monster_fields"], L["monster_slots"],
                   ["mon.id", "mon.intentMoveId", "mon.curHp", "mon.maxHp", "mon.block",
                    "mon.targetable", "mon.intentDmg", "mon.intentHits", "mon.miscInfo"], "mon")
    yield from blk(L["player_attr_off"], 1, L["player_attr_len"],
                   ["stance", "orbSlots", "energyPerTurn"], "attr")
    yield from blk(L["player_counter_off"], 1, L["player_counter_len"], None, "counter")
    yield from blk(L["player_status_off"], 1, L["player_status_len"], None, "pstatus")
    yield from blk(L["monster_status_off"], 1, L["monster_slots"] * L["monster_status_len"],
                   None, "mstatus")
    yield from blk(L["card_select_off"], L["card_select_fields"], L["card_select_slots"],
                   ["sel.cardId", "sel.cost", "sel.costForTurn", "sel.specialData"], "sel")
    yield from blk(L["select_meta_off"], 1, L["select_meta_len"],
                   ["candidateCount", "pickCount", "canPickZero", "canPickAnyNumber"], "meta")
    yield from blk(L["run_ctx_off"], 1, L["run_ctx_len"], ["floor", "gold"], "run")
    yield from blk(L["potion_meta_off"], 1, L["potion_meta_len"],
                   ["potionCount", "potionCapacity"], "potion")


def _check_index_cols(obs: np.ndarray) -> bool:
    """索引 / 名义列必须无小数部分 —— 它们是查表下标或 one-hot 行号，除一下就废。"""
    ok = True
    groups = [
        ("cardId / 药水槽 id（查表下标）",
         sorted(set(CARD_ID_IDX) | set(POTION_SLOT_IDX))),
        ("名义类别（one-hot 行号）", sorted(CAT_IDX)),
    ]
    for tag, cols in groups:
        ci = np.asarray(cols, dtype=int)
        frac = np.flatnonzero((np.abs(obs[:, ci] % 1) > 0).any(axis=0))
        if frac.size:
            ok = False
        print(f"  {'✅' if frac.size == 0 else '❌'} {tag}：{frac.size} / {ci.size} 列被除了")
        for j in frac[:5]:
            c = int(ci[int(j)])
            print(f"       列 {c}：{obs[:6, c].tolist()}")
    return ok


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    cache = sys.argv[1]
    ratio = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    d = dict(np.load(cache, allow_pickle=False))
    raw = np.asarray(d["obs"])
    obs = raw.astype(np.float64)
    print(f"{cache}  obs {obs.shape} {raw.dtype}\n")

    print("① 索引 / 名义列（除数被写错时这里必报）")
    ok = _check_index_cols(obs)

    stride = max(1, len(obs) // 60000)
    sub = obs[::stride]
    print(f"\n② 标量列分布（分位取自每 {stride} 行 1 行 = {len(sub)} 行；max 用全量 {len(obs)} 行）")
    is_scalar = np.zeros(obs.shape[1], bool)
    is_scalar[SCALAR_IDX] = True
    hdr = f"  {'字段':<26}{'p50':>9}{'p99':>9}{'p99.9':>10}{'max':>10}  判定"
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    rng_bad, out_bad, clean = [], [], 0
    for name, cols in _entries():
        ci = np.asarray(cols, dtype=int)
        if not is_scalar[ci].all():
            continue
        q = np.percentile(sub[:, ci].ravel(), [50, 99, 99.9])
        m = float(obs[:, ci].max())
        if q[2] >= 3.0:
            rng_bad.append(name)
            flag = "⚠️ 量程不够（除数本身可疑）"
        elif q[2] > 0 and m >= ratio * float(q[2]):
            # `q[2] > 0`：该列绝大多数帧都为 0 时（如只在选牌帧才有值的 sel.cost），
            # `max ≥ ratio × 0` 恒真 ⇒ 那是稀疏，不是离群。
            out_bad.append(name)
            flag = f"⚠️ 仅极少数行离群（≥ {ratio:g}×p99.9）"
        else:
            clean += 1
            flag = ""
        print(f"  {name:<26}{q[0]:>9.3f}{q[1]:>9.3f}{q[2]:>10.3f}{m:>10.3f}  {flag}")
    print("  " + "-" * (len(hdr) - 2))
    print(f"  标量字段 {clean + len(rng_bad) + len(out_bad)} 个：正常 {clean}、"
          f"量程不够 {len(rng_bad)}、仅离群 {len(out_bad)}")
    if rng_bad:
        print(f"❌ 除数量程不够：{', '.join(rng_bad)}")
    else:
        print("✅ 没有任何标量列的量程不够")
    if out_bad:
        print(f"ℹ️ 只有少量行离群（一般是数据污染，不是除数）：{', '.join(out_bad)}")
    if not ok:
        print("❌ 索引 / 名义列被除了 —— 查 `bindings/slaythespire.cpp` 的 `SnapWriter::push_index`")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
