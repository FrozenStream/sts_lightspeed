//
// 战斗快照（= 网络输入向量）—— 布局与生产者**同住一个文件**，从 `bindings/slaythespire.cpp`
// 搬出来（2026-10-10）。
//
// 为什么要搬：`Snap` 原来只住在宿主 TU，于是 `selfplay` / `teacher` 想要观测就只能
// **从 Python 注入** `write_obs`。搬到 `collect/` 后，采集侧自己就能算观测 ⇒ 那层注入
// 2026-10-14 撤掉（`registerSelfplay(m)` 不再收回调，观测由 `snapshotObs` 直接取）。
//
// ⚠️ **布局改动 = 作废全部缓存**：`snapshot_len` / 每块偏移都进了 `snapshot_layout()`，
//    Python 侧一律从那里推导下标、**不硬编码**。改这里必须同时重定基线（`_chk_res.py` /
//    `_chk_shape.py` / `_chk_actions.py` 都读布局）。
// ⚠️ 除数是**逐字段写在赋值处**的（`s.push<上限>(v)`）。守卫要"乘回除数"时必须照抄那个上限
//    （例：run 上下文的 floor 是 `push<50>`，不是 60 —— 曾因此误判过一次失败）。
//
// 与 pybind **无关**：本文件只依赖引擎类型，纯 C++。
//
#ifndef STS_LIGHTSPEED_SNAPSHOT_H
#define STS_LIGHTSPEED_SNAPSHOT_H

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "combat/BattleContext.h"
#include "combat/Monster.h"
#include "combat/MonsterGroup.h"
#include "combat/Player.h"
#include "constants/Cards.h"
#include "constants/Relics.h"
#include "constants/Potions.h"
#include "sim/search/Action.h" // search::Action（卡牌选择的合法性）

namespace sts::collect
{

    inline constexpr int maxMonsters = 5;
    inline constexpr int handSlots = 10;
    inline constexpr int potionSlots = 5;

    struct Snap
    {
        // ── player: [curHp, maxHp, block, energy, turn, inputState, cardSelectTask]
        static constexpr int playerLen = 7;
        static constexpr int playerOff = 0;

        // ── counts: [monsterCount, monstersAlive, handSize, drawSize, discardSize, exhaustSize, deckSize]
        static constexpr int countsLen = 7;
        static constexpr int countsOff = playerOff + playerLen;

        // ── hand: [cardId, costForTurn, specialData, playable]
        static constexpr int handSlots = sts::collect::handSlots;
        static constexpr int handFields = 4;
        static constexpr int handOff = countsOff + countsLen;

        // ── draw discard exhaust: 128 * [cardId]
        static constexpr int pileSlots = 128;
        static constexpr int pileFields = 1;
        static constexpr int drawOff = handOff + handSlots * handFields;
        static constexpr int discardOff = drawOff + pileSlots * pileFields;
        static constexpr int exhaustOff = discardOff + pileSlots * pileFields;

        // ── deck: Deck::MAX_SIZE * [cardId]
        static constexpr int deckSlots = Deck::MAX_SIZE;
        static constexpr int deckFields = 1;
        static constexpr int deckOff = exhaustOff + pileSlots * pileFields;

        // ── monster:
        // [MonsterId, intentMoveId, curHp, maxHp, block,
        //  targetable, intentDamage, intentHits, miscInfo]
        static constexpr int monsterSlots = maxMonsters;
        static constexpr int monsterFields = 9;
        static constexpr int monsterOff = deckOff + deckSlots * deckFields;

        // ── player: [stance, orbSlots, energyPerTurn]
        static constexpr int playerAttrLen = 3;
        static constexpr int playerAttrOff = monsterOff + monsterSlots * monsterFields;

