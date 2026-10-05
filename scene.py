"""
按层造发育状态：`shape_floor` 一族，按 `devshape` 的 6 段阶梯缩放。
"""
import random
import slaythespire as sts
import devshape

CHARACTER = sts.CharacterClass.IRONCLAD
ASCENSION_RANGE = (0, 20)


def ascension_of(seed: int) -> int:
    return random.Random(seed * 15485863 + 23).randint(*ASCENSION_RANGE)


def build_red_pools() -> dict[sts.CardRarity, list[sts.CardId]]:
    pools = {sts.CardRarity.COMMON: [],
             sts.CardRarity.UNCOMMON: [],
             sts.CardRarity.RARE: [],
             sts.CardRarity.CURSE: []}
    for _, card_id in sts.CardId.__members__.items():
        card = sts.Card(card_id)
        if card.rarity == sts.CardRarity.CURSE:
            pools[sts.CardRarity.CURSE].append(card_id)
        elif card.rarity in pools and sts.get_card_color(card_id) == sts.CardColor.RED:
            pools[card.rarity].append(card_id)
    return pools


RED_POOLS = build_red_pools()
TRANSFORM_RED_POOL = (RED_POOLS[sts.CardRarity.COMMON] + RED_POOLS[sts.CardRarity.UNCOMMON] + RED_POOLS[sts.CardRarity.RARE])

UNSUPPORTED_POTIONS = (sts.Potion.ELIXIR_POTION, sts.Potion.GAMBLERS_BREW)      # 多选动作不支持
POTION_GENERATORS = (sts.Potion.ENTROPIC_BREW,)                                 # 生成药水会随机出不支持的药水
CARD_INJECTORS = (sts.Potion.COLORLESS_POTION,)                                 # 会生成无色卡牌的药水（当前仅支持红色卡牌）

BANNED_POTIONS = UNSUPPORTED_POTIONS + POTION_GENERATORS + CARD_INJECTORS

# 不进发放池的遗物（抽到就退掉重抽 —— 只在**没有拾取副作用**的遗物上成立，加成员前先确认）。
# 两件的性质**不同**：
#  · `GAMBLING_CHIP`（RARE）回合开始强制进**弃牌多选屏**（`CardSelectTask::GAMBLE`）——
#    引擎只枚举出 `MULTI_CARD_SELECT`，统一下标里没有它（`actionToIdx` 当场抛）⇒ 采集整项作废。
#  · `TOOLBOX`（SHOP）开局三选一无色牌。⚠️ **这一屏编码得出来**（单张选牌，实测 2026-10-02），
#    挡它只是为了坐住「开局观测必须是 PLAYER_NORMAL」这条口径。要放开就把它从这张表里删掉，
#    同时把 `_chk_shape` ⑥ 放宽成「开局若在选牌屏则必须能编码全部动作」。
BANNED_RELICS = (sts.RelicId.GAMBLING_CHIP, sts.RelicId.TOOLBOX)

# 可进牌组的**特殊牌**（`CardRarity::SPECIAL`）—— 这一档是**手挑**的，推不出来：其余 SPECIAL
# 卡是别角色的衍生牌（Insight / Smite / Shiv / …），诅咒色那 3 张由对应遗物的拾取效果带
# （而引擎的 `obtainRelic` 不执行拾取效果 ⇒ 实际拿不到）。
# **并进 `RED_POOLS`**（名字里的「红」对诅咒那一档本来就已不成立）⇒ 发牌那条循环五档一致。
SPECIAL_POOL = (sts.CardId.APPARITION, sts.CardId.BITE,
                sts.CardId.RITUAL_DAGGER, sts.CardId.JAX)
RED_POOLS[sts.CardRarity.SPECIAL] = list(SPECIAL_POOL)

# 发牌档位顺序 == `devshape.DECK` 的列序（长度不符就当场崩，不静默丢一列）。
DECK_TIERS = (sts.CardRarity.COMMON, sts.CardRarity.UNCOMMON, sts.CardRarity.RARE,
              sts.CardRarity.CURSE, sts.CardRarity.SPECIAL)

# 可发放的**特殊遗物**（tier = SPECIAL）。
# ⚠️ **不能用 `obtain_random_relic(RelicTier.SPECIAL)`** —— 引擎那张池没内容，那条路会
#    **段错误**（不是抛异常，实测 2026-10-02），所以这里按 id 显式发。
# 排除三类（都不是事件遗物）：`NEOWS_LAMENT`（涅奥起点专用）、`CIRCLET` / `RED_CIRCLET`
# （奖池耗尽时的填充件，见 `GameContext.cpp` 的 `returnRandomRelic` 兑底分支）。
NOT_EVENT_SPECIAL_RELICS = ("NEOWS_LAMENT", "CIRCLET", "RED_CIRCLET")
SPECIAL_RELIC_POOL = tuple(
    v for n, v in sts.RelicId.__members__.items()
    if sts.relic_tier(v) == sts.RelicTier.SPECIAL and n not in NOT_EVENT_SPECIAL_RELICS)                                    # 多选动作不支持

