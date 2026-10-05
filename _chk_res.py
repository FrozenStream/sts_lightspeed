"""资源块守卫：遗物=多热池化、药水=**逐槽 id 列表**，以及药水**动作**的因子化网格。

## 为什么单独一个守卫

这三件事各自都不会报错，只会**静默**变错：

1. **布局**：`potionSlotOff` 那 5 列如果没写、或哨兵没映成 0，快照长度照样对，
   但"槽里是哪一瓶"全是垃圾 ⇒ 下面第 3 条那个别名问题会原封不动地回来。
2. **哪几列进网络**：多热/逐槽块该走查表而不是进标量段。漏排除就是几十维死列
   （旧的袋子多热就是这么躺了几天没人发现）；多排除就是把信息扔了。
3. **槽位可分辨**（本轮的核心）：动作是**槽位寻址**（`slot*7+pt`），
   所以"槽 0 是 A、槽 1 是 B"与"槽 0 是 B、槽 1 是 A"必须是**两个不同的输入**。
   以前观测只有袋子多热 ⇒ 这两者逐位相同，而动作后果不同 ⇒ 网络学不到"喝哪一瓶"。
   这条断言就是那件事的**唯一**证据来源。

⚠️ 反向也要钉：**药水只有一个存储（逐槽 id）** ⇒ 再拿 `potion_bag_off` 取块就是错。

用法：`python _chk_res.py`
"""
import random
import sys

import numpy as np
import slaythespire as sts
import torch

import azaction as AA
import azlayout as AL
import aznet

for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

OK = FAIL = 0


def check(name, cond, extra=""):
    global OK, FAIL
    if cond:
        OK += 1
        print(f"  ✓ {name}")
    else:
        FAIL += 1
        print(f"  ✗ {name} {extra}")