        // ── player counter: [cardsPlayedThisTurn, attacksPlayedThisTurn,  skillsPlayedThisTurn,
        //                     cardsDiscardedThisTurn, cardDrawPerTurn, timesDamagedThisCombat]
        // —— relic counter: [penNib, happyFlower, incenseBurner, inkBottle,
        //                    nunchaku, sundial, orangePelletsCardTypesPlayed]
        static constexpr int playerCounterLen = 13;
        static constexpr int playerCounterOff = playerAttrOff + playerAttrLen;

        // ── player status
        static constexpr int playerStatusLen = static_cast<int>(PlayerStatus::THE_BOMB) + 1;
        static constexpr int playerStatusOff = playerCounterOff + playerCounterLen;

        // ── monster status: 5 * monsterStatusLen
        static constexpr int monsterStatusLen = static_cast<int>(MonsterStatus::INVALID) + 1;
        static constexpr int monsterStatusOff = playerStatusOff + playerStatusLen;

        // ── card select candicate: pileSlots * 4：[cardId, cost, costForTurn, specialData]，
        static constexpr int cardSelectSlots = pileSlots;
        static constexpr int cardSelectFields = 4;
        static constexpr int cardSelectOff = monsterStatusOff + monsterSlots * monsterStatusLen;

        // ── select meta: [candidateCount, pickCount, canPickZero, canPickAnyNumber]
        static constexpr int selectMetaLen = 4;
        static constexpr int selectMetaOff = cardSelectOff + cardSelectSlots * cardSelectFields;

        // ── [floor, gold]
        static constexpr int runCtxLen = 2;
        static constexpr int runCtxOff = selectMetaOff + selectMetaLen;

        // ── relic
        static constexpr int relicLen = static_cast<int>(RelicId::INVALID) + 1;
        static constexpr int relicOff = runCtxOff + runCtxLen;

        // ── relic meta: [potionCount, potionCapacity]
        static constexpr int potionMetaLen = 2;
        static constexpr int potionOff = relicOff + relicLen;

        // ── potion
        static constexpr int potionSlots = sts::collect::potionSlots;
        static constexpr int potionSlotLen = potionSlots;
        static constexpr int potionSlotOff = potionOff + potionMetaLen;

        // FULL SIZE
        static constexpr int snapshotLen = potionSlotOff + potionSlotLen;

        static constexpr int encodedCardIdTypes = 2 * sts::cardCount;
        static constexpr int monsterIdTypes = static_cast<int>(MonsterId::WRITHING_MASS) + 1;
        static constexpr int monsterMoveIdTypes = static_cast<int>(MonsterMoveId::WRITHING_MASS_STRONG_STRIKE) + 1;
        static constexpr int inputStateTypes = static_cast<int>(InputState::SELECT_CONFUSED_CARD_COST) + 1;
        static constexpr int cardSelectTaskTypes = static_cast<int>(CardSelectTask::WARCRY) + 1;
        static constexpr int stanceTypes = static_cast<int>(Stance::DIVINITY) + 1;
        static constexpr int potionTypes = sts::potionEnumCount;
        static constexpr int orangePelletsTypes = 8;
    };

    namespace
    {

        struct SnapWriter
        {
            // 数值列：写本字段的除数。**无默认值** ⇒ 每个调用点都必须写明，漏写编译不过。
            template <int Divisor>
            void push(int v)
            {
                assert(n < Snap::snapshotLen);
                buf[n++] = static_cast<float>(v) / static_cast<float>(Divisor);
            }

            // 名义类别 / 索引列：原样输出，供查表（嵌入）或 one-hot。**不给除数参数**。
            void push_index(int v)
            {
                assert(n < Snap::snapshotLen);
                buf[n++] = static_cast<float>(v);
            }

            void push_zeros(int k)
            {
                for (int i = 0; i < k; ++i)
                    push_index(0);
            }

            std::array<float, Snap::snapshotLen> buf{};
            int n = 0;
        };