# ── 按层造状态（`collect.make_stage_battle` 的实现）──────────
# 表在 `devshape`（6 段阶梯 = 每幕上/下半，act4 归第 6 段）。
# 入场随机化：hp = uniform(HP_FRAC_RANGE) × max_hp，金币 0..400。

HP_FRAC_RANGE = (0.35, 0.95)

BANE_ID = int(sts.CardId.ASCENDERS_BANE)


def build_shaped_deck(gc: sts.GameContext, seed: int) -> None:
    """
    按 `devshape` 的 6 段阶梯发牌/升级/删牌
    """
    rng = random.Random(seed * 7919 + 13)
    for _ in range(devshape.how_many_to_remove(gc.floor_num, rng)):
        cand = [i for i, c in enumerate(gc.deck) if int(c.id) != BANE_ID]
        if len(cand) > 0:
            gc.remove_card(rng.choice(cand))
    # 追加牌：五档各自抽（顺序同 `devshape.DECK` 的列序）
    counts = devshape.how_many_to_add(gc.floor_num)
    if len(counts) != len(DECK_TIERS):
        raise ValueError(f"devshape.DECK 有 {len(counts)} 列，这里认 {len(DECK_TIERS)} 档")
    for rar, cnt in zip(DECK_TIERS, counts):
        for _ in range(devshape.draw_poisson(cnt, rng)):
            gc.obtain_card(sts.Card(rng.choice(RED_POOLS[rar])))
    # 升级卡牌
    n_up = devshape.how_many_to_upgrade(gc.floor_num, rng)
    gc.upgrade_random_cards(n_up)


def _obtain_tier_relic(gc: sts.GameContext, tier: sts.RelicTier) -> None:
    """抽一件该档的遗物；抽到 `BANNED_RELICS` 就退掉重抽（同旧掉落路径的处理）。

    ⚠️ `obtain_random_relic` 只返回 bool，要知道抽到哪件只能比 `gc.relics` 的前后差。
    """
    for _ in range(64):
        before = {r.id for r in gc.relics}
        gc.obtain_random_relic(tier)
        new = [r.id for r in gc.relics if r.id not in before]
        if not new or new[0] not in BANNED_RELICS:
            return
        gc.remove_relic(new[0])
    raise RuntimeError(f"{tier} 档抽不出非禁用遗物")


def grant_shaped_relics(gc: sts.GameContext, seed: int) -> None:
    """按 devshape 的分档表发遗物：**每个档各自**抽件数（档位不再交给引擎的掉落表）。

    初始 Burning Blood 由 `reset_run_resources()` 给，不在这里发；特殊（事件）档按 id 发
    —— `obtain_random_relic(SPECIAL)` 会段错误（见 `SPECIAL_RELIC_POOL`）。
    """
    rng = random.Random(seed * 104729 + 7)
    for tier in ("common", "uncommon", "rare", "boss", "shop"):
        n = devshape.draw_poisson(devshape.relics_of(tier, gc.floor_num), rng)
        for _ in range(n):
            _obtain_tier_relic(gc, getattr(sts.RelicTier, tier.upper()))
    n = devshape.draw_poisson(devshape.relics_of("special", gc.floor_num), rng)
    for _ in range(n):
        gc.obtain_relic(SPECIAL_RELIC_POOL[rng.randrange(len(SPECIAL_RELIC_POOL))])


def grant_shaped_potions(gc: sts.GameContext, seed: int) -> None:
    """发 0-3 瓶药水 """
    rng = random.Random(seed * 224737 + 17)
    pool = [x for x in sts.potion_pool(CHARACTER) if x not in BANNED_POTIONS]
    for pid in rng.sample(pool, rng.randint(0, 3)):
        gc.obtain_potion(pid)


def shape_floor(gc: sts.GameContext, seed: int) -> sts.GameContext:
    """
    将 牌组/升级/遗物/药水/hp/金币 随机生成为当前层的期望形状
    - seed: 随机种子
    """
    gc.reset_run_resources()          # 牌组/遗物/药水 + 遗物池 + max_hp 回基础
    build_shaped_deck(gc, seed)
    grant_shaped_relics(gc, seed)
    grant_shaped_potions(gc, seed)
    st = random.Random(seed * 65537 + 29)
    gc.cur_hp = max(1, round(gc.max_hp * st.uniform(HP_FRAC_RANGE[0], HP_FRAC_RANGE[1])))
    gc.gold = st.randint(0, 400)
    return gc


ACT_EDGES = (17, 34, 51)


def act_of(floor: int) -> int:
    return 1 + sum(1 for e in ACT_EDGES if floor >= e)