def main():
    L = AL.LAYOUT

    # ---- 1. 布局：逐槽块存在、落在末尾、且真的写了 id ----------------------
    print("[布局]")
    check("快照长度 == 药水逐槽块末（块间无洞、无尾余）",
           L["len"] == L["potion_slot_off"] + L["potion_slots"],
           (L["len"], L["potion_slot_off"], L["potion_slots"]))
    check("逐槽块紧跟在 [potionCount, potionCapacity] 后面",
          L["potion_slot_off"] == L["potion_meta_off"] + L["potion_meta_len"])
    check("槽数 == 5（POTION_BELT 上限）", L["potion_slots"] == 5, L["potion_slots"])
    check("⭐ 总牌组块紧跟在三个牌堆后面，宽度 == 引擎卡组上限",
          L["deck_off"] == L["exhaust_off"] + L["pile_slots"] * L["pile_fields"]
          and L["monster_off"] == L["deck_off"] + L["deck_slots"] * L["deck_fields"],
          (L["deck_off"], L["deck_slots"], L["monster_off"]))
    # `runCtx` 只有 [floor, gold]，后面**直接**是遗物块。
    check("⭐ run 上下文块紧跟在 select_meta 后面、宽度 2，后接遗物块",
          L["run_ctx_len"] == 2
          and L["relic_off"] == L["run_ctx_off"] + L["run_ctx_len"],
          (L["run_ctx_off"], L["run_ctx_len"], L["relic_off"]))

    gc, b, _ = _make_battle()
    s = np.asarray(b.get_snapshot_np(), dtype=np.float64)
    check("快照长度与布局一致", s.shape[0] == L["len"], (s.shape[0], L["len"]))
    # ⭐ 总牌组块 == `gc.deck`（**集合口径**：两个方向都按排序后的非零槽比对）；
    #    它与三个牌堆的区别就在这里：**排除战斗造出来的临时牌/状态牌**、且不随战斗漂移。
    want = sorted(int(c.id) * 2 + (1 if c.upgraded else 0) for c in gc.deck)
    got = sorted(int(x) for x in s[L["deck_off"]:L["deck_off"] + L["deck_slots"]] if x > 0)
    check("⭐ 总牌组块 == `gc.deck`（逐卡编码 id*2+upgraded）", got == want,
          (len(got), len(want)))
    s0_deck = s[L["deck_off"]:L["deck_off"] + L["deck_slots"]].copy()
    for _ in range(4):                      # 推进几步：牌堆会动，牌组必须**逐位不变**
        if b.is_battle_over():
            break
        a = aznet.teacher_action(b, 50, random.Random(0))
        if a is None:
            break
        a[3].execute(b)
    s1 = np.asarray(b.get_snapshot_np(), dtype=np.float64)
    check("⭐ 推进若干步后总牌组块逐位不变（临时/状态牌只进牌堆）",
          bool(np.array_equal(s0_deck, s1[L["deck_off"]:L["deck_off"] + L["deck_slots"]])))
    # ⭐ run 上下文块 == gc 的 floor/gold（act / isElite / isBoss 不进快照）。
    # 快照已归一化 ⇒ 乘回除数再与引擎比（除数写在 `SnapWriter` 的赋值处：floor ÷60、gold ÷999）。
    check("⭐ run 上下文：floor/gold == gc 的两个字段",
          (round(float(s[AL.RUN_CTX_OFF]) * 60), round(float(s[AL.RUN_CTX_OFF + 1]) * 999))
          == (int(gc.floor_num), int(gc.gold)),
          (s[AL.RUN_CTX_OFF:AL.RUN_CTX_OFF + AL.RUN_CTX_LEN].tolist(),
           int(gc.floor_num), int(gc.gold)))
    slots = s[L["potion_slot_off"]:L["potion_slot_off"] + L["potion_slots"]]
    check("逐槽块的取值都是合法药水 id（0..43）",
          bool(((slots >= 0) & (slots < AL.POTION_TYPES)).all()), slots)
    nz = int((slots > 0).sum())
    # potionCount 已归一化（÷`potion_slots`，除数与 C++ 的 `Snap::potionSlots` 同源）⇒ 乘回来比。
    check("非零槽数 == potionCount",
          nz == round(float(s[L["potion_meta_off"]]) * L["potion_slots"]),
          (nz, float(s[L["potion_meta_off"]])))
    # ⭐ 跨生产者：快照（战斗侧 `bc.potions`）== `gc.potions`（run 侧）。袋子块删掉之后，
    #    这是逐槽块**唯一**的独立核对（空槽哨兵 INVALID / EMPTY_POTION_SLOT 均映成 0）。
    _empty = {int(sts.Potion.INVALID), int(sts.Potion.EMPTY_POTION_SLOT)}
    want_pots = [0 if int(p) in _empty else int(p) for p in gc.potions]
    check("⭐ 逐槽块 == `gc.potions`（逐槽逐位，空槽映 0）",
          slots.tolist() == want_pots, (slots.tolist(), want_pots))

    # ---- 2. 哪几列进网络 --------------------------------------------------
    print("[进不进网络]")
    check("遗物 / 逐槽 两类都不在标量段",
          not (AL.SKIP_IDX & set(AL.SCALAR_IDX)), len(AL.SKIP_IDX & set(AL.SCALAR_IDX)))
    check("SKIP 覆盖：181 遗物 + 5 槽",
          len(AL.SKIP_IDX) == L["relic_len"] + L["potion_slots"],
          len(AL.SKIP_IDX))
    check("[count, capacity] 两个真标量**留下**",
          L["potion_meta_off"] in AL.SCALAR_IDX and L["potion_meta_off"] + 1 in AL.SCALAR_IDX)

    # ---- 3. 网格与动作空间对齐 -------------------------------------------
    print("[药水动作的因子化网格]")
    check("手牌段 + 药水段 == END_TURN 下标（段宽全部来自 C++）",
          AL.HAND_SLOTS * AA.HAND_STRIDE + AL.POTION_SLOTS * AA.POTION_STRIDE == AA.END_TURN_IDX,
          AA.END_TURN_IDX)
    n = aznet.AZNet(hidden=64, dropout=0.0, arch="factor")
    check("药水段从扁平头里搬走了（只剩 END_TURN）",
          n.policy_head.end_head.out_features == 1, n.policy_head.end_head.out_features)
    check("药水槽 key 是从**同一张**状态表查出来的",
          n.policy_head.k_potion.in_features == n.potion_embed.embedding_dim,
          (n.policy_head.k_potion.in_features, n.potion_embed.embedding_dim))
    check("遗物块 = 不带 bias 的 `Linear(RELIC_TYPES, relic_rank)`（多热的列和）",
          (n.relic_linear.in_features, n.relic_linear.out_features, n.relic_linear.bias)
          == (AL.RELIC_TYPES, aznet.RELIC_RANK, None),)
    check("`discard_target` 存在（pt=6「丢弃」不依赖怪物）",
          n.policy_head.discard_target is not None)

    # ---- 4. ⭐ 核心：槽位必须可分辨；只有一个药水存储 ---------------------
    print("[槽位可分辨 / 唯一存储]")
    n.eval()
    def obs_with(slots_ids):
        o = torch.zeros(1, AL.OBS_DIM)
        for k, v in enumerate(slots_ids):
            o[0, AL.POTION_SLOT_IDX[k]] = v
        return o

    with torch.no_grad():
        a = obs_with([7, 9, 0, 0, 0])          # 槽0=7号, 槽1=9号
        c = obs_with([9, 7, 0, 0, 0])          # 交换
        ia, ic = n._inputs(a), n._inputs(c)
    check("⭐ 交换两个槽里的药 ⇒ 输入必须不同（这是别名问题的解药）",
          not bool(torch.equal(ia, ic)),
          "交换后输入逐位相同 ⇒ 槽位信息没进网络")
    check("⭐ 同一个观测得到的药水段 logits 也必须不同",
          not bool(torch.equal(n(a)[0], n(c)[0])))
    with torch.no_grad():
        e = n._inputs(torch.zeros(1, AL.OBS_DIM))
    # v3：行 0 = 空槽、**可学**（与 `card_embed` 同款）⇒ 判据：空槽段 == 行 0 平铺
    # `槽数` 份，且行 0 确实在可学集合里。
    _r = n.potion_embed.embedding_dim
    check("全空槽 ⇒ 药水段 = `potion_embed.weight[0]` 平铺（行 0 参与、非恒零）",
          bool(torch.allclose(e[0, -AL.POTION_SLOTS * _r:],
                              n.potion_embed.weight[0].repeat(AL.POTION_SLOTS),
                              atol=1e-6)))
    check("药水 / 卡牌行 0 可学（`padding_idx is None`，与 `aznet` 的行 0 约定同款）",
          n.potion_embed.padding_idx is None and n.card_embed.padding_idx is None)

    print(f"\n{OK} 通过 / {FAIL} 失败")
    raise SystemExit(1 if FAIL else 0)


def _make_battle():
    """造一个**真的有药水**的战斗（否则逐槽块的取样全是 0，等于没测）。"""
    from collect import make_stage_battle
    for seed in range(200):
        gc, b, enc = make_stage_battle(seed, sts.MonsterEncounter.THREE_SENTRIES, 12)
        if int(gc.potion_count) > 0:
            return gc, b, enc
    raise SystemExit("✗ 前 200 个种子里没有一个带药水 —— 场景是不是变了？")


if __name__ == "__main__":
    main()