        // 卡牌选择候选块 + 元信息块。槽位 k == `select_card(k)`（容器下标序，与牌堆块的堆顶序相反）。
        inline void appendCardSelectCandidates(const BattleContext &bc, SnapWriter &s)
        {
            const auto &info = bc.cardSelectInfo;
            int n = 0;

            // 合法性直接问引擎。
            const auto legalAt = [&bc](int k)
            {
                return search::Action(search::ActionType::SINGLE_CARD_SELECT, k).isValidAction(bc);
            };

            // 逐字段语义与手牌块同形：cardId 走索引，费用 / 本回合费用 ÷5，specialData ÷999。
            const auto emitInstance = [&](int k, const CardInstance &c)
            {
                if (legalAt(k))
                {
                    s.push_index(static_cast<int>(c.id) * 2 + (c.upgraded ? 1 : 0));
                    s.push<5>(c.cost);
                    s.push<5>(c.costForTurn);
                    s.push<999>(c.specialData);
                }
                else
                {
                    s.push_zeros(Snap::cardSelectFields); // 非法 ⇒ 空槽
                }
                ++n;
            };
            const auto emitRange = [&](const auto &begin, const auto &end)
            {
                for (auto it = begin; it != end && n < Snap::cardSelectSlots; ++it)
                {
                    emitInstance(static_cast<int>(it - begin), *it); // k = 容器下标
                }
            };

            switch (info.cardSelectTask)
            {
            case CardSelectTask::ARMAMENTS:
            case CardSelectTask::DUAL_WIELD:
            case CardSelectTask::EXHAUST_ONE:
            case CardSelectTask::FORETHOUGHT:
            case CardSelectTask::WARCRY:
                emitRange(bc.cards.hand.begin(), bc.cards.hand.begin() + bc.cards.cardsInHand);
                break;

            case CardSelectTask::CODEX:
            case CardSelectTask::DISCOVERY:
                // 3 张是生成的，不在任何牌堆里。CODEX 的 k=3（跳过）无卡，故不铺槽。
                for (int i = 0; i < 3; ++i)
                {
                    const CardInstance tmp(info.cards[i], false);
                    emitInstance(i, tmp);
                }
                break;

            case CardSelectTask::EXHUME:
                emitRange(bc.cards.exhaustPile.begin(), bc.cards.exhaustPile.end());
                break;

            case CardSelectTask::HEADBUTT:
            case CardSelectTask::HOLOGRAM:
            case CardSelectTask::LIQUID_MEMORIES_POTION:
                emitRange(bc.cards.discardPile.begin(), bc.cards.discardPile.end());
                break;

            case CardSelectTask::SECRET_TECHNIQUE:
            case CardSelectTask::SECRET_WEAPON:
                emitRange(bc.cards.drawPile.begin(), bc.cards.drawPile.end());
                break;

            default:
                // 枚举器（getCardSelectActions）没给这些任务铺边 ⇒ 全 0 是对的。
                break;
            }

            // 铺出来的槽数（被 cardSelectSlots 截断后的上界）；涂 0 的槽也算在内，不是合法动作数。
            const int candidateCount = n;
            while (n < Snap::cardSelectSlots)
            {
                // 补零宽度必须与 `cardSelectFields` **同源**：手写宽度会在字段数变化时静默错位。
                s.push_zeros(Snap::cardSelectFields);
                ++n;
            }

            s.push<30>(candidateCount);
            s.push<10>(info.pickCount);
            s.push<1>(info.canPickZero ? 1 : 0);
            s.push<1>(info.canPickAnyNumber ? 1 : 0);
        }

