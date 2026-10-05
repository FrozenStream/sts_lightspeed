//
// Created by keega on 9/16/2021.
//

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "combat/BattleContext.h"
#include "combat/Monster.h"
#include "combat/MonsterGroup.h"
#include "combat/Player.h"
#include "game/Game.h"
#include "sim/ConsoleSimulator.h"
#include "sim/PrintHelpers.h"
#include "sim/SimHelpers.h"
#include "sim/search/ScumSearchAgent2.h"

#include "slaythespire.h"

using namespace sts;

static constexpr std::pair<const char *, Event> kEventBindingNames[] = {
    {"INVALID", Event::INVALID},
    {"MONSTER", Event::MONSTER},
    {"REST", Event::REST},
    {"SHOP", Event::SHOP},
    {"TREASURE", Event::TREASURE},
    {"NEOW", Event::NEOW},
    {"OMINOUS_FORGE", Event::OMINOUS_FORGE},
    {"PLEADING_VAGRANT", Event::PLEADING_VAGRANT},
    {"ANCIENT_WRITING", Event::ANCIENT_WRITING},
    {"OLD_BEGGAR", Event::OLD_BEGGAR},
    {"BIG_FISH", Event::BIG_FISH},
    {"BONFIRE_SPIRITS", Event::BONFIRE_SPIRITS},
    {"COLOSSEUM", Event::COLOSSEUM},
    {"CURSED_TOME", Event::CURSED_TOME},
    {"DEAD_ADVENTURER", Event::DEAD_ADVENTURER},
    {"DESIGNER_IN_SPIRE", Event::DESIGNER_IN_SPIRE},
    {"AUGMENTER", Event::AUGMENTER},
    {"DUPLICATOR", Event::DUPLICATOR},
    {"FACE_TRADER", Event::FACE_TRADER},
    {"FALLING", Event::FALLING},
    {"FORGOTTEN_ALTAR", Event::FORGOTTEN_ALTAR},
    {"THE_DIVINE_FOUNTAIN", Event::THE_DIVINE_FOUNTAIN},
    {"GHOSTS", Event::GHOSTS},
    {"GOLDEN_IDOL", Event::GOLDEN_IDOL},
    {"GOLDEN_SHRINE", Event::GOLDEN_SHRINE},
    {"WING_STATUE", Event::WING_STATUE},
    {"KNOWING_SKULL", Event::KNOWING_SKULL},
    {"LAB", Event::LAB},
    {"THE_SSSSSERPENT", Event::THE_SSSSSERPENT},
    {"LIVING_WALL", Event::LIVING_WALL},
    {"MASKED_BANDITS", Event::MASKED_BANDITS},
    {"MATCH_AND_KEEP", Event::MATCH_AND_KEEP},
    {"MINDBLOOM", Event::MINDBLOOM},
    {"HYPNOTIZING_COLORED_MUSHROOMS", Event::HYPNOTIZING_COLORED_MUSHROOMS},
    {"MYSTERIOUS_SPHERE", Event::MYSTERIOUS_SPHERE},
    {"THE_NEST", Event::THE_NEST},
    {"NLOTH", Event::NLOTH},
    {"NOTE_FOR_YOURSELF", Event::NOTE_FOR_YOURSELF},
    {"PURIFIER", Event::PURIFIER},
    {"SCRAP_OOZE", Event::SCRAP_OOZE},
    {"SECRET_PORTAL", Event::SECRET_PORTAL},
    {"SENSORY_STONE", Event::SENSORY_STONE},
    {"SHINING_LIGHT", Event::SHINING_LIGHT},
    {"THE_CLERIC", Event::THE_CLERIC},
    {"THE_JOUST", Event::THE_JOUST},
    {"THE_LIBRARY", Event::THE_LIBRARY},
    {"THE_MAUSOLEUM", Event::THE_MAUSOLEUM},
    {"THE_MOAI_HEAD", Event::THE_MOAI_HEAD},
    {"THE_WOMAN_IN_BLUE", Event::THE_WOMAN_IN_BLUE},
    {"TOMB_OF_LORD_RED_MASK", Event::TOMB_OF_LORD_RED_MASK},
    {"TRANSMORGRIFIER", Event::TRANSMORGRIFIER},
    {"UPGRADE_SHRINE", Event::UPGRADE_SHRINE},
    {"VAMPIRES", Event::VAMPIRES},
    {"WE_MEET_AGAIN", Event::WE_MEET_AGAIN},
    {"WHEEL_OF_CHANGE", Event::WHEEL_OF_CHANGE},
    {"WINDING_HALLS", Event::WINDING_HALLS},
    {"WORLD_OF_GOOP", Event::WORLD_OF_GOOP},
};

namespace
{
struct Snap
{
    // ── player: [curHp, maxHp, block, energy, turn, inputState, cardSelectTask]
    static constexpr int playerLen = 7;
    static constexpr int playerOff = 0;

    // ── counts: [monsterCount, monstersAlive, handSize, drawSize, discardSize, exhaustSize, deckSize]
    static constexpr int countsLen = 7;
    static constexpr int countsOff = playerOff + playerLen;

    // ── hand: [cardId, costForTurn, specialData, playable]
    static constexpr int handSlots = sts::py::handSlots;
    static constexpr int handFields = 4;
    static constexpr int handOff = countsOff + countsLen;

    // ── 三个牌堆: 128 * [cardId]
    static constexpr int pileSlots = 128;
    static constexpr int pileFields = 1;
    static constexpr int drawOff = handOff + handSlots * handFields;
    static constexpr int discardOff = drawOff + pileSlots * pileFields;
    static constexpr int exhaustOff = discardOff + pileSlots * pileFields;

    // ── deck: 总牌组，`Deck::MAX_SIZE` 槽 * [cardId]（run 牌组容器的容量，**不是**战斗侧堆的 64）
    static constexpr int deckSlots = Deck::MAX_SIZE;
    static constexpr int deckFields = 1;
    static constexpr int deckOff = exhaustOff + pileSlots * pileFields;

    // ── monster:
    // [MonsterId, intentMoveId, curHp, maxHp, block,
    //  targetable, intentDamage, intentHits, miscInfo]
    static constexpr int monsterSlots = sts::py::maxMonsters;
    static constexpr int monsterFields = 9;
    static constexpr int monsterOff = deckOff + deckSlots * deckFields;

    // ── player: [stance, orbSlots, energyPerTurn]
    static constexpr int playerAttrLen = 3;
    static constexpr int playerAttrOff = monsterOff + monsterSlots * monsterFields;

    // ── player counter: [cardsPlayedThisTurn, attacksPlayedThisTurn,  skillsPlayedThisTurn,
    //  cardsDiscardedThisTurn, cardDrawPerTurn, timesDamagedThisCombat]
    // —— relic counter: [penNib, happyFlower, incenseBurner,
    //    inkBottle, nunchaku, sundial, orangePelletsCardTypesPlayed]
    static constexpr int playerCounterLen = 13;
    static constexpr int playerCounterOff = playerAttrOff + playerAttrLen;

    // ── player status
    static constexpr int playerStatusLen = static_cast<int>(PlayerStatus::THE_BOMB) + 1;
    static constexpr int playerStatusOff = playerCounterOff + playerCounterLen;

    // ── monster status: 5 * monsterStatusLen
    static constexpr int monsterStatusLen = static_cast<int>(MonsterStatus::INVALID) + 1;
    static constexpr int monsterStatusOff = playerStatusOff + playerStatusLen;

    // ── card select candicate: pileSlots * 4：[cardId, cost, costForTurn, specialData]，
    static constexpr int cardSelectSlots = pileSlots; // **与 pileSlots 同宽**
    static constexpr int cardSelectFields = 4;
    static constexpr int cardSelectOff = monsterStatusOff + monsterSlots * monsterStatusLen;

    // ── select meta: [candidateCount, pickCount, canPickZero, canPickAnyNumber]
    static constexpr int selectMetaLen = 4;
    static constexpr int selectMetaOff = cardSelectOff + cardSelectSlots * cardSelectFields;

    // ── run 级上下文：[floor, gold]
    static constexpr int runCtxLen = 2;
    static constexpr int runCtxOff = selectMetaOff + selectMetaLen;

    // ── 遗物
    static constexpr int relicLen = static_cast<int>(RelicId::INVALID) + 1;
    static constexpr int relicOff = runCtxOff + runCtxLen;

    // ── relic meta: [potionCount, potionCapacity]
    static constexpr int potionMetaLen = 2;
    static constexpr int potionOff = relicOff + relicLen;

    // ── 药水
    static constexpr int potionSlots = sts::py::potionSlots;
    static constexpr int potionSlotLen = potionSlots;
    static constexpr int potionSlotOff = potionOff + potionMetaLen;

    // 快照总宽
    static constexpr int snapshotLen = potionSlotOff + potionSlotLen;