        // 一次性取整场战斗的定长快照（= 网络输入向量，已归一化）。除数逐字段写在赋值处。
    } // namespace
    inline std::array<float, Snap::snapshotLen> snapshotOfImpl(const BattleContext &bc)
    {
        SnapWriter s;

        // player: [curHp, maxHp, block, energy, turn, inputState, cardSelectTask]
        // 前 5 列是数值，后两列是名义类别（one-hot 的行号）⇒ 走 `push_index`。
        s.push<100>(bc.player.curHp);
        s.push<100>(bc.player.maxHp);
        s.push<200>(bc.player.block);
        s.push<10>(bc.player.energy);
        s.push<50>(bc.turn);
        s.push_index(static_cast<int>(bc.inputState));
        s.push_index(static_cast<int>(bc.cardSelectInfo.cardSelectTask));

        // counts: [monsterCount, monstersAlive, handSize, drawSize, discardSize, exhaustSize, deckSize]
        // 七列同量纲（张数 / 只数）⇒ 同一除数。
        s.push<5>(bc.monsters.monsterCount);
        s.push<5>(bc.monsters.monstersAlive);
        s.push<10>(bc.cards.cardsInHand);
        s.push<30>(static_cast<int>(bc.cards.drawPile.size()));
        s.push<30>(static_cast<int>(bc.cards.discardPile.size()));
        s.push<30>(static_cast<int>(bc.cards.exhaustPile.size()));
        s.push<30>(bc.masterDeckSize);

        // hand: [cardId, costForTurn, specialData, playable]
        for (int i = 0; i < Snap::handSlots; ++i)
        {
            if (i < bc.cards.cardsInHand)
            {
                const auto &c = bc.cards.hand[i];
                s.push_index(static_cast<int>(c.id) * 2 + (c.upgraded ? 1 : 0));
                s.push<5>(c.costForTurn);
                s.push<100>(c.specialData);
                s.push<1>(c.canUseOnAnyTarget(bc) ? 1 : 0);
            }
            else
                s.push_zeros(Snap::handFields);
        }

        // pile: [cardId]
        const auto appendPile = [&s](const std::vector<sts::CardInstance> &pile, int maxSlots)
        {
            const int n = static_cast<int>(pile.size());
            for (int j = 0; j < maxSlots; ++j)
            {
                if (j < n)
                    s.push_index(static_cast<int>(pile[j].id) * 2 + (pile[j].upgraded ? 1 : 0));
                else
                    s.push_index(0);
            }
        };
        appendPile(bc.cards.drawPile, Snap::pileSlots);
        appendPile(bc.cards.discardPile, Snap::pileSlots);
        appendPile(bc.cards.exhaustPile, Snap::pileSlots);

        // deck: [cardId]（`bc.masterDeck` 里存的已经是编码后的 id）
        for (int j = 0; j < Snap::deckSlots; ++j)
            s.push_index(j < bc.masterDeckSize ? bc.masterDeck[j] : 0);

        // 怪物：除意图 id 外还给出算好的威胁值。intentDamage 是单次命中的实际伤害，
        for (int i = 0; i < Snap::monsterSlots; ++i)
        {
            if (i < bc.monsters.monsterCount)
            {
                const auto &m = bc.monsters.arr[i];
                s.push_index(static_cast<int>(m.id));
                s.push_index(static_cast<int>(m.moveHistory[0]));
                s.push<500>(m.curHp);
                s.push<500>(m.maxHp);
                s.push<200>(m.block);
                s.push<1>(m.isTargetable() ? 1 : 0);

                // 非攻击意图 getMoveBaseDamage 返回 {0, 0}
                const auto md = m.getMoveBaseDamage(bc);
                if (md.damage > 0 && md.attackCount > 0)
                {
                    s.push<50>(m.calculateDamageToPlayer(bc, md.damage));
                    s.push<5>(md.attackCount);
                }
                else
                {
                    s.push<50>(0);
                    s.push<5>(0);
                }

                // 每怪杂项（含义随 monsterId 变：暗灵复活倒计时、觉醒者阶段位、冠军二阶段…）
                s.push<10>(m.miscInfo);
            }
            else
            {
                s.push_zeros(Snap::monsterFields);
            }
        }

        // 玩家属性块
        s.push_index(static_cast<int>(bc.player.stance));
        s.push<10>(bc.player.orbSlots);
        s.push<5>(bc.player.energyPerTurn);

        // 玩家回合计数块（次数量纲，同一除数；**末列除外** —— 见下面那行）
        s.push<20>(bc.player.cardsPlayedThisTurn);
        s.push<20>(bc.player.attacksPlayedThisTurn);
        s.push<20>(bc.player.skillsPlayedThisTurn);
        s.push<20>(bc.player.cardsDiscardedThisTurn);
        s.push<20>(bc.player.cardDrawPerTurn);
        s.push<20>(bc.player.timesDamagedThisCombat);
        s.push<20>(bc.player.penNibCounter);
        s.push<20>(bc.player.happyFlowerCounter);
        s.push<20>(bc.player.incenseBurnerCounter);
        s.push<20>(bc.player.inkBottleCounter);
        s.push<20>(bc.player.nunchakuCounter);
        s.push<20>(bc.player.sundialCounter);
        // `bitset<3>`：三种牌型各是否已用于橙丸（bit0=攻击 / bit1=技能 / bit2=能力）。
        // 取值 0..7 是**离散状态**（8 种组合），不是量级 ⇒ 走 `push_index`，由 8 行的嵌入 /
        // one-hot 去学每一位。**别当数值除一个除数**：单标量下三个位共用同一个权重（只差
        // 1:2:4 的缩放）⇒ 秩 1 编码，学不出独立的位权重。
        s.push_index(static_cast<int>(bc.player.orangePelletsCardTypesPlayed.to_ulong()));

        // 玩家状态块：全枚举，下标 = PlayerStatus 值
        for (int k = 0; k < Snap::playerStatusLen; ++k)
            s.push<20>(bc.player.getStatusRuntime(static_cast<PlayerStatus>(k)));

        // 怪物状态块：全枚举，按 [怪物][状态] 展平
        for (int i = 0; i < Snap::monsterSlots; ++i)
        {
            if (i < bc.monsters.monsterCount)
            {
                const auto &m = bc.monsters.arr[i];
                for (int k = 0; k < Snap::monsterStatusLen; ++k)
                    s.push<20>(m.getStatusInternal(static_cast<MonsterStatus>(k)));
            }
            else
                s.push_zeros(Snap::monsterStatusLen);
        }

        appendCardSelectCandidates(bc, s);

        // run
        s.push<50>(bc.floorNum);
        s.push<999>(bc.player.gold);

        // relic
        for (int w = 0; w < 3; ++w)
        {
            const std::uint64_t bits = w == 0 ? bc.player.relicBits0 : w == 1 ? bc.player.relicBits1
                                                                              : bc.player.relicBits2;
            for (int b = 0; b < 64 && w * 64 + b < Snap::relicLen; ++b)
                s.push<1>(static_cast<int>((bits >> b) & 1ULL));
        }

        // potion
        // 计数两列除 `potionSlots`（= `POTION_BELT` 容量上限 5）：它们是"瓶数 / 容量"，
        // 与 `counts` 块同一性质 ⇒ 同样要归一化（上限恰为 5 ⇒ 除完 ≤ 1）。
        s.push<Snap::potionSlots>(bc.potionCount);
        s.push<Snap::potionSlots>(bc.potionCapacity);
        const int empty0 = static_cast<int>(Potion::INVALID);
        const int empty1 = static_cast<int>(Potion::EMPTY_POTION_SLOT);

        for (int k = 0; k < Snap::potionSlotLen; ++k)
        {
            int t = empty0;
            if (k < Snap::potionSlots && k < bc.potionCapacity)
            {
                const int v = static_cast<int>(bc.potions[k]);
                t = (v == empty0 || v == empty1) ? empty0 : v; // 空槽 → 0
            }
            s.push_index(t);
        }

        assert(s.n == Snap::snapshotLen);
        return s.buf;
    }

    // 一次性取整场战斗的定长快照（= 网络输入向量，已归一化）。除数逐字段写在赋值处。
    inline std::array<float, Snap::snapshotLen> snapshotOf(const BattleContext &bc)
    {
        return snapshotOfImpl(bc);
    }
} // namespace sts::collect

#endif // STS_LIGHTSPEED_SNAPSHOT_H