    static constexpr int encodedCardIdTypes = 2 * sts::cardEnumCount;
    static constexpr int monsterIdTypes = static_cast<int>(MonsterId::WRITHING_MASS) + 1;
    static constexpr int monsterMoveIdTypes = static_cast<int>(MonsterMoveId::WRITHING_MASS_STRONG_STRIKE) + 1;
    static constexpr int inputStateTypes = static_cast<int>(InputState::SELECT_CONFUSED_CARD_COST) + 1;
    static constexpr int cardSelectTaskTypes = static_cast<int>(CardSelectTask::WARCRY) + 1;
    static constexpr int stanceTypes = static_cast<int>(Stance::DIVINITY) + 1;
    static constexpr int potionTypes = sts::potionEnumCount;
    // `bitset<3>`（橙丸：三种牌型各是否已用）⇒ 8 种组合，名义类别，不是量级。
    static constexpr int orangePelletsTypes = 8;
};

// 快照写入器：**除数写在赋值处**，与字段语义同一行。
// 不分块列除数表 —— 那种写法在「块内字段数 ≠ 表长」时会**静默轮转**
// （`card_select` 的 3 个值与 4 个字段就是这么错的），且表与实际写入序脱钩后无从对账。
//
// 产出的就是**网络输入向量**：数值列一律归一化，索引 / 名义列走 `push_index`
// （**没有除数参数** —— 除掉一个查表下标是概念错误，这条路在类型上写不出来）。
// ⚠️ 它**不再是「引擎状态的忠实转储」** ⇒ 要读真值走活对象访问器
// （`bc.player_hp` / `bc.monsters[i].cur_hp` / `bc.monster_count` …），
// 别拿归一化值反推。
//
// ⚠️ 缓冲 float32：全部原始值 |v| < 2^24 ⇒ 整数在 float32 里精确（索引列不失真）。
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
void appendCardSelectCandidates(const BattleContext &bc, SnapWriter &s)
{
    const auto &info = bc.cardSelectInfo;
    int n = 0;

    // 合法性直接问引擎。
    const auto legalAt = [&bc](int k) {
        return search::Action(search::ActionType::SINGLE_CARD_SELECT, k).isValidAction(bc);
    };

    // 逐字段语义与手牌块同形：cardId 走索引，费用 / 本回合费用 ÷5，specialData ÷999。
    const auto emitInstance = [&](int k, const CardInstance &c) {
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
    const auto emitRange = [&](const auto &begin, const auto &end) {
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
std::array<float, Snap::snapshotLen> snapshotOf(const BattleContext &bc)
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
    const auto appendPile = [&s](const std::vector<sts::CardInstance> &pile, int maxSlots) {
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
        const std::uint64_t bits = w == 0 ? bc.player.relicBits0 : w == 1 ? bc.player.relicBits1 : bc.player.relicBits2;
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
} // namespace

PYBIND11_MODULE(slaythespire, m)
{
    m.doc() = "pybind11 example plugin"; // optional module docstring

    // ── 类型预注册 ──────────────────────────────────────────────
    // pybind11 在 .def()/.value() 那一刻就把类型名写进 __text_signature__。
    // 被引用的类型若此时尚未注册，签名会退化成 C++ 名（sts::Card 之类），
    // pybind11-stubgen 解析不了 → 该处类型丢失成 `...`。
    // 所以先把所有类型注册一遍；各自的 .def()/.value() 仍留在原处。
    pybind11::enum_<search::ActionType> actionTypeEnum(m, "ActionType");
    pybind11::enum_<GameOutcome> gameOutcome(m, "GameOutcome");
    pybind11::enum_<Outcome> battleOutcome(m, "Outcome");
    pybind11::enum_<InputState> inputStateEnum(m, "InputState");
    pybind11::enum_<CardSelectTask> cardSelectTaskEnum(m, "CardSelectTask");
    pybind11::enum_<ScreenState> screenState(m, "ScreenState");
    pybind11::enum_<Event> eventEnum(m, "Event");
    pybind11::enum_<CharacterClass> characterClass(m, "CharacterClass");
    pybind11::enum_<Room> roomEnum(m, "Room");
    pybind11::enum_<CardRarity> cardRarity(m, "CardRarity");
    pybind11::enum_<CardColor> cardColor(m, "CardColor");
    pybind11::enum_<CardType> cardType(m, "CardType");
    pybind11::enum_<CardId> cardId(m, "CardId");
    pybind11::enum_<MonsterEncounter> meEnum(m, "MonsterEncounter");
    pybind11::enum_<RelicId> relicEnum(m, "RelicId");
    pybind11::enum_<RelicTier> relicTierEnum(m, "RelicTier");
    // 药水枚举：**用头文件里的名字表循环填**（43 项，手抄必出错）。
    // `potionEnumNames[]`（`include/constants/Potions.h`）的下标 == 枚举值。
    pybind11::enum_<Potion> potionEnum(m, "Potion");
    // 状态 / 怪 id 枚举：快照里这几列都是裸 `int`，绑成枚举后 Python 侧才有语义
    // （`sts.MonsterId(4).name` == "AWAKENED_ONE"）。填表循环在文末。
    pybind11::enum_<MonsterId> monsterIdEnum(m, "MonsterId");
    pybind11::enum_<MonsterMoveId> monsterMoveEnum(m, "MonsterMoveId");
    pybind11::enum_<PlayerStatus> playerStatusEnum(m, "PlayerStatus");
    pybind11::enum_<MonsterStatus> monsterStatusEnum(m, "MonsterStatus");

    pybind11::class_<Card> card(m, "Card");
    pybind11::class_<RelicInstance> relic(m, "Relic");
    pybind11::class_<Player> player(m, "Player");
    pybind11::class_<Monster> monster(m, "Monster");
    pybind11::class_<MonsterGroup> monsterGroup(m, "MonsterGroup");
    pybind11::class_<Map> map(m, "SpireMap");
    pybind11::class_<NNInterface> nnInterface(m, "NNInterface");
    pybind11::class_<GameContext> gameContext(m, "GameContext");
    pybind11::class_<search::ScumSearchAgent2> agent(m, "Agent");
    pybind11::class_<search::Action> action(m, "Action",
                                            "C++ search::Action — the Python-facing action type (replaces raw bits).");
    pybind11::class_<sts::py::SearchStats> searchStats(
        m, "SearchStats", "Per-state search statistics from BattleContext.search_stats().");
    pybind11::class_<BattleContext> battleContext(m, "BattleContext");

    m.def("play", &sts::py::play, "play Slay the Spire Console");
    m.def("get_seed_str", &SeedHelper::getString, "gets the integral representation of seed string used in the game ui");
    m.def("get_seed_long", &SeedHelper::getLong, "gets the seed string representation of an integral seed");
    m.def("getNNInterface", &sts::NNInterface::getInstance, "gets the NNInterface object");
    m.def("get_neow_options", &sts::py::getNeowOptionDescriptions, pybind11::arg("gc"), "get Neow bonus descriptions");
    m.def("get_card_color", &sts::getCardColor, pybind11::arg("card_id"), "get the color (CardColor) of a card id");
    m.def("evaluate_end_state", &search::BattleScumSearcher2::evaluateEndState, pybind11::arg("bc"),
          "引擎的终局分（`evaluateEndState`）：赢 = 100*(35 + curHp + 4*药水 - 0.01*回合)，"
          "输 = 10*(1-非小怪血比) - 活怪数 - 0.2*能量浪费 + 0.03*抽牌 + 2*药水 + 0.2*回合。");
    m.attr("DEFAULT_EXPLORATION_PARAMETER") = search::BattleScumSearcher2::defaultExplorationParameter;
    // 批量快照：一次跨语言调用把 k 个 BattleContext 的快照按行写进 (k, snapshot_len) float32。
    // 逐叶调 `get_snapshot_np` 是 k 次分配 + Python 侧 `np.stack`。走**归一化**出口（同 `get_snapshot_np`）。
    m.def(
        "batch_snapshot",
        [](const pybind11::sequence &ctxs) {
            const auto k = static_cast<pybind11::ssize_t>(ctxs.size());
            pybind11::array_t<float> out(
                std::vector<pybind11::ssize_t>{k, static_cast<pybind11::ssize_t>(Snap::snapshotLen)});
            auto buf = out.mutable_unchecked<2>();
            for (pybind11::ssize_t j = 0; j < k; ++j)
            {
                const auto v = snapshotOf(ctxs[j].cast<const BattleContext &>());
                for (int i = 0; i < Snap::snapshotLen; ++i)
                {
                    buf(j, i) = v[i];
                }
            }
            return out;
        },
        pybind11::arg("contexts"), "batch of get_snapshot_np: (k, snapshot_len) float32, row j = contexts[j]");

    // 地图网格尺寸：引擎里是裸 `std::array<std::array<MapNode, 7>, 15> nodes`（无具名常量）
    // ⇒ run 侧的坐标推导（层号 → 列）查这里，Python 侧**不写死** 7 / 15。
    m.attr("MAP_WIDTH") = 7;
    m.attr("MAP_HEIGHT") = 15;

    // 快照布局：读本文件开头的 `Snap` —— **唯一一份**（布局与生产者同住一个 TU）。
    // Python 侧一律从这里推导下标，**不要在 Python 里硬编码偏移**。
    m.def(
        "snapshot_layout",
        []() {
            pybind11::dict d;
            d["len"] = Snap::snapshotLen;
            d["player_len"] = Snap::playerLen;
            d["player_off"] = Snap::playerOff;
            d["counts_len"] = Snap::countsLen;
            d["counts_off"] = Snap::countsOff;
            d["hand_slots"] = Snap::handSlots;
            d["hand_fields"] = Snap::handFields;
            d["hand_off"] = Snap::handOff;
            d["pile_slots"] = Snap::pileSlots;
            d["pile_fields"] = Snap::pileFields;
            d["draw_off"] = Snap::drawOff;
            d["discard_off"] = Snap::discardOff;
            d["exhaust_off"] = Snap::exhaustOff;
            d["deck_slots"] = Snap::deckSlots;
            d["deck_fields"] = Snap::deckFields;
            d["deck_off"] = Snap::deckOff;
            d["monster_slots"] = Snap::monsterSlots;
            d["monster_fields"] = Snap::monsterFields;
            d["monster_off"] = Snap::monsterOff;
            d["player_attr_len"] = Snap::playerAttrLen;
            d["player_attr_off"] = Snap::playerAttrOff;
            d["player_counter_len"] = Snap::playerCounterLen;
            d["player_counter_off"] = Snap::playerCounterOff;
            d["player_status_len"] = Snap::playerStatusLen;
            d["player_status_off"] = Snap::playerStatusOff;
            d["monster_status_len"] = Snap::monsterStatusLen;
            d["monster_status_off"] = Snap::monsterStatusOff;
            d["card_select_slots"] = Snap::cardSelectSlots;
            d["card_select_fields"] = Snap::cardSelectFields;
            d["card_select_off"] = Snap::cardSelectOff;
            d["select_meta_len"] = Snap::selectMetaLen;
            d["select_meta_off"] = Snap::selectMetaOff;
            d["run_ctx_len"] = Snap::runCtxLen;
            d["run_ctx_off"] = Snap::runCtxOff;
            d["relic_len"] = Snap::relicLen;
            d["relic_off"] = Snap::relicOff;
            d["potion_meta_len"] = Snap::potionMetaLen;
            d["potion_meta_off"] = Snap::potionOff;
            d["potion_slots"] = Snap::potionSlots;
            d["potion_slot_off"] = Snap::potionSlotOff;
            d["encoded_card_id_types"] = Snap::encodedCardIdTypes;
            d["monster_id_types"] = Snap::monsterIdTypes;
            d["monster_move_id_types"] = Snap::monsterMoveIdTypes;
            d["input_state_types"] = Snap::inputStateTypes;
            d["card_select_task_types"] = Snap::cardSelectTaskTypes;
            d["stance_types"] = Snap::stanceTypes;
            d["orange_pellets_types"] = Snap::orangePelletsTypes;
            d["potion_types"] = Snap::potionTypes;
            return d;
        },
        "fixed-length battle snapshot layout: block lengths and start offsets");

    // 动作索引空间的**结构**（权威 = `slaythespire.h` 里那几行 `constexpr`）：
    // 段起点 / 段宽 / 步长一律从这里取，Python 侧**不手抄**。
    m.def(
        "action_layout",
        []() {
            pybind11::dict d;
            d["hand_slots"] = sts::py::handSlots;
            d["potion_slots"] = sts::py::potionSlots;
            d["monster_slots"] = sts::py::maxMonsters;
            d["hand_stride"] = sts::py::handStride;     // 单张牌 = 1 无目标 + 怪槽
            d["potion_stride"] = sts::py::potionStride; // 单瓶药 = 1 无目标 + 怪槽 + 1 丢弃
            d["potion_base"] = sts::py::potionBase;     // 药水段起点 = 手牌段宽度
            d["end_turn"] = sts::py::endTurnIdx;
            d["count"] = sts::py::unifiedActionCount;
            return d;
        },
        "unified action index space: segment widths, strides and offsets");

    // `sts.Event` 成员数（含 INVALID）：run 网的事件嵌入表行数。
    m.def(
        "event_count", []() { return (int)(sizeof(kEventBindingNames) / sizeof(kEventBindingNames[0])); },
        "`sts.Event` 成员数（含 INVALID）");
    // 药水池：**引擎没有 `returnRandomPotion`**，所以把池子本身给 Python，
    // 抽签用场景自己的确定性 RNG 做。（遗物侧有 `returnRandomRelic`，走 `obtain_random_relic`。）
    m.def(
        "potion_pool",
        [](CharacterClass cc) {
            std::vector<Potion> v;
            for (int i = 0; i < PotionPool::poolSize; ++i)
                v.push_back(PotionPool::getPotionForClass(cc, i));
            return v;
        },
        pybind11::arg("character_class"), "the potion pool for the given character");
    m.def(
        "potion_rarity", [](Potion p) { return static_cast<int>(getPotionRarity(p)); }, pybind11::arg("potion"),
        "the rarity of the given potion");

    // --- 搜索动作：直接暴露 search::Action（Python 侧不再用裸 bits）---
    actionTypeEnum.value("CARD", search::ActionType::CARD)
        .value("POTION", search::ActionType::POTION)
        .value("SINGLE_CARD_SELECT", search::ActionType::SINGLE_CARD_SELECT)
        .value("MULTI_CARD_SELECT", search::ActionType::MULTI_CARD_SELECT)
        .value("END_TURN", search::ActionType::END_TURN);

    action.def(pybind11::init<std::uint32_t>(), pybind11::arg("bits"), "Action(bits) == C++ Action(std::uint32_t)")
        .def(pybind11::init<search::ActionType>(), pybind11::arg("action_type"),
             "Action(action_type) == C++ Action(ActionType)")
        .def(pybind11::init<search::ActionType, int>(), pybind11::arg("action_type"), pybind11::arg("idx1"),
             "Action(action_type, idx1) == C++ Action(ActionType, int)")
        .def(pybind11::init<search::ActionType, int, int>(), pybind11::arg("action_type"), pybind11::arg("idx1"),
             pybind11::arg("idx2"), "Action(action_type, idx1, idx2) == C++ Action(ActionType, int, int)")
        .def_property_readonly(
            "bits", [](const search::Action &a) { return a.bits; }, "raw bit encoding (== C++ Action::bits)")
        .def_property_readonly("action_type", &search::Action::getActionType)
        .def_property_readonly("source_idx", &search::Action::getSourceIdx,
                               "card/potion index (hand index / potion slot)")
        .def_property_readonly("target_idx", &search::Action::getTargetIdx, "monster index (99 = discard potion)")
        .def_property_readonly("select_idx", &search::Action::getSelectIdx, "option index for SINGLE_CARD_SELECT")
        .def_property_readonly(
            "selected_idxs",
            [](const search::Action &a) {
                const auto fl = a.getSelectedIdxs();
                return std::vector<int>(fl.begin(), fl.end());
            },
            "selected option indices for MULTI_CARD_SELECT")
        .def("is_valid", &search::Action::isValidAction, pybind11::arg("bc"), "C++ Action::isValidAction(bc)")
        .def(
            "execute", [](const search::Action &a, BattleContext &bc) { a.execute(bc); }, pybind11::arg("bc"),
            "C++ Action::execute(bc)")
        .def(
            "print_desc",
            [](const search::Action &a, const BattleContext &bc) {
                std::ostringstream os;
                a.printDesc(os, bc);
                return os.str();
            },
            pybind11::arg("bc"), "C++ Action::printDesc(bc, os) as a string")
        .def(
            "__eq__",
            [](const search::Action &a, pybind11::object other) {
                return pybind11::isinstance<search::Action>(other) && a == other.cast<search::Action>();
            },
            pybind11::is_operator())
        .def(
            "__ne__",
            [](const search::Action &a, pybind11::object other) {
                return !(pybind11::isinstance<search::Action>(other) && a == other.cast<search::Action>());
            },
            pybind11::is_operator())
        .def("__hash__", [](const search::Action &a) { return std::hash<std::uint32_t>{}(a.bits); })
        .def("__repr__", [](const search::Action &a) {
            static const char *typeNames[] = {"CARD", "POTION", "SINGLE_CARD_SELECT", "MULTI_CARD_SELECT", "END_TURN"};
            const auto t = static_cast<int>(a.getActionType());
            const char *name = (t >= 0 && t <= 4) ? typeNames[t] : "INVALID";
            std::ostringstream os;
            os << "<Action " << name << " src=" << a.getSourceIdx() << " tgt=" << a.getTargetIdx() << " bits=0x"
               << std::hex << a.bits << ">";
            return os.str();
        });

    battleContext.def(pybind11::init<>(), "default construct")
        .def(pybind11::init<const BattleContext &>(), pybind11::arg("other"), "copy construct")
        .def(
            "init", [](BattleContext &bc, const GameContext &gc) { bc.init(gc); }, pybind11::arg("gc"),
            "BattleContext::init(gc); requires gc.screen_state == ScreenState::BATTLE.\n"
            "NOT a constructor and does not reset state: call at most once per object "
            "(a second init on the same object is UB). Open a new battle with a new object.")
        .def(
            "init",
            [](BattleContext &bc, const GameContext &gc, MonsterEncounter encounter) { bc.init(gc, encounter); },
            pybind11::arg("gc"), pybind11::arg("encounter"),
            "BattleContext::init(gc, encounter); call at most once per object")
        .def(
            "exit_battle", [](BattleContext &bc, GameContext &gc) { bc.exitBattle(gc); }, pybind11::arg("gc"),
            "write battle results back to the game state; NOT valid for battles built with "
            "init(gc, encounter) — the engine's regain-control callback is null there")
        .def("__repr__", [](const BattleContext &) { return "<BattleContext>"; })
        // --- 状态 ---
        .def("is_battle_over", [](const BattleContext &bc) { return bc.outcome != Outcome::UNDECIDED; })
        .def_readonly("outcome", &BattleContext::outcome)
        .def_readonly("input_state", &BattleContext::inputState)
        .def_property_readonly("card_select_task",
                               [](const BattleContext &bc) { return bc.cardSelectInfo.cardSelectTask; })
        // --- 决策循环 ---
        .def("select_card", sts::py::selectCard, pybind11::arg("idx"),
             "choose the card at idx when input_state == CARD_SELECT")
        .def("end_turn", sts::py::endTurn)
        .def("drink_potion", sts::py::drinkPotion, pybind11::arg("idx"), pybind11::arg("target") = 0)
        .def("discard_potion", sts::py::discardPotion, pybind11::arg("idx"))
        // --- 读取 ---
        .def("get_hand", sts::py::getHand, "cards in hand: [[cardId, upgraded, cost, costForTurn], ...]")
        .def("get_draw_pile", sts::py::getDrawPile, "cards in draw pile")
        .def("get_discard_pile", sts::py::getDiscardPile, "cards in discard pile")
        .def_property_readonly("player_hp", [](const BattleContext &bc) { return bc.player.curHp; })
        .def_property_readonly("player_max_hp", [](const BattleContext &bc) { return bc.player.maxHp; })
        .def_property_readonly("player_block", [](const BattleContext &bc) { return bc.player.block; })
        .def_property_readonly("player_energy", [](const BattleContext &bc) { return bc.player.energy; })
        .def_readonly("turn", &BattleContext::turn)
        .def_property_readonly("monster_count", [](const BattleContext &bc) { return bc.monsters.monsterCount; })
        // 计数类的**引擎真值**：快照里它们是 ÷30 的网络输入，读不出个数 ⇒ 单独开访问器。
        .def_property_readonly("monsters_alive", [](const BattleContext &bc) { return bc.monsters.monstersAlive; })
        .def_property_readonly("master_deck_size", [](const BattleContext &bc) { return bc.masterDeckSize; })
        // 怪物读数**只有两条路**：快照的怪物块（含 id / 意图 / targetable），或活对象
        // `bc.monsters[i]` 上那几个 def_readwrite。**不开 `get_monster_*` 访问器** —— 多一套
        // 读法只是多一处会漂开的副本。
        // ⚠️ 快照已归一化 ⇒ 它的怪物块**只服务网络**，不能当引擎真值读；
        //    要真值就抛 `bc.monsters[i]`。
        .def(
            "get_snapshot", [](const BattleContext &bc) { return snapshotOf(bc); },
            (std::string("one-shot fixed-length snapshot of the whole battle, **NORMALIZED** (float).\n"
                         "len = sts.snapshot_layout()['len']. This is the network input vector: every numeric\n"
                         "column is already divided by its divisor AT THE PRODUCER (see `SnapWriter` in this\n"
                         "file) -- do NOT divide again, and do NOT read it back as engine truth (use the\n"
                         "live-object accessors instead: player_hp / monster_count / monsters_alive /\n"
                         "master_deck_size / monsters[i].cur_hp ...). Index / nominal columns (cardId,\n"
                         "monsterId, intentMoveId, inputState, cardSelectTask, stance, potion ids) are\n"
                         "emitted verbatim so they stay exact lookup keys.\n"
                         "Layout (cardId is NNInterface-style id*2+upgraded; specialData = persistent\n"
                         "counters for Searing Blow / Rampage / Genetic Algorithm / Ritual Dagger):\n"
                         "  [0..6]     player: curHp, maxHp, block, energy, turn, inputState, cardSelectTask\n"
                         "  [8..14]    counts: monsterCount, monstersAlive, handSize, drawSize, discardSize,\n"
                         "             exhaustSize, deckSize. monsterCount INCLUDES CORPSES (it never shrinks);\n"
                         "             monstersAlive is the victory condition, deckSize is the master deck's\n"
                         "             size (the deck slots alone cannot carry it -- mean pooling drops it).\n") +
             "  [" + std::to_string(Snap::handOff) + ".." + std::to_string(Snap::drawOff - 1) +
             "] hand: 10 slots x [cardId, costForTurn, specialData, playable] (empty=0)\n" + "  [" +
             std::to_string(Snap::drawOff) + ".." + std::to_string(Snap::discardOff - 1) +
             "] draw pile: " + std::to_string(Snap::pileSlots) +
             " slots x [cardId], CONTAINER order, zero-padded.\n"
             "             The slot ORDER carries no meaning: the net pools a pile as a SET\n"
             "             (per-card embed -> mean over non-empty slots) -- no ordering claim is\n"
             "             made here and nothing is truncated (128 >= any pile the engine builds).\n"
             "             NO cost/specialData for the same reason: per-slot scalars would be\n"
             "             position-indexed features.\n" +
             "  [" + std::to_string(Snap::discardOff) + ".." + std::to_string(Snap::exhaustOff - 1) +
             "] discard pile: same layout (container order, not recency)\n" + "  [" + std::to_string(Snap::exhaustOff) +
             ".." + std::to_string(Snap::deckOff - 1) + "] exhaust pile: same layout\n" + "  [" +
             std::to_string(Snap::deckOff) + ".." + std::to_string(Snap::monsterOff - 1) +
             "] master deck: " + std::to_string(Snap::deckSlots) +
             " slots x [cardId], ORDER-IRRELEVANT (per-card embed -> mean, like the piles).\n"
             "             NOT redundant with the piles: those hold battle-made temp/status cards\n"
             "             (Shiv/Wound/Dazed...) and shift as the battle progresses; this block is\n"
             "             the constant 'cards I own' view (same set as gc.deck on the run side).\n" +
             "  [" + std::to_string(Snap::monsterOff) + ".." + std::to_string(Snap::playerAttrOff - 1) +
             "] monsters: 5 x [monsterId, intentMoveId, hp, maxHp, block, targetable,\n"
             "             intentDamage, intentHits, miscInfo]\n"
             "             (empty=0). slots are NEVER compacted -- a dead monster keeps its slot\n"
             "             (monsterCount includes corpses). targetable = !isDeadOrEscaped() is the\n"
             "             ONLY dead/gone column; WHY a monster cannot be targeted is carried by\n"
             "             its INTENT: on the very frame it goes down, an Awakened One phase 1 /\n"
             "             Darkling gets AWAKENED_ONE_REBIRTH / DARKLING_REGROW, and an escaping\n"
             "             Looter/Mugger sits on LOOTER_ESCAPE / MUGGER_ESCAPE. miscInfo is the\n"
             "             engine's per-monster scratch int (Darkling regrow countdown, Champ\n"
             "             phase 2, ...).\n"
             "             intentDamage is the PER-HIT damage actually dealt to the player, already\n"
             "             including monster Strength, monster Weak, player Vulnerable/Intangible/Wrath\n"
             "             (Monster::calculateDamageToPlayer). Total incoming = intentDamage * intentHits.\n" +
             "  [" + std::to_string(Snap::playerAttrOff) + ".." + std::to_string(Snap::playerCounterOff - 1) +
             "] player attrs: stance, orbSlots, energyPerTurn\n" + "  [" + std::to_string(Snap::playerCounterOff) +
             ".." + std::to_string(Snap::playerStatusOff - 1) +
             "] player turn counters: cardsPlayed, attacksPlayed, skillsPlayed, cardsDiscarded,\n"
             "             cardDrawPerTurn, timesDamagedThisCombat, penNibCounter, happyFlowerCounter,\n"
             "             incenseBurnerCounter, inkBottleCounter, nunchakuCounter, sundialCounter,\n"
             "             orangePelletsCardTypesPlayed (bitset<3> as 0..7, NOMINAL\n"
             "             category -- index is the 3-bit combination, not a magnitude)\n" +
             "  [" + std::to_string(Snap::playerStatusOff) + ".." + std::to_string(Snap::monsterStatusOff - 1) +
             "] player statuses: 87 x PlayerStatus (index = enum value)\n" + "  [" +
             std::to_string(Snap::monsterStatusOff) + ".." + std::to_string(Snap::cardSelectOff - 1) +
             "] monster statuses: 5 x 43 x MonsterStatus (index = enum value)\n" + "  [" +
             std::to_string(Snap::cardSelectOff) + ".." + std::to_string(Snap::selectMetaOff - 1) +
             "] card-select candidates: " + std::to_string(Snap::cardSelectSlots) +
             " slots x [cardId, cost, costForTurn, specialData]\n"
             "             (source follows cardSelectTask; slot k == select_card(k), CONTAINER order,\n"
             "              so it is indexed differently from the pile blocks above; the width EQUALS\n"
             "              pileSlots because the candidate source is a whole pile)\n"
             "             A NON-ZERO slot means k IS a legal select action; illegal candidates\n"
             "             (Exhume itself, non-upgradable for Armaments, ...) are painted 0 IN PLACE.\n" +
             "  [" + std::to_string(Snap::selectMetaOff) + ".." + std::to_string(Snap::runCtxOff - 1) +
             "] select meta: candidateCount, pickCount, canPickZero, canPickAnyNumber\n"
             "             (candidateCount = slots laid out, NOT the number of legal actions;\n"
             "              CODEX lays out 3 but its legal set is {0,1,2,3}, k=3 = 'skip')\n" +
             "  [" + std::to_string(Snap::runCtxOff) + ".." + std::to_string(Snap::relicOff - 1) +
             "] run context: floor, gold.\n"
             "             (Run-level constants: identical in every frame of a battle.\n"
             "              Deliberately NO map block, NO isElite/isBoss and NO act: node context\n"
             "              does not change how a battle should be played, the elite/boss bit is\n"
             "              already carried by the monster_id embedding, and act is a function\n"
             "              of floor.)\n" +
             "  [" + std::to_string(Snap::relicOff) + ".." + std::to_string(Snap::potionOff - 1) +
             "] relics: " + std::to_string(Snap::relicLen) +
             " x 0/1, MULTI-HOT,\n"
             "             index = RelicId enum value. Read from `bc.player.relicBits0/1/2`\n"
             "             (the battle-side copy), so MCTS clones carry it -- NOT `gc.relics`.\n" +
             "  [" + std::to_string(Snap::potionOff) + ".." + std::to_string(Snap::potionSlotOff - 1) +
             "] potion meta: potionCount, potionCapacity\n" + "  [" + std::to_string(Snap::potionSlotOff) + ".." +
             std::to_string(Snap::snapshotLen - 1) + "] potions: " + std::to_string(Snap::potionSlotLen) +
             " x potion id, ONE PER SLOT (0 = empty).\n"
             "             Slot-addressed like the hand, because the action space is\n"
             "             (slot*7 + target) -- a bag multihot cannot tell two distinct\n"
             "             potions apart.\n")
                .c_str())
        .def(
            "get_snapshot_np",
            [](const BattleContext &bc) {
                const auto v = snapshotOf(bc);
                auto arr = pybind11::array_t<float>(static_cast<pybind11::ssize_t>(v.size()));
                std::copy(v.begin(), v.end(), arr.mutable_data());
                return arr;
            },
            "numpy version of get_snapshot (float32, **ALREADY NORMALIZED** = network input)")
        .def_property_readonly(
            "snapshot_len", [](const BattleContext &) { return Snap::snapshotLen; },
            "length of get_snapshot() output (see sts.snapshot_layout())")
        .def_property_readonly(
            "pile_slots", [](const BattleContext &) { return Snap::pileSlots; },
            ("per-pile card slots included in the snapshot (" + std::to_string(Snap::pileSlots) + ", excess truncated)")
                .c_str())
        .def_property_readonly_static("unified_action_count",
                                      [](pybind11::object) { return sts::py::unifiedActionCount; })
        .def_property_readonly_static("end_turn_idx", [](pybind11::object) { return sts::py::endTurnIdx; })
        .def(
            "clone",
            [](const BattleContext &bc) {
                return std::make_unique<BattleContext>(bc); // 拷贝构造，与 C++ 一致
            },
            "copy of this battle (== BattleContext(other)); 便于 MCTS / AlphaZero 分支")
        // --- 搜索 ---
        .def("scum_search_step", sts::py::scumSearchStep, pybind11::arg("simulations"),
             "C++ UCT search (BattleScumSearcher2): search N simulations from current state and execute the best first "
             "action")
        .def("root_policy", sts::py::rootPolicy, pybind11::arg("simulations"),
             "AlphaZero-style policy target: run the C++ UCT search N simulations from the current state "
             "and return [(Action, visit_count), ...] for the search root's children")
        .def("search_stats", sts::py::searchStats, pybind11::arg("simulations"),
             pybind11::arg("exploration_parameter") = search::BattleScumSearcher2::defaultExplorationParameter,
             "run the C++ UCT search N simulations and return SearchStats: visit policy, "
             "root_value (mean evaluateEndState over playouts), root_win_rate (fraction of "
             "playouts that won, in [0,1]), best_value and best_player_hp.\n"
             "exploration_parameter = UCB 常数（默认 3*sqrt(2) ≈ 4.2426）。调小 = 更倾向利用 q 值。")
        // --- 统一动作索引 ---
        .def("enumerate_actions", sts::py::enumerateActions,
             "all legal actions for current state as Action objects (C++ authoritative enumeration)")
        .def("action_to_idx", sts::py::actionToIdx, pybind11::arg("action"),
             "map an Action to the unified action index (-1 if unmappable)")
        .def("idx_to_action", sts::py::idxToAction, pybind11::arg("idx"),
             "map a unified action index to an Action (invalid Action if illegal; check is_valid(bc))")
        .def("expand_children", sts::py::expandChildren,
             "expand one MCTS node in ONE call: enumerate legal actions and materialize each child\n"
             "(same as `clone()` + `action.execute(child)` + `action_to_idx`, in that order).\n"
             "Returns ([(idx, Action), ...], [child BattleContext, ...]) -- both lists in the same\n"
             "order; idx follows `action_to_idx` (raises when unmappable).")
        .def_readwrite("player", &BattleContext::player)
        .def_readwrite("monsters", &BattleContext::monsters);

    player.def_readwrite("block", &Player::block)
        .def_readwrite("strength", &Player::strength)
        .def_readwrite("dexterity", &Player::dexterity)
        .def_readwrite("focus", &Player::focus)
        .def_readwrite("artifact", &Player::artifact)
        .def_readwrite("cur_hp", &Player::curHp)
        .def_readwrite("max_hp", &Player::maxHp)
        .def_readwrite("energy", &Player::energy)
        .def_readwrite("energy_per_turn", &Player::energyPerTurn)
        .def_readwrite("card_draw_per_turn", &Player::cardDrawPerTurn);

    monster.def_readwrite("idx", &Monster::idx)
        .def_readwrite("cur_hp", &Monster::curHp)
        .def_readwrite("max_hp", &Monster::maxHp)
        .def_readwrite("block", &Monster::block)
        // 只读真值：快照里那几列是归一化的（÷500 / ÷10…），读不出原值。
        .def_readonly("id", &Monster::id)
        .def_readonly("misc_info", &Monster::miscInfo)
        .def_property_readonly("is_targetable", &Monster::isTargetable)
        .def_readwrite("strength", &Monster::strength)
        .def_readwrite("vulnerable", &Monster::vulnerable)
        .def_readwrite("weak", &Monster::weak)
        .def_readwrite("poison", &Monster::poison)
        .def_readwrite("artifact", &Monster::artifact)
        .def_readwrite("lock_on", &Monster::lockOn)
        .def_readwrite("choked", &Monster::choked)
        .def_readwrite("mark", &Monster::mark)
        .def_readwrite("metallicize", &Monster::metallicize)
        .def_readwrite("plated_armor", &Monster::platedArmor)
        .def_readwrite("regen", &Monster::regen);

    monsterGroup.def_property_readonly("count", &sts::py::getMonsterCount)
        .def("__len__", &sts::py::getMonsterCount)
        .def("__getitem__", &sts::py::getMonster, pybind11::return_value_policy::reference_internal)
        .def(
            "__iter__",
            [](const MonsterGroup &self) {
                // ⚠️ 末尾指针只能写 `begin() + monsterCount`：`&arr[monsterCount]` 在
                //    `monsterCount == arr.size()`（= 5 怪上限，`LOTS_OF_SLIMES` 那种）时是
                //    **越界元素访问** ⇒ MSVC Debug 的 `std::array::operator[]` 当场 abort
                //    （实测 rc=3）。`begin() + n`（n ≤ size）是良定义的 one-past-the-end。
                return pybind11::make_iterator(self.arr.begin(), self.arr.begin() + self.monsterCount);
            },
            pybind11::keep_alive<0, 1>());

    searchStats
        .def_readonly("policy", &sts::py::SearchStats::policy,
                      "[(Action, visit_count), ...] for the search root's children")
        .def_readonly("playouts", &sts::py::SearchStats::playouts)
        .def_readonly("root_value", &sts::py::SearchStats::rootValue,
                      "mean evaluateEndState over playouts from root (random-playout expectation)")
        .def_readonly("root_win_rate", &sts::py::SearchStats::rootWinRate,
                      "fraction of playouts from root that ended in victory, in [0,1]")
        .def_readonly("best_value", &sts::py::SearchStats::bestValue, "max evaluateEndState seen (optimistic value)")
        .def_readonly("best_player_hp", &sts::py::SearchStats::bestPlayerHp);

    nnInterface
        .def("getObservation", &NNInterface::getObservation, pybind11::arg("gc"),
             "get observation array (412 elements) given a GameContext")
        .def("getObservationMaximums", &NNInterface::getObservationMaximums,
             "get the defined maximum values of the observation space")
        .def_property_readonly_static("observation_space_size",
                                      [](pybind11::object) { return NNInterface::observation_space_size; });

    agent.def(pybind11::init<>());
    agent
        .def_readwrite("simulation_count_base", &search::ScumSearchAgent2::simulationCountBase,
                       "number of simulations the agent uses for monte carlo tree search each turn")
        .def_readwrite("boss_simulation_multiplier", &search::ScumSearchAgent2::bossSimulationMultiplier,
                       "bonus multiplier to the simulation count for boss fights")
        .def_readwrite(
            "pause_on_card_reward", &search::ScumSearchAgent2::pauseOnCardReward,
            "causes the agent to pause so as to cede control to the user when it encounters a card reward choice")
        .def_readwrite("print_logs", &search::ScumSearchAgent2::printLogs,
                       "when set to true, the agent prints state information as it makes actions")
        .def("playout", &search::ScumSearchAgent2::playout, pybind11::arg("gc"));

    gameContext
        .def(pybind11::init<CharacterClass, std::uint64_t, int>(), pybind11::arg("character_class"),
             pybind11::arg("seed"), pybind11::arg("ascension_level"))
        .def("pick_reward_card", &sts::py::pickRewardCard, pybind11::arg("card"),
             "choose to obtain the card at the specified index in the card reward list")
        .def("skip_reward_cards", &sts::py::skipRewardCards,
             "choose to skip the card reward (increases max_hp by 2 with singing bowl)")
        .def("get_card_reward", &sts::py::getCardReward, "return the current card reward list")
        .def_property_readonly(
            "card_reward_count", [](const GameContext &gc) { return gc.info.rewardsContainer.cardRewardCount; },
            "number of remaining card reward groups")
        .def_property_readonly("encounter", [](const GameContext &gc) { return gc.info.encounter; })
        .def_property_readonly(
            "deck", [](const GameContext &gc) { return std::vector(gc.deck.cards.begin(), gc.deck.cards.end()); },
            "returns a copy of the list of cards in the deck")
        .def(
            "obtain_card", [](GameContext &gc, Card card) { gc.deck.obtain(gc, card); }, pybind11::arg("card"),
            "add a card to the deck")
        .def(
            "remove_card",
            [](GameContext &gc, int idx) {
                if (idx < 0 || idx >= gc.deck.size())
                {
                    throw std::runtime_error("牌组下标越界：" + std::to_string(idx) + "（当前 " +
                                             std::to_string(gc.deck.size()) + " 张）");
                }
                gc.deck.remove(gc, idx);
            },
            "remove a card at a idx in the deck")
        // 随机升级 k 张可升级的牌 —— 引擎自带（`Deck::upgradeRandomCards`，Whetstone /
        // War Paint 那类"随机升级"效果走的就是它）：从可升级集合里洗牌取前 k 张，用
        // `gc.miscRng`（确定性、随局可复现）。
        .def(
            "upgrade_random_cards", [](GameContext &gc, int count) { gc.deck.upgradeRandomCards(gc.miscRng, count); },
            pybind11::arg("count"),
            "upgrade up to `count` random upgradeable cards in the deck\n"
            "(engine's own routine; deterministic via miscRng)")
        // 药水：**唯一一个能往局里塞指定药水的入口**。
        // ⚠️ 药水槽**仍然没有读接口**：战斗里要读库存就看快照的药水块
        //    （`snapshot_layout()['potion_meta_off']`），不要指望 `gc.potions`。
        .def(
            "obtain_potion", [](GameContext &gc, Potion p) { gc.obtainPotion(p); }, pybind11::arg("potion"),
            "add a potion to the first empty slot (no-op when full)")
        .def_property_readonly("potion_count", [](const GameContext &gc) { return gc.potionCount; })
        .def_property_readonly("potion_capacity", [](const GameContext &gc) { return gc.potionCapacity; })
        .def_property_readonly(
            "potions", [](const GameContext &gc) { return std::vector<Potion>(gc.potions.begin(), gc.potions.end()); },
            "copy of the potion slot array (Potion enum; INVALID / EMPTY_POTION_SLOT = empty).\n"
            "专为「场景指纹」与诊断加的 —— 战斗侧读库存请看快照的药水块。")
        .def(
            "remove_relic", [](GameContext &gc, RelicId r) { gc.relics.remove(r); }, pybind11::arg("relic"),
            "remove a relic from the run\n"
            "⚠️ 必须走引擎的 `RelicContainer::remove`（它同时清 `relicBits0/1/2`）；\n"
            "   直接擦 `relics` 那个 vector 会**留下悬空的位** —— 快照的遗物多热块\n"
            "   会继续报一件已经不存在的遗物，而且**不会报错**。")
        .def(
            "obtain_relic", [](GameContext &gc, RelicId r) { return gc.obtainRelic(r); }, pybind11::arg("relic"),
            "obtain a relic into the run (also applies its pickup effect).\n")
        .def(
            "obtain_random_relic", [](GameContext &gc, RelicTier t) { return gc.obtainRelic(gc.returnRandomRelic(t)); },
            pybind11::arg("tier"),
            "draw a relic from the engine's own **per-character** pool for tier `t` and\n"
            "obtain it. `returnRandomRelic` also removes it from the pool ⇒ no duplicates,\n"
            "and the tier mix is the real game's.\n")
        .def(
            "obtain_random_drop_relic",
            [](GameContext &gc) {
                const RelicId r = gc.returnRandomRelic(returnRandomRelicTier(gc.relicRng, gc.act));
                gc.obtainRelic(r);
                return r;
            },
            "抽一件掉落遗物并获得，没有BOSS遗物")
        .def(
            "reset_run_resources",
            [](GameContext &gc) {
                while (gc.deck.size() > 0)
                {
                    gc.deck.remove(gc, gc.deck.size() - 1);
                }
                while (!gc.relics.relics.empty())
                {
                    gc.relics.remove(gc.relics.relics.back().id);
                }
                gc.potions.fill(Potion::EMPTY_POTION_SLOT);
                gc.potionCount = 0;
                gc.potionCapacity = gc.ascension < 11 ? 3 : 2;
                gc.commonRelicPool.clear();
                gc.uncommonRelicPool.clear();
                gc.rareRelicPool.clear();
                gc.shopRelicPool.clear();
                gc.bossRelicPool.clear();
                gc.initRelics();
                gc.initPlayer();
            },
            "reset the run resources to their initial state")
        .def_property_readonly(
            "relics", [](const GameContext &gc) { return std::vector(gc.relics.relics); },
            "returns a copy of the list of relics")
        .def(
            "__repr__",
            [](const GameContext &gc) {
                std::ostringstream oss;
                oss << "<" << gc << ">";
                return oss.str();
            },
            "returns a string representation of the GameContext");

    gameContext.def_readwrite("outcome", &GameContext::outcome)
        .def_readwrite("act", &GameContext::act)
        .def_readwrite("floor_num", &GameContext::floorNum)
        .def_readwrite("screen_state", &GameContext::screenState)

        .def_property_readonly(
            "cur_event", [](const GameContext &gc) { return gc.curEvent; },
            "当前事件 id（`sts.Event`）。**只在 `screen_state == EVENT_SCREEN` 时有意义**；\n"
            "其他屏幕上它残留上一次的值（`transitionToMapNode` 里置 INVALID，但事件结束后不再清），\n"
            "所以读之前先看 `screen_state`。")
        .def_property_readonly(
            "event_phase", [](const GameContext &gc) { return gc.info.eventData; },
            "事件的相位 / 计数器（`info.eventData`）。**语义随事件变**：\n"
            "CURSED_TOME = 已翻页数、SCRAP_OOZE = 已尝试次数、MATCH_AND_KEEP = 剩余尝试数。\n"
            "⚠️ 对**不用这个字段**的事件，它是上一个事件的**残留值**、没有任何含义（不一定是 0）。\n"
            "**别无条件当特征喂进网络**；\n"
            "要用就先按 `cur_event` 查表，只在已知使用该字段的事件上取值。")
        .def_property_readonly(
            "valid_event_options",
            [](const GameContext &gc) {
                // `getValidEventSelectBits` 的前提是「结果为 UNDECIDED 且当前在事件屏」，
                // 它内部是按 `curEvent` 硬编码的 switch，**没有自检** ⇒ 这里自己把门。
                if (gc.outcome != GameOutcome::UNDECIDED || gc.screenState != ScreenState::EVENT_SCREEN)
                {
                    return 0;
                }
                return search::GameAction::getValidEventSelectBits(gc);
            },
            "当前事件页**合法选项下标的位掩码**：bit k == `choose_event_option(k)` 合法。\n"
            "⚠️ 0 也可能是「真的没有合法选项」（MATCH_AND_KEEP 单独处理）。\n"
            "⚠️ 已知例外：LAB 返回 1，但 `chooseEventOption(LAB)` 会 `assert(false)`。")

        .def_readwrite("seed", &GameContext::seed)
        .def_readwrite("cur_map_node_x", &GameContext::curMapNodeX)
        .def_readwrite("cur_map_node_y", &GameContext::curMapNodeY)
        .def_readwrite("cur_room", &GameContext::curRoom)
        //        .def_readwrite("cur_event", &GameContext::curEvent) // todo standardize event names
        .def_readwrite("boss", &GameContext::boss)

        .def_readwrite("cur_hp", &GameContext::curHp)
        .def_readwrite("max_hp", &GameContext::maxHp)
        .def_readwrite("gold", &GameContext::gold)

        .def_readwrite("blue_key", &GameContext::blueKey)
        .def_readwrite("green_key", &GameContext::greenKey)
        .def_readwrite("red_key", &GameContext::redKey)

        .def_readwrite("card_rarity_factor", &GameContext::cardRarityFactor)
        .def_readwrite("potion_chance", &GameContext::potionChance)
        .def_readwrite("monster_chance", &GameContext::monsterChance)
        .def_readwrite("shop_chance", &GameContext::shopChance)
        .def_readwrite("treasure_chance", &GameContext::treasureChance)

        .def_readwrite("shop_remove_count", &GameContext::shopRemoveCount)
        .def_readwrite("speedrun_pace", &GameContext::speedrunPace)
        .def_readwrite("skip_battles", &GameContext::skipBattles,
                       "⚠️ **开发/测试开关**：置 true 后 `enterBattle` 不再等 Python 建 `BattleContext`，\n"
                       "而是直接调 `afterBattle()` —— 战斗不模拟、一律当胜利，\n"
                       "用来快速跑「地图 / 事件 / 奖励」的整局流程（跑路网时很有用）。\n"
                       "⚠️ 它只跳过战斗模拟；**事件序列只由 `eventRng` 决定**，而 `src/combat` 里\n"
                       "对 `eventRng` 的引用数为 **0**（`grep -rn eventRng src/combat` 为空）\n"
                       "⇒ 跳过战斗不改变「第几层碰到哪个事件」。")
        .def_readwrite("note_for_yourself_card", &GameContext::noteForYourselfCard)

        .def("transition_to_map_node", &GameContext::transitionToMapNode,
             "move to a map node (0-6), advancing to the next floor")
        .def("choose_event_option", &GameContext::chooseEventOption, "choose an event option by index")
        .def("regain_control", &GameContext::regainControl, "advance to the next screen when the current one is done")
        .def("get_monster_for_room_creation", &GameContext::getMonsterForRoomCreation,
             "random normal monster encounter for the current act/room")
        .def("get_elite_for_room_creation", &GameContext::getEliteForRoomCreation,
             "random elite encounter for the current act/room")
        .def("player_heal", &GameContext::playerHeal, pybind11::arg("amount"),
             "heal the player by amount (clamped to max hp)")
        .def("choose_boss_relic", &GameContext::chooseBossRelic, pybind11::arg("idx"),
             "choose a boss relic reward (0-2) or 3 to skip")
        .def_property_readonly(
            "boss_relic_reward_count",
            [](const GameContext &gc) { return gc.screenState == ScreenState::BOSS_RELIC_REWARDS ? 3 : 0; },
            "number of boss relic candidates (0 when the screen is not up)")
        .def("choose_campfire_option", &GameContext::chooseCampfireOption, pybind11::arg("idx"),
             "rest room option: 0 rest, 1 smith, 2 recall, 6 skip")
        .def("choose_treasure_room_option", &GameContext::chooseTreasureRoomOption, pybind11::arg("open_chest"),
             "open the chest (True) or skip (False)")
        .def("choose_select_card_screen_option", &GameContext::chooseSelectCardScreenOption, pybind11::arg("idx"),
             "choose a card on an out-of-combat card select screen")
        .def_property_readonly(
            "to_select_cards",
            [](const GameContext &gc) {
                std::vector<Card> cards;
                cards.reserve(gc.info.toSelectCards.size());
                for (const auto &ssc : gc.info.toSelectCards)
                {
                    cards.push_back(ssc.card);
                }
                return cards;
            },
            "cards selectable on the current CARD_SELECT screen (exposes info.toSelectCards); "
            "list index == idx passed to choose_select_card_screen_option(idx)")
        .def_property_readonly(
            "map", [](GameContext &gc) -> Map & { return *gc.map; }, pybind11::return_value_policy::reference_internal,
            "readonly access to the current act map");

    relic.def_readwrite("id", &RelicInstance::id).def_readwrite("data", &RelicInstance::data);

    // 遗物的档位表在 `include/constants/Relics.h`；`RelicId` 自身不带档位，
    // 所以想统计「抽出来的都是些什么货」就必须经由这个函数。
    m.def("relic_tier", &sts::getRelicTier, pybind11::arg("relic"),
          "查询某个 `RelicId` 的档位（`RelicTier`：白/蓝/金/商店/Boss/初始…）。");

    map.def(pybind11::init<std::uint64_t, int, int, bool>());
    map.def("get_room_type", &sts::py::getRoomType);
    map.def("has_edge", &sts::py::hasEdge);
    map.def("get_nn_rep", &sts::py::getNNMapRepresentation);
    map.def("__repr__", [](const Map &m) { return m.toString(true); });

    card.def(pybind11::init<CardId>(), pybind11::arg("card_id"))
        .def(
            "__repr__",
            [](const Card &c) {
                std::string s("<slaythespire.Card ");
                s += c.getName();
                if (c.isUpgraded())
                {
                    s += '+';
                    if (c.id == sts::CardId::SEARING_BLOW)
                    {
                        s += std::to_string(c.getUpgraded());
                    }
                }
                return s += ">";
            },
            "returns a string representation of a Card")
        .def("upgrade", &Card::upgrade)
        .def_readwrite("misc", &Card::misc,
                       "value internal to the simulator used for things like ritual dagger damage");

    card.def_property_readonly("id", &Card::getId)
        .def_property_readonly("upgraded", &Card::isUpgraded)
        .def_property_readonly("upgrade_count", &Card::getUpgraded)
        .def_property_readonly("innate", &Card::isInnate)
        .def_property_readonly("transformable", &Card::canTransform)
        .def_property_readonly("upgradable", &Card::canUpgrade)
        .def_property_readonly("is_strikeCard", &Card::isStrikeCard)
        .def_property_readonly("is_starter_strike_or_defend", &Card::isStarterStrikeOrDefend)
        .def_property_readonly("rarity", &Card::getRarity)
        .def_property_readonly("type", &Card::getType);

    gameOutcome.value("UNDECIDED", GameOutcome::UNDECIDED)
        .value("PLAYER_VICTORY", GameOutcome::PLAYER_VICTORY)
        .value("PLAYER_LOSS", GameOutcome::PLAYER_LOSS);

    battleOutcome.value("UNDECIDED", Outcome::UNDECIDED)
        .value("PLAYER_VICTORY", Outcome::PLAYER_VICTORY)
        .value("PLAYER_LOSS", Outcome::PLAYER_LOSS);

    inputStateEnum.value("EXECUTING_ACTIONS", InputState::EXECUTING_ACTIONS)
        .value("PLAYER_NORMAL", InputState::PLAYER_NORMAL)
        .value("CARD_SELECT", InputState::CARD_SELECT);

    cardSelectTaskEnum.value("INVALID", CardSelectTask::INVALID)
        .value("ARMAMENTS", CardSelectTask::ARMAMENTS)
        .value("CODEX", CardSelectTask::CODEX)
        .value("DISCOVERY", CardSelectTask::DISCOVERY)
        .value("DUAL_WIELD", CardSelectTask::DUAL_WIELD)
        .value("EXHAUST_ONE", CardSelectTask::EXHAUST_ONE)
        .value("EXHAUST_MANY", CardSelectTask::EXHAUST_MANY)
        .value("EXHUME", CardSelectTask::EXHUME)
        .value("FORETHOUGHT", CardSelectTask::FORETHOUGHT)
        .value("GAMBLE", CardSelectTask::GAMBLE)
        .value("HEADBUTT", CardSelectTask::HEADBUTT)
        .value("HOLOGRAM", CardSelectTask::HOLOGRAM)
        .value("LIQUID_MEMORIES_POTION", CardSelectTask::LIQUID_MEMORIES_POTION)
        .value("MEDITATE", CardSelectTask::MEDITATE)
        .value("NIGHTMARE", CardSelectTask::NIGHTMARE)
        .value("RECYCLE", CardSelectTask::RECYCLE)
        .value("SECRET_TECHNIQUE", CardSelectTask::SECRET_TECHNIQUE)
        .value("SECRET_WEAPON", CardSelectTask::SECRET_WEAPON)
        .value("SEEK", CardSelectTask::SEEK)
        .value("SETUP", CardSelectTask::SETUP)
        .value("WARCRY", CardSelectTask::WARCRY);

    screenState.value("INVALID", ScreenState::INVALID)
        .value("EVENT_SCREEN", ScreenState::EVENT_SCREEN)
        .value("REWARDS", ScreenState::REWARDS)
        .value("BOSS_RELIC_REWARDS", ScreenState::BOSS_RELIC_REWARDS)
        .value("CARD_SELECT", ScreenState::CARD_SELECT)
        .value("MAP_SCREEN", ScreenState::MAP_SCREEN)
        .value("TREASURE_ROOM", ScreenState::TREASURE_ROOM)
        .value("REST_ROOM", ScreenState::REST_ROOM)
        .value("SHOP_ROOM", ScreenState::SHOP_ROOM)
        .value("BATTLE", ScreenState::BATTLE);

    characterClass.value("IRONCLAD", CharacterClass::IRONCLAD)
        .value("SILENT", CharacterClass::SILENT)
        .value("DEFECT", CharacterClass::DEFECT)
        .value("WATCHER", CharacterClass::WATCHER)
        .value("INVALID", CharacterClass::INVALID);

    roomEnum.value("SHOP", Room::SHOP)
        .value("REST", Room::REST)
        .value("EVENT", Room::EVENT)
        .value("ELITE", Room::ELITE)
        .value("MONSTER", Room::MONSTER)
        .value("TREASURE", Room::TREASURE)
        .value("BOSS", Room::BOSS)
        .value("BOSS_TREASURE", Room::BOSS_TREASURE)
        .value("NONE", Room::NONE)
        .value("INVALID", Room::INVALID);

    cardRarity.value("COMMON", CardRarity::COMMON)
        .value("UNCOMMON", CardRarity::UNCOMMON)
        .value("RARE", CardRarity::RARE)
        .value("BASIC", CardRarity::BASIC)
        .value("SPECIAL", CardRarity::SPECIAL)
        .value("CURSE", CardRarity::CURSE)
        .value("INVALID", CardRarity::INVALID);

    cardColor.value("RED", CardColor::RED)
        .value("GREEN", CardColor::GREEN)
        .value("PURPLE", CardColor::PURPLE)
        .value("COLORLESS", CardColor::COLORLESS)
        .value("CURSE", CardColor::CURSE)
        .value("INVALID", CardColor::INVALID);

    cardType.value("ATTACK", CardType::ATTACK)
        .value("SKILL", CardType::SKILL)
        .value("POWER", CardType::POWER)
        .value("CURSE", CardType::CURSE)
        .value("STATUS", CardType::STATUS)
        .value("INVALID", CardType::INVALID);

    cardId.value("INVALID", CardId::INVALID)
        .value("ACCURACY", CardId::ACCURACY)
        .value("ACROBATICS", CardId::ACROBATICS)
        .value("ADRENALINE", CardId::ADRENALINE)
        .value("AFTER_IMAGE", CardId::AFTER_IMAGE)
        .value("AGGREGATE", CardId::AGGREGATE)
        .value("ALCHEMIZE", CardId::ALCHEMIZE)
        .value("ALL_FOR_ONE", CardId::ALL_FOR_ONE)
        .value("ALL_OUT_ATTACK", CardId::ALL_OUT_ATTACK)
        .value("ALPHA", CardId::ALPHA)
        .value("AMPLIFY", CardId::AMPLIFY)
        .value("ANGER", CardId::ANGER)
        .value("APOTHEOSIS", CardId::APOTHEOSIS)
        .value("APPARITION", CardId::APPARITION)
        .value("ARMAMENTS", CardId::ARMAMENTS)
        .value("ASCENDERS_BANE", CardId::ASCENDERS_BANE)
        .value("AUTO_SHIELDS", CardId::AUTO_SHIELDS)
        .value("A_THOUSAND_CUTS", CardId::A_THOUSAND_CUTS)
        .value("BACKFLIP", CardId::BACKFLIP)
        .value("BACKSTAB", CardId::BACKSTAB)
        .value("BALL_LIGHTNING", CardId::BALL_LIGHTNING)
        .value("BANDAGE_UP", CardId::BANDAGE_UP)
        .value("BANE", CardId::BANE)
        .value("BARRAGE", CardId::BARRAGE)
        .value("BARRICADE", CardId::BARRICADE)
        .value("BASH", CardId::BASH)
        .value("BATTLE_HYMN", CardId::BATTLE_HYMN)
        .value("BATTLE_TRANCE", CardId::BATTLE_TRANCE)
        .value("BEAM_CELL", CardId::BEAM_CELL)
        .value("BECOME_ALMIGHTY", CardId::BECOME_ALMIGHTY)
        .value("BERSERK", CardId::BERSERK)
        .value("BETA", CardId::BETA)
        .value("BIASED_COGNITION", CardId::BIASED_COGNITION)
        .value("BITE", CardId::BITE)
        .value("BLADE_DANCE", CardId::BLADE_DANCE)
        .value("BLASPHEMY", CardId::BLASPHEMY)
        .value("BLIND", CardId::BLIND)
        .value("BLIZZARD", CardId::BLIZZARD)
        .value("BLOODLETTING", CardId::BLOODLETTING)
        .value("BLOOD_FOR_BLOOD", CardId::BLOOD_FOR_BLOOD)
        .value("BLUDGEON", CardId::BLUDGEON)
        .value("BLUR", CardId::BLUR)
        .value("BODY_SLAM", CardId::BODY_SLAM)
        .value("BOOT_SEQUENCE", CardId::BOOT_SEQUENCE)
        .value("BOUNCING_FLASK", CardId::BOUNCING_FLASK)
        .value("BOWLING_BASH", CardId::BOWLING_BASH)
        .value("BRILLIANCE", CardId::BRILLIANCE)
        .value("BRUTALITY", CardId::BRUTALITY)
        .value("BUFFER", CardId::BUFFER)
        .value("BULLET_TIME", CardId::BULLET_TIME)
        .value("BULLSEYE", CardId::BULLSEYE)
        .value("BURN", CardId::BURN)
        .value("BURNING_PACT", CardId::BURNING_PACT)
        .value("BURST", CardId::BURST)
        .value("CALCULATED_GAMBLE", CardId::CALCULATED_GAMBLE)
        .value("CALTROPS", CardId::CALTROPS)
        .value("CAPACITOR", CardId::CAPACITOR)
        .value("CARNAGE", CardId::CARNAGE)
        .value("CARVE_REALITY", CardId::CARVE_REALITY)
        .value("CATALYST", CardId::CATALYST)
        .value("CHAOS", CardId::CHAOS)
        .value("CHARGE_BATTERY", CardId::CHARGE_BATTERY)
        .value("CHILL", CardId::CHILL)
        .value("CHOKE", CardId::CHOKE)
        .value("CHRYSALIS", CardId::CHRYSALIS)
        .value("CLASH", CardId::CLASH)
        .value("CLAW", CardId::CLAW)
        .value("CLEAVE", CardId::CLEAVE)
        .value("CLOAK_AND_DAGGER", CardId::CLOAK_AND_DAGGER)
        .value("CLOTHESLINE", CardId::CLOTHESLINE)
        .value("CLUMSY", CardId::CLUMSY)
        .value("COLD_SNAP", CardId::COLD_SNAP)
        .value("COLLECT", CardId::COLLECT)
        .value("COMBUST", CardId::COMBUST)
        .value("COMPILE_DRIVER", CardId::COMPILE_DRIVER)
        .value("CONCENTRATE", CardId::CONCENTRATE)
        .value("CONCLUDE", CardId::CONCLUDE)
        .value("CONJURE_BLADE", CardId::CONJURE_BLADE)
        .value("CONSECRATE", CardId::CONSECRATE)
        .value("CONSUME", CardId::CONSUME)
        .value("COOLHEADED", CardId::COOLHEADED)
        .value("CORE_SURGE", CardId::CORE_SURGE)
        .value("CORPSE_EXPLOSION", CardId::CORPSE_EXPLOSION)
        .value("CORRUPTION", CardId::CORRUPTION)
        .value("CREATIVE_AI", CardId::CREATIVE_AI)
        .value("CRESCENDO", CardId::CRESCENDO)
        .value("CRIPPLING_CLOUD", CardId::CRIPPLING_CLOUD)
        .value("CRUSH_JOINTS", CardId::CRUSH_JOINTS)
        .value("CURSE_OF_THE_BELL", CardId::CURSE_OF_THE_BELL)
        .value("CUT_THROUGH_FATE", CardId::CUT_THROUGH_FATE)
        .value("DAGGER_SPRAY", CardId::DAGGER_SPRAY)
        .value("DAGGER_THROW", CardId::DAGGER_THROW)
        .value("DARKNESS", CardId::DARKNESS)
        .value("DARK_EMBRACE", CardId::DARK_EMBRACE)
        .value("DARK_SHACKLES", CardId::DARK_SHACKLES)
        .value("DASH", CardId::DASH)
        .value("DAZED", CardId::DAZED)
        .value("DEADLY_POISON", CardId::DEADLY_POISON)
        .value("DECAY", CardId::DECAY)
        .value("DECEIVE_REALITY", CardId::DECEIVE_REALITY)
        .value("DEEP_BREATH", CardId::DEEP_BREATH)
        .value("DEFEND_BLUE", CardId::DEFEND_BLUE)
        .value("DEFEND_GREEN", CardId::DEFEND_GREEN)
        .value("DEFEND_PURPLE", CardId::DEFEND_PURPLE)
        .value("DEFEND_RED", CardId::DEFEND_RED)
        .value("DEFLECT", CardId::DEFLECT)
        .value("DEFRAGMENT", CardId::DEFRAGMENT)
        .value("DEMON_FORM", CardId::DEMON_FORM)
        .value("DEUS_EX_MACHINA", CardId::DEUS_EX_MACHINA)
        .value("DEVA_FORM", CardId::DEVA_FORM)
        .value("DEVOTION", CardId::DEVOTION)
        .value("DIE_DIE_DIE", CardId::DIE_DIE_DIE)
        .value("DISARM", CardId::DISARM)
        .value("DISCOVERY", CardId::DISCOVERY)
        .value("DISTRACTION", CardId::DISTRACTION)
        .value("DODGE_AND_ROLL", CardId::DODGE_AND_ROLL)
        .value("DOOM_AND_GLOOM", CardId::DOOM_AND_GLOOM)
        .value("DOPPELGANGER", CardId::DOPPELGANGER)
        .value("DOUBLE_ENERGY", CardId::DOUBLE_ENERGY)
        .value("DOUBLE_TAP", CardId::DOUBLE_TAP)
        .value("DOUBT", CardId::DOUBT)
        .value("DRAMATIC_ENTRANCE", CardId::DRAMATIC_ENTRANCE)
        .value("DROPKICK", CardId::DROPKICK)
        .value("DUALCAST", CardId::DUALCAST)
        .value("DUAL_WIELD", CardId::DUAL_WIELD)
        .value("ECHO_FORM", CardId::ECHO_FORM)
        .value("ELECTRODYNAMICS", CardId::ELECTRODYNAMICS)
        .value("EMPTY_BODY", CardId::EMPTY_BODY)
        .value("EMPTY_FIST", CardId::EMPTY_FIST)
        .value("EMPTY_MIND", CardId::EMPTY_MIND)
        .value("ENDLESS_AGONY", CardId::ENDLESS_AGONY)
        .value("ENLIGHTENMENT", CardId::ENLIGHTENMENT)
        .value("ENTRENCH", CardId::ENTRENCH)
        .value("ENVENOM", CardId::ENVENOM)
        .value("EQUILIBRIUM", CardId::EQUILIBRIUM)
        .value("ERUPTION", CardId::ERUPTION)
        .value("ESCAPE_PLAN", CardId::ESCAPE_PLAN)
        .value("ESTABLISHMENT", CardId::ESTABLISHMENT)
        .value("EVALUATE", CardId::EVALUATE)
        .value("EVISCERATE", CardId::EVISCERATE)
        .value("EVOLVE", CardId::EVOLVE)
        .value("EXHUME", CardId::EXHUME)
        .value("EXPERTISE", CardId::EXPERTISE)
        .value("EXPUNGER", CardId::EXPUNGER)
        .value("FAME_AND_FORTUNE", CardId::FAME_AND_FORTUNE)
        .value("FASTING", CardId::FASTING)
        .value("FEAR_NO_EVIL", CardId::FEAR_NO_EVIL)
        .value("FEED", CardId::FEED)
        .value("FEEL_NO_PAIN", CardId::FEEL_NO_PAIN)
        .value("FIEND_FIRE", CardId::FIEND_FIRE)
        .value("FINESSE", CardId::FINESSE)
        .value("FINISHER", CardId::FINISHER)
        .value("FIRE_BREATHING", CardId::FIRE_BREATHING)
        .value("FISSION", CardId::FISSION)
        .value("FLAME_BARRIER", CardId::FLAME_BARRIER)
        .value("FLASH_OF_STEEL", CardId::FLASH_OF_STEEL)
        .value("FLECHETTES", CardId::FLECHETTES)
        .value("FLEX", CardId::FLEX)
        .value("FLURRY_OF_BLOWS", CardId::FLURRY_OF_BLOWS)
        .value("FLYING_KNEE", CardId::FLYING_KNEE)
        .value("FLYING_SLEEVES", CardId::FLYING_SLEEVES)
        .value("FOLLOW_UP", CardId::FOLLOW_UP)
        .value("FOOTWORK", CardId::FOOTWORK)
        .value("FORCE_FIELD", CardId::FORCE_FIELD)
        .value("FOREIGN_INFLUENCE", CardId::FOREIGN_INFLUENCE)
        .value("FORESIGHT", CardId::FORESIGHT)
        .value("FORETHOUGHT", CardId::FORETHOUGHT)
        .value("FTL", CardId::FTL)
        .value("FUSION", CardId::FUSION)
        .value("GENETIC_ALGORITHM", CardId::GENETIC_ALGORITHM)
        .value("GHOSTLY_ARMOR", CardId::GHOSTLY_ARMOR)
        .value("GLACIER", CardId::GLACIER)
        .value("GLASS_KNIFE", CardId::GLASS_KNIFE)
        .value("GOOD_INSTINCTS", CardId::GOOD_INSTINCTS)
        .value("GO_FOR_THE_EYES", CardId::GO_FOR_THE_EYES)
        .value("GRAND_FINALE", CardId::GRAND_FINALE)
        .value("HALT", CardId::HALT)
        .value("HAND_OF_GREED", CardId::HAND_OF_GREED)
        .value("HAVOC", CardId::HAVOC)
        .value("HEADBUTT", CardId::HEADBUTT)
        .value("HEATSINKS", CardId::HEATSINKS)
        .value("HEAVY_BLADE", CardId::HEAVY_BLADE)
        .value("HEEL_HOOK", CardId::HEEL_HOOK)
        .value("HELLO_WORLD", CardId::HELLO_WORLD)
        .value("HEMOKINESIS", CardId::HEMOKINESIS)
        .value("HOLOGRAM", CardId::HOLOGRAM)
        .value("HYPERBEAM", CardId::HYPERBEAM)
        .value("IMMOLATE", CardId::IMMOLATE)
        .value("IMPATIENCE", CardId::IMPATIENCE)
        .value("IMPERVIOUS", CardId::IMPERVIOUS)
        .value("INDIGNATION", CardId::INDIGNATION)
        .value("INFERNAL_BLADE", CardId::INFERNAL_BLADE)
        .value("INFINITE_BLADES", CardId::INFINITE_BLADES)
        .value("INFLAME", CardId::INFLAME)
        .value("INJURY", CardId::INJURY)
        .value("INNER_PEACE", CardId::INNER_PEACE)
        .value("INSIGHT", CardId::INSIGHT)
        .value("INTIMIDATE", CardId::INTIMIDATE)
        .value("IRON_WAVE", CardId::IRON_WAVE)
        .value("JAX", CardId::JAX)
        .value("JACK_OF_ALL_TRADES", CardId::JACK_OF_ALL_TRADES)
        .value("JUDGMENT", CardId::JUDGMENT)
        .value("JUGGERNAUT", CardId::JUGGERNAUT)
        .value("JUST_LUCKY", CardId::JUST_LUCKY)
        .value("LEAP", CardId::LEAP)
        .value("LEG_SWEEP", CardId::LEG_SWEEP)
        .value("LESSON_LEARNED", CardId::LESSON_LEARNED)
        .value("LIKE_WATER", CardId::LIKE_WATER)
        .value("LIMIT_BREAK", CardId::LIMIT_BREAK)
        .value("LIVE_FOREVER", CardId::LIVE_FOREVER)
        .value("LOOP", CardId::LOOP)
        .value("MACHINE_LEARNING", CardId::MACHINE_LEARNING)
        .value("MADNESS", CardId::MADNESS)
        .value("MAGNETISM", CardId::MAGNETISM)
        .value("MALAISE", CardId::MALAISE)
        .value("MASTERFUL_STAB", CardId::MASTERFUL_STAB)
        .value("MASTER_OF_STRATEGY", CardId::MASTER_OF_STRATEGY)
        .value("MASTER_REALITY", CardId::MASTER_REALITY)
        .value("MAYHEM", CardId::MAYHEM)
        .value("MEDITATE", CardId::MEDITATE)
        .value("MELTER", CardId::MELTER)
        .value("MENTAL_FORTRESS", CardId::MENTAL_FORTRESS)
        .value("METALLICIZE", CardId::METALLICIZE)
        .value("METAMORPHOSIS", CardId::METAMORPHOSIS)
        .value("METEOR_STRIKE", CardId::METEOR_STRIKE)
        .value("MIND_BLAST", CardId::MIND_BLAST)
        .value("MIRACLE", CardId::MIRACLE)
        .value("MULTI_CAST", CardId::MULTI_CAST)
        .value("NECRONOMICURSE", CardId::NECRONOMICURSE)
        .value("NEUTRALIZE", CardId::NEUTRALIZE)
        .value("NIGHTMARE", CardId::NIGHTMARE)
        .value("NIRVANA", CardId::NIRVANA)
        .value("NORMALITY", CardId::NORMALITY)
        .value("NOXIOUS_FUMES", CardId::NOXIOUS_FUMES)
        .value("OFFERING", CardId::OFFERING)
        .value("OMEGA", CardId::OMEGA)
        .value("OMNISCIENCE", CardId::OMNISCIENCE)
        .value("OUTMANEUVER", CardId::OUTMANEUVER)
        .value("OVERCLOCK", CardId::OVERCLOCK)
        .value("PAIN", CardId::PAIN)
        .value("PANACEA", CardId::PANACEA)
        .value("PANACHE", CardId::PANACHE)
        .value("PANIC_BUTTON", CardId::PANIC_BUTTON)
        .value("PARASITE", CardId::PARASITE)
        .value("PERFECTED_STRIKE", CardId::PERFECTED_STRIKE)
        .value("PERSEVERANCE", CardId::PERSEVERANCE)
        .value("PHANTASMAL_KILLER", CardId::PHANTASMAL_KILLER)
        .value("PIERCING_WAIL", CardId::PIERCING_WAIL)
        .value("POISONED_STAB", CardId::POISONED_STAB)
        .value("POMMEL_STRIKE", CardId::POMMEL_STRIKE)
        .value("POWER_THROUGH", CardId::POWER_THROUGH)
        .value("PRAY", CardId::PRAY)
        .value("PREDATOR", CardId::PREDATOR)
        .value("PREPARED", CardId::PREPARED)
        .value("PRESSURE_POINTS", CardId::PRESSURE_POINTS)
        .value("PRIDE", CardId::PRIDE)
        .value("PROSTRATE", CardId::PROSTRATE)
        .value("PROTECT", CardId::PROTECT)
        .value("PUMMEL", CardId::PUMMEL)
        .value("PURITY", CardId::PURITY)
        .value("QUICK_SLASH", CardId::QUICK_SLASH)
        .value("RAGE", CardId::RAGE)
        .value("RAGNAROK", CardId::RAGNAROK)
        .value("RAINBOW", CardId::RAINBOW)
        .value("RAMPAGE", CardId::RAMPAGE)
        .value("REACH_HEAVEN", CardId::REACH_HEAVEN)
        .value("REAPER", CardId::REAPER)
        .value("REBOOT", CardId::REBOOT)
        .value("REBOUND", CardId::REBOUND)
        .value("RECKLESS_CHARGE", CardId::RECKLESS_CHARGE)
        .value("RECURSION", CardId::RECURSION)
        .value("RECYCLE", CardId::RECYCLE)
        .value("REFLEX", CardId::REFLEX)
        .value("REGRET", CardId::REGRET)
        .value("REINFORCED_BODY", CardId::REINFORCED_BODY)
        .value("REPROGRAM", CardId::REPROGRAM)
        .value("RIDDLE_WITH_HOLES", CardId::RIDDLE_WITH_HOLES)
        .value("RIP_AND_TEAR", CardId::RIP_AND_TEAR)
        .value("RITUAL_DAGGER", CardId::RITUAL_DAGGER)
        .value("RUPTURE", CardId::RUPTURE)
        .value("RUSHDOWN", CardId::RUSHDOWN)
        .value("SADISTIC_NATURE", CardId::SADISTIC_NATURE)
        .value("SAFETY", CardId::SAFETY)
        .value("SANCTITY", CardId::SANCTITY)
        .value("SANDS_OF_TIME", CardId::SANDS_OF_TIME)
        .value("SASH_WHIP", CardId::SASH_WHIP)
        .value("SCRAPE", CardId::SCRAPE)
        .value("SCRAWL", CardId::SCRAWL)
        .value("SEARING_BLOW", CardId::SEARING_BLOW)
        .value("SECOND_WIND", CardId::SECOND_WIND)
        .value("SECRET_TECHNIQUE", CardId::SECRET_TECHNIQUE)
        .value("SECRET_WEAPON", CardId::SECRET_WEAPON)
        .value("SEEING_RED", CardId::SEEING_RED)
        .value("SEEK", CardId::SEEK)
        .value("SELF_REPAIR", CardId::SELF_REPAIR)
        .value("SENTINEL", CardId::SENTINEL)
        .value("SETUP", CardId::SETUP)
        .value("SEVER_SOUL", CardId::SEVER_SOUL)
        .value("SHAME", CardId::SHAME)
        .value("SHIV", CardId::SHIV)
        .value("SHOCKWAVE", CardId::SHOCKWAVE)
        .value("SHRUG_IT_OFF", CardId::SHRUG_IT_OFF)
        .value("SIGNATURE_MOVE", CardId::SIGNATURE_MOVE)
        .value("SIMMERING_FURY", CardId::SIMMERING_FURY)
        .value("SKEWER", CardId::SKEWER)
        .value("SKIM", CardId::SKIM)
        .value("SLICE", CardId::SLICE)
        .value("SLIMED", CardId::SLIMED)
        .value("SMITE", CardId::SMITE)
        .value("SNEAKY_STRIKE", CardId::SNEAKY_STRIKE)
        .value("SPIRIT_SHIELD", CardId::SPIRIT_SHIELD)
        .value("SPOT_WEAKNESS", CardId::SPOT_WEAKNESS)
        .value("STACK", CardId::STACK)
        .value("STATIC_DISCHARGE", CardId::STATIC_DISCHARGE)
        .value("STEAM_BARRIER", CardId::STEAM_BARRIER)
        .value("STORM", CardId::STORM)
        .value("STORM_OF_STEEL", CardId::STORM_OF_STEEL)
        .value("STREAMLINE", CardId::STREAMLINE)
        .value("STRIKE_BLUE", CardId::STRIKE_BLUE)
        .value("STRIKE_GREEN", CardId::STRIKE_GREEN)
        .value("STRIKE_PURPLE", CardId::STRIKE_PURPLE)
        .value("STRIKE_RED", CardId::STRIKE_RED)
        .value("STUDY", CardId::STUDY)
        .value("SUCKER_PUNCH", CardId::SUCKER_PUNCH)
        .value("SUNDER", CardId::SUNDER)
        .value("SURVIVOR", CardId::SURVIVOR)
        .value("SWEEPING_BEAM", CardId::SWEEPING_BEAM)
        .value("SWIFT_STRIKE", CardId::SWIFT_STRIKE)
        .value("SWIVEL", CardId::SWIVEL)
        .value("SWORD_BOOMERANG", CardId::SWORD_BOOMERANG)
        .value("TACTICIAN", CardId::TACTICIAN)
        .value("TALK_TO_THE_HAND", CardId::TALK_TO_THE_HAND)
        .value("TANTRUM", CardId::TANTRUM)
        .value("TEMPEST", CardId::TEMPEST)
        .value("TERROR", CardId::TERROR)
        .value("THE_BOMB", CardId::THE_BOMB)
        .value("THINKING_AHEAD", CardId::THINKING_AHEAD)
        .value("THIRD_EYE", CardId::THIRD_EYE)
        .value("THROUGH_VIOLENCE", CardId::THROUGH_VIOLENCE)
        .value("THUNDERCLAP", CardId::THUNDERCLAP)
        .value("THUNDER_STRIKE", CardId::THUNDER_STRIKE)
        .value("TOOLS_OF_THE_TRADE", CardId::TOOLS_OF_THE_TRADE)
        .value("TRANQUILITY", CardId::TRANQUILITY)
        .value("TRANSMUTATION", CardId::TRANSMUTATION)
        .value("TRIP", CardId::TRIP)
        .value("TRUE_GRIT", CardId::TRUE_GRIT)
        .value("TURBO", CardId::TURBO)
        .value("TWIN_STRIKE", CardId::TWIN_STRIKE)
        .value("UNLOAD", CardId::UNLOAD)
        .value("UPPERCUT", CardId::UPPERCUT)
        .value("VAULT", CardId::VAULT)
        .value("VIGILANCE", CardId::VIGILANCE)
        .value("VIOLENCE", CardId::VIOLENCE)
        .value("VOID", CardId::VOID)
        .value("WALLOP", CardId::WALLOP)
        .value("WARCRY", CardId::WARCRY)
        .value("WAVE_OF_THE_HAND", CardId::WAVE_OF_THE_HAND)
        .value("WEAVE", CardId::WEAVE)
        .value("WELL_LAID_PLANS", CardId::WELL_LAID_PLANS)
        .value("WHEEL_KICK", CardId::WHEEL_KICK)
        .value("WHIRLWIND", CardId::WHIRLWIND)
        .value("WHITE_NOISE", CardId::WHITE_NOISE)
        .value("WILD_STRIKE", CardId::WILD_STRIKE)
        .value("WINDMILL_STRIKE", CardId::WINDMILL_STRIKE)
        .value("WISH", CardId::WISH)
        .value("WORSHIP", CardId::WORSHIP)
        .value("WOUND", CardId::WOUND)
        .value("WRAITH_FORM", CardId::WRAITH_FORM)
        .value("WREATH_OF_FLAME", CardId::WREATH_OF_FLAME)
        .value("WRITHE", CardId::WRITHE)
        .value("ZAP", CardId::ZAP);

    meEnum.value("INVALID", ME::INVALID)
        .value("CULTIST", ME::CULTIST)
        .value("JAW_WORM", ME::JAW_WORM)
        .value("TWO_LOUSE", ME::TWO_LOUSE)
        .value("SMALL_SLIMES", ME::SMALL_SLIMES)
        .value("BLUE_SLAVER", ME::BLUE_SLAVER)
        .value("GREMLIN_GANG", ME::GREMLIN_GANG)
        .value("LOOTER", ME::LOOTER)
        .value("LARGE_SLIME", ME::LARGE_SLIME)
        .value("LOTS_OF_SLIMES", ME::LOTS_OF_SLIMES)
        .value("EXORDIUM_THUGS", ME::EXORDIUM_THUGS)
        .value("EXORDIUM_WILDLIFE", ME::EXORDIUM_WILDLIFE)
        .value("RED_SLAVER", ME::RED_SLAVER)
        .value("THREE_LOUSE", ME::THREE_LOUSE)
        .value("TWO_FUNGI_BEASTS", ME::TWO_FUNGI_BEASTS)
        .value("GREMLIN_NOB", ME::GREMLIN_NOB)
        .value("LAGAVULIN", ME::LAGAVULIN)
        .value("THREE_SENTRIES", ME::THREE_SENTRIES)
        .value("SLIME_BOSS", ME::SLIME_BOSS)
        .value("THE_GUARDIAN", ME::THE_GUARDIAN)
        .value("HEXAGHOST", ME::HEXAGHOST)
        .value("SPHERIC_GUARDIAN", ME::SPHERIC_GUARDIAN)
        .value("CHOSEN", ME::CHOSEN)
        .value("SHELL_PARASITE", ME::SHELL_PARASITE)
        .value("THREE_BYRDS", ME::THREE_BYRDS)
        .value("TWO_THIEVES", ME::TWO_THIEVES)
        .value("CHOSEN_AND_BYRDS", ME::CHOSEN_AND_BYRDS)
        .value("SENTRY_AND_SPHERE", ME::SENTRY_AND_SPHERE)
        .value("SNAKE_PLANT", ME::SNAKE_PLANT)
        .value("SNECKO", ME::SNECKO)
        .value("CENTURION_AND_HEALER", ME::CENTURION_AND_HEALER)
        .value("CULTIST_AND_CHOSEN", ME::CULTIST_AND_CHOSEN)
        .value("THREE_CULTIST", ME::THREE_CULTIST)
        .value("SHELLED_PARASITE_AND_FUNGI", ME::SHELLED_PARASITE_AND_FUNGI)
        .value("GREMLIN_LEADER", ME::GREMLIN_LEADER)
        .value("SLAVERS", ME::SLAVERS)
        .value("BOOK_OF_STABBING", ME::BOOK_OF_STABBING)
        .value("AUTOMATON", ME::AUTOMATON)
        .value("COLLECTOR", ME::COLLECTOR)
        .value("CHAMP", ME::CHAMP)
        .value("THREE_DARKLINGS", ME::THREE_DARKLINGS)
        .value("ORB_WALKER", ME::ORB_WALKER)
        .value("THREE_SHAPES", ME::THREE_SHAPES)
        .value("SPIRE_GROWTH", ME::SPIRE_GROWTH)
        .value("TRANSIENT", ME::TRANSIENT)
        .value("FOUR_SHAPES", ME::FOUR_SHAPES)
        .value("MAW", ME::MAW)
        .value("SPHERE_AND_TWO_SHAPES", ME::SPHERE_AND_TWO_SHAPES)
        .value("JAW_WORM_HORDE", ME::JAW_WORM_HORDE)
        .value("WRITHING_MASS", ME::WRITHING_MASS)
        .value("GIANT_HEAD", ME::GIANT_HEAD)
        .value("NEMESIS", ME::NEMESIS)
        .value("REPTOMANCER", ME::REPTOMANCER)
        .value("AWAKENED_ONE", ME::AWAKENED_ONE)
        .value("TIME_EATER", ME::TIME_EATER)
        .value("DONU_AND_DECA", ME::DONU_AND_DECA)
        .value("SHIELD_AND_SPEAR", ME::SHIELD_AND_SPEAR)
        .value("THE_HEART", ME::THE_HEART)
        .value("LAGAVULIN_EVENT", ME::LAGAVULIN_EVENT)
        .value("COLOSSEUM_EVENT_SLAVERS", ME::COLOSSEUM_EVENT_SLAVERS)
        .value("COLOSSEUM_EVENT_NOBS", ME::COLOSSEUM_EVENT_NOBS)
        .value("MASKED_BANDITS_EVENT", ME::MASKED_BANDITS_EVENT)
        .value("MUSHROOMS_EVENT", ME::MUSHROOMS_EVENT)
        .value("MYSTERIOUS_SPHERE_EVENT", ME::MYSTERIOUS_SPHERE_EVENT);

    relicEnum.value("AKABEKO", RelicId::AKABEKO)
        .value("ART_OF_WAR", RelicId::ART_OF_WAR)
        .value("BIRD_FACED_URN", RelicId::BIRD_FACED_URN)
        .value("BLOODY_IDOL", RelicId::BLOODY_IDOL)
        .value("BLUE_CANDLE", RelicId::BLUE_CANDLE)
        .value("BRIMSTONE", RelicId::BRIMSTONE)
        .value("CALIPERS", RelicId::CALIPERS)
        .value("CAPTAINS_WHEEL", RelicId::CAPTAINS_WHEEL)
        .value("CENTENNIAL_PUZZLE", RelicId::CENTENNIAL_PUZZLE)
        .value("CERAMIC_FISH", RelicId::CERAMIC_FISH)
        .value("CHAMPION_BELT", RelicId::CHAMPION_BELT)
        .value("CHARONS_ASHES", RelicId::CHARONS_ASHES)
        .value("CHEMICAL_X", RelicId::CHEMICAL_X)
        .value("CLOAK_CLASP", RelicId::CLOAK_CLASP)
        .value("DARKSTONE_PERIAPT", RelicId::DARKSTONE_PERIAPT)
        .value("DEAD_BRANCH", RelicId::DEAD_BRANCH)
        .value("DUALITY", RelicId::DUALITY)
        .value("ECTOPLASM", RelicId::ECTOPLASM)
        .value("EMOTION_CHIP", RelicId::EMOTION_CHIP)
        .value("FROZEN_CORE", RelicId::FROZEN_CORE)
        .value("FROZEN_EYE", RelicId::FROZEN_EYE)
        .value("GAMBLING_CHIP", RelicId::GAMBLING_CHIP)
        .value("GINGER", RelicId::GINGER)
        .value("GOLDEN_EYE", RelicId::GOLDEN_EYE)
        .value("GREMLIN_HORN", RelicId::GREMLIN_HORN)
        .value("HAND_DRILL", RelicId::HAND_DRILL)
        .value("HAPPY_FLOWER", RelicId::HAPPY_FLOWER)
        .value("HORN_CLEAT", RelicId::HORN_CLEAT)
        .value("HOVERING_KITE", RelicId::HOVERING_KITE)
        .value("ICE_CREAM", RelicId::ICE_CREAM)
        .value("INCENSE_BURNER", RelicId::INCENSE_BURNER)
        .value("INK_BOTTLE", RelicId::INK_BOTTLE)
        .value("INSERTER", RelicId::INSERTER)
        .value("KUNAI", RelicId::KUNAI)
        .value("LETTER_OPENER", RelicId::LETTER_OPENER)
        .value("LIZARD_TAIL", RelicId::LIZARD_TAIL)
        .value("MAGIC_FLOWER", RelicId::MAGIC_FLOWER)
        .value("MARK_OF_THE_BLOOM", RelicId::MARK_OF_THE_BLOOM)
        .value("MEDICAL_KIT", RelicId::MEDICAL_KIT)
        .value("MELANGE", RelicId::MELANGE)
        .value("MERCURY_HOURGLASS", RelicId::MERCURY_HOURGLASS)
        .value("MUMMIFIED_HAND", RelicId::MUMMIFIED_HAND)
        .value("NECRONOMICON", RelicId::NECRONOMICON)
        .value("NILRYS_CODEX", RelicId::NILRYS_CODEX)
        .value("NUNCHAKU", RelicId::NUNCHAKU)
        .value("ODD_MUSHROOM", RelicId::ODD_MUSHROOM)
        .value("OMAMORI", RelicId::OMAMORI)
        .value("ORANGE_PELLETS", RelicId::ORANGE_PELLETS)
        .value("ORICHALCUM", RelicId::ORICHALCUM)
        .value("ORNAMENTAL_FAN", RelicId::ORNAMENTAL_FAN)
        .value("PAPER_KRANE", RelicId::PAPER_KRANE)
        .value("PAPER_PHROG", RelicId::PAPER_PHROG)
        .value("PEN_NIB", RelicId::PEN_NIB)
        .value("PHILOSOPHERS_STONE", RelicId::PHILOSOPHERS_STONE)
        .value("POCKETWATCH", RelicId::POCKETWATCH)
        .value("RED_SKULL", RelicId::RED_SKULL)
        .value("RUNIC_CUBE", RelicId::RUNIC_CUBE)
        .value("RUNIC_DOME", RelicId::RUNIC_DOME)
        .value("RUNIC_PYRAMID", RelicId::RUNIC_PYRAMID)
        .value("SACRED_BARK", RelicId::SACRED_BARK)
        .value("SELF_FORMING_CLAY", RelicId::SELF_FORMING_CLAY)
        .value("SHURIKEN", RelicId::SHURIKEN)
        .value("SNECKO_EYE", RelicId::SNECKO_EYE)
        .value("SNECKO_SKULL", RelicId::SNECKO_SKULL)
        .value("SOZU", RelicId::SOZU)
        .value("STONE_CALENDAR", RelicId::STONE_CALENDAR)
        .value("STRANGE_SPOON", RelicId::STRANGE_SPOON)
        .value("STRIKE_DUMMY", RelicId::STRIKE_DUMMY)
        .value("SUNDIAL", RelicId::SUNDIAL)
        .value("THE_ABACUS", RelicId::THE_ABACUS)
        .value("THE_BOOT", RelicId::THE_BOOT)
        .value("THE_SPECIMEN", RelicId::THE_SPECIMEN)
        .value("TINGSHA", RelicId::TINGSHA)
        .value("TOOLBOX", RelicId::TOOLBOX)
        .value("TORII", RelicId::TORII)
        .value("TOUGH_BANDAGES", RelicId::TOUGH_BANDAGES)
        .value("TOY_ORNITHOPTER", RelicId::TOY_ORNITHOPTER)
        .value("TUNGSTEN_ROD", RelicId::TUNGSTEN_ROD)
        .value("TURNIP", RelicId::TURNIP)
        .value("TWISTED_FUNNEL", RelicId::TWISTED_FUNNEL)
        .value("UNCEASING_TOP", RelicId::UNCEASING_TOP)
        .value("VELVET_CHOKER", RelicId::VELVET_CHOKER)
        .value("VIOLET_LOTUS", RelicId::VIOLET_LOTUS)
        .value("WARPED_TONGS", RelicId::WARPED_TONGS)
        .value("WRIST_BLADE", RelicId::WRIST_BLADE)
        .value("BLACK_BLOOD", RelicId::BLACK_BLOOD)
        .value("BURNING_BLOOD", RelicId::BURNING_BLOOD)
        .value("MEAT_ON_THE_BONE", RelicId::MEAT_ON_THE_BONE)
        .value("FACE_OF_CLERIC", RelicId::FACE_OF_CLERIC)
        .value("ANCHOR", RelicId::ANCHOR)
        .value("ANCIENT_TEA_SET", RelicId::ANCIENT_TEA_SET)
        .value("BAG_OF_MARBLES", RelicId::BAG_OF_MARBLES)
        .value("BAG_OF_PREPARATION", RelicId::BAG_OF_PREPARATION)
        .value("BLOOD_VIAL", RelicId::BLOOD_VIAL)
        .value("BOTTLED_FLAME", RelicId::BOTTLED_FLAME)
        .value("BOTTLED_LIGHTNING", RelicId::BOTTLED_LIGHTNING)
        .value("BOTTLED_TORNADO", RelicId::BOTTLED_TORNADO)
        .value("BRONZE_SCALES", RelicId::BRONZE_SCALES)
        .value("BUSTED_CROWN", RelicId::BUSTED_CROWN)
        .value("CLOCKWORK_SOUVENIR", RelicId::CLOCKWORK_SOUVENIR)
        .value("COFFEE_DRIPPER", RelicId::COFFEE_DRIPPER)
        .value("CRACKED_CORE", RelicId::CRACKED_CORE)
        .value("CURSED_KEY", RelicId::CURSED_KEY)
        .value("DAMARU", RelicId::DAMARU)
        .value("DATA_DISK", RelicId::DATA_DISK)
        .value("DU_VU_DOLL", RelicId::DU_VU_DOLL)
        .value("ENCHIRIDION", RelicId::ENCHIRIDION)
        .value("FOSSILIZED_HELIX", RelicId::FOSSILIZED_HELIX)
        .value("FUSION_HAMMER", RelicId::FUSION_HAMMER)
        .value("GIRYA", RelicId::GIRYA)
        .value("GOLD_PLATED_CABLES", RelicId::GOLD_PLATED_CABLES)
        .value("GREMLIN_VISAGE", RelicId::GREMLIN_VISAGE)
        .value("HOLY_WATER", RelicId::HOLY_WATER)
        .value("LANTERN", RelicId::LANTERN)
        .value("MARK_OF_PAIN", RelicId::MARK_OF_PAIN)
        .value("MUTAGENIC_STRENGTH", RelicId::MUTAGENIC_STRENGTH)
        .value("NEOWS_LAMENT", RelicId::NEOWS_LAMENT)
        .value("NINJA_SCROLL", RelicId::NINJA_SCROLL)
        .value("NUCLEAR_BATTERY", RelicId::NUCLEAR_BATTERY)
        .value("ODDLY_SMOOTH_STONE", RelicId::ODDLY_SMOOTH_STONE)
        .value("PANTOGRAPH", RelicId::PANTOGRAPH)
        .value("PRESERVED_INSECT", RelicId::PRESERVED_INSECT)
        .value("PURE_WATER", RelicId::PURE_WATER)
        .value("RED_MASK", RelicId::RED_MASK)
        .value("RING_OF_THE_SERPENT", RelicId::RING_OF_THE_SERPENT)
        .value("RING_OF_THE_SNAKE", RelicId::RING_OF_THE_SNAKE)
        .value("RUNIC_CAPACITOR", RelicId::RUNIC_CAPACITOR)
        .value("SLAVERS_COLLAR", RelicId::SLAVERS_COLLAR)
        .value("SLING_OF_COURAGE", RelicId::SLING_OF_COURAGE)
        .value("SYMBIOTIC_VIRUS", RelicId::SYMBIOTIC_VIRUS)
        .value("TEARDROP_LOCKET", RelicId::TEARDROP_LOCKET)
        .value("THREAD_AND_NEEDLE", RelicId::THREAD_AND_NEEDLE)
        .value("VAJRA", RelicId::VAJRA)
        .value("ASTROLABE", RelicId::ASTROLABE)
        .value("BLACK_STAR", RelicId::BLACK_STAR)
        .value("CALLING_BELL", RelicId::CALLING_BELL)
        .value("CAULDRON", RelicId::CAULDRON)
        .value("CULTIST_HEADPIECE", RelicId::CULTIST_HEADPIECE)
        .value("DOLLYS_MIRROR", RelicId::DOLLYS_MIRROR)
        .value("DREAM_CATCHER", RelicId::DREAM_CATCHER)
        .value("EMPTY_CAGE", RelicId::EMPTY_CAGE)
        .value("ETERNAL_FEATHER", RelicId::ETERNAL_FEATHER)
        .value("FROZEN_EGG", RelicId::FROZEN_EGG)
        .value("GOLDEN_IDOL", RelicId::GOLDEN_IDOL)
        .value("JUZU_BRACELET", RelicId::JUZU_BRACELET)
        .value("LEES_WAFFLE", RelicId::LEES_WAFFLE)
        .value("MANGO", RelicId::MANGO)
        .value("MATRYOSHKA", RelicId::MATRYOSHKA)
        .value("MAW_BANK", RelicId::MAW_BANK)
        .value("MEAL_TICKET", RelicId::MEAL_TICKET)
        .value("MEMBERSHIP_CARD", RelicId::MEMBERSHIP_CARD)
        .value("MOLTEN_EGG", RelicId::MOLTEN_EGG)
        .value("NLOTHS_GIFT", RelicId::NLOTHS_GIFT)
        .value("NLOTHS_HUNGRY_FACE", RelicId::NLOTHS_HUNGRY_FACE)
        .value("OLD_COIN", RelicId::OLD_COIN)
        .value("ORRERY", RelicId::ORRERY)
        .value("PANDORAS_BOX", RelicId::PANDORAS_BOX)
        .value("PEACE_PIPE", RelicId::PEACE_PIPE)
        .value("PEAR", RelicId::PEAR)
        .value("POTION_BELT", RelicId::POTION_BELT)
        .value("PRAYER_WHEEL", RelicId::PRAYER_WHEEL)
        .value("PRISMATIC_SHARD", RelicId::PRISMATIC_SHARD)
        .value("QUESTION_CARD", RelicId::QUESTION_CARD)
        .value("REGAL_PILLOW", RelicId::REGAL_PILLOW)
        .value("SSSERPENT_HEAD", RelicId::SSSERPENT_HEAD)
        .value("SHOVEL", RelicId::SHOVEL)
        .value("SINGING_BOWL", RelicId::SINGING_BOWL)
        .value("SMILING_MASK", RelicId::SMILING_MASK)
        .value("SPIRIT_POOP", RelicId::SPIRIT_POOP)
        .value("STRAWBERRY", RelicId::STRAWBERRY)
        .value("THE_COURIER", RelicId::THE_COURIER)
        .value("TINY_CHEST", RelicId::TINY_CHEST)
        .value("TINY_HOUSE", RelicId::TINY_HOUSE)
        .value("TOXIC_EGG", RelicId::TOXIC_EGG)
        .value("WAR_PAINT", RelicId::WAR_PAINT)
        .value("WHETSTONE", RelicId::WHETSTONE)
        .value("WHITE_BEAST_STATUE", RelicId::WHITE_BEAST_STATUE)
        .value("WING_BOOTS", RelicId::WING_BOOTS)
        .value("CIRCLET", RelicId::CIRCLET)
        .value("RED_CIRCLET", RelicId::RED_CIRCLET)
        .value("INVALID", RelicId::INVALID);

    for (int i = 0; i < (int)(sizeof(potionEnumNames) / sizeof(potionEnumNames[0])); ++i)
    {
        potionEnum.value(potionEnumNames[i], static_cast<Potion>(i));
    }

    // Event 枚举：从 `kEventBindingNames` 铺，顺序 = 枚举值顺序。
    for (const auto &e : kEventBindingNames)
    {
        eventEnum.value(e.first, e.second);
    }

    // PlayerStatus / MonsterStatus / MonsterId / MonsterMoveId：同样用名字表循环填（下标 == 枚举值）。
    // 这几张表也就是快照块宽度的来源（见 `Snap` 里的 `sizeof(...)`）⇒ 名字与宽度同源。
    for (int i = 0; i < (int)(sizeof(playerStatusEnumStrings) / sizeof(playerStatusEnumStrings[0])); ++i)
    {
        playerStatusEnum.value(playerStatusEnumStrings[i], static_cast<PlayerStatus>(i));
    }
    for (int i = 0; i < (int)(sizeof(monsterIdStrings) / sizeof(monsterIdStrings[0])); ++i)
    {
        monsterIdEnum.value(monsterIdStrings[i], static_cast<MonsterId>(i));
    }
    for (int i = 0; i < (int)(sizeof(monsterMoveStrings) / sizeof(monsterMoveStrings[0])); ++i)
    {
        monsterMoveEnum.value(monsterMoveStrings[i], static_cast<MonsterMoveId>(i));
    }

    relicTierEnum.value("COMMON", RelicTier::COMMON)
        .value("UNCOMMON", RelicTier::UNCOMMON)
        .value("RARE", RelicTier::RARE)
        .value("BOSS", RelicTier::BOSS)
        .value("SHOP", RelicTier::SHOP)
        .value("STARTER", RelicTier::STARTER)
        .value("SPECIAL", RelicTier::SPECIAL)
        .value("INVALID", RelicTier::INVALID);

#ifdef VERSION_INFO
    m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
    m.attr("__version__") = "dev";
#endif
}
