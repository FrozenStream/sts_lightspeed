//
// 引擎侧（与 Python 训练侧**无关**）的 C++ 测试 —— 动作空间 / 快照资源块 / 生死与复活。
//
// 形状与 `apps/scene-test.cpp` 一样：`apps/test_util.h` 的断言 + 纯文本 golden。
// 分家的准绳是**参照物在哪**：这里收的全是"引擎自己就能判对错"的东西 ——
//   · `actions`：`bindings/bindings-util.cpp` 的编码器 / 枚举 / `expandChildren`；
//   · `snapshot`：快照的列**真的写着**引擎的那个值（布局本身不做断言，见下）；
//   · `revive`：`targetable` 与**意图**把「永久尸体」和「待复活」分开、两条判胜边界。
//   · `search`：**net-free UCT**（教师用的那个）在固定局面上的读数 —— 冻 golden（见本节注释）。
// 参照物在 Python 的那些（对 `azaction.teacher_action`、`zspec`、网络、npz 缓存）**留在 Python** ——
// 搬进来只会退化成"自己跟自己比"。
// ⚠️ 搜索是**例外**：Python 侧那份实现（`azsearch.py`）2026-10-11 已删，这里改成 KataGo 式的
// **冻 golden**（`engine-test search` 与 `_chk_selfplay --golden`），代价是改搜索要显式重基线。
//
// 为什么这几件事必须测（都是踩过的坑）：
//   · 2026-09-13：目标字段只有 5 个取值（0..4）⇒ **第 5 只怪编不出来**，`action_to_idx` 返回
//     -1 被静默丢弃；`LOTS_OF_SLIMES`（5 只史莱姆）上教师 500sims 只有 24.0%，随机却有 83.0%
//     —— 一度被读成"这场景比 Boss 还难"。教训：**「教师弱」不等于「场景难」**。
//   · 药水打第 5 只怪会编成 `pt=5`，而旧编码里 5 就是 `discard` ⇒ 它**不报错**，只是把
//     "用药水打它"静默记成"丢弃这瓶药水"。所以查的是**单射性**，只查范围挡不住。
//   · 生死/去留只有 `targetable` 一列，而 `monsterCount` 只增不减 ⇒ 光看它分不出「永久尸体」
//     和「下回合满血站起来」；**为什么不能打写在意图里**。判胜又看 `areMonstersBasicallyDead()`
//     （把 halfDead 算活着）⇒ 与计数器会差 1，两条边界都得钉。
//
// ⚠️ 布局（块偏移 / 宽度）**不写断言**：`azlayout.LAYOUT` 也只是 `sts.snapshot_layout()`，
//    在 Python 里"验"它等于把引擎的常量链读回来再比一遍（恒真）。回归改由 golden 里的
//    `layout` 行拦 —— 偏移被冻住，谁动了就得显式重基线，同时逼出一次 code review。
//
// 用法：
//   engine-test                                              # 全部小节（诊断走 stderr）
//   engine-test actions|snapshot|revive|search               # 只跑某一节
//   engine-test --golden collect/tests/golden_engine.txt     # 再与 golden 逐行对账
//   engine-test --dump > collect/tests/golden_engine.txt     # 刷新 golden（有意改口径时）
//
// ⚠️ 人看的输出一律走 **stderr**，stdout 只留给 `--dump` 的 golden。
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "constants/MonsterEncounters.h"
#include "constants/MonsterIds.h"    // MonsterId::AWAKENED_ONE / DARKLING
#include "constants/MonsterMoves.h"  // MMID（意图招式 id）与名字表
#include "constants/Potions.h"
#include "encs.h"          // 任务表（`allEnemies` + `representativeFloor`）
#include "action.h"        // 动作层：枚举 / 编码 / 反解 / 展开（`sts::collect`）
#include "scenebuild.h"    // buildBattle / buildStaged / ascensionOf / actOf
#include "slaythespire.h"  // sts::py：段宽常量（`handStride` / `endTurnIdx` …）+ endTurn
#include "snapshot.h"      // snapshotOf + `Snap` 的布局常量（唯一一份）

#include "test_util.h"

// 严格只依赖能编进 C++ 测试的那几个源（`bindings-util.cpp`）；不拉 pybind / libtorch。
//
// ⚠️ 容量与动作常量住在 `sts::collect`（`collect/snapshot.h` / `bindings/slaythespire.h` 的
//    `sts::py`）；动作层（`enumerateActions` / `actionToIdx` / `idxToAction` / `expandChildren`）
//    2026-10-12 起也在 `sts::collect`（`collect/action.h`）。
//    两边都 `using namespace` 进来，名字不重名（重名会当场二义，正是我们要的）。
using namespace sts;
using namespace sts::py;
using namespace sts::collect;

namespace
{

// 一个扫描场景 = `(seed, 层)`。两个都扫：
//   · floor 4 —— 会发药水，药水动作因此也进了扫描范围；
//   · floor 12 + seed 1000 —— 老守卫 `_chk_actions.py` 用的那一个，读数可以直接对账。
struct Scene
{
    std::uint64_t seed;
    int floor;
};

const Scene SCENES[2] = {{0, 4}, {1000, 12}};

[[noreturn]] void fail(const std::string &where, const std::string &what)
{
    testutil::fatal(where + "：" + what);
}

std::string encName(MonsterEncounter e)
{
    return monsterEncounterEnumNames[static_cast<int>(e)];
}

std::string whereOf(const Scene &sc, MonsterEncounter enc)
{
    return encName(enc) + "@floor" + std::to_string(sc.floor) + " seed=" + std::to_string(sc.seed);
}

// 一个 `(遭遇, 场景)` 的动作空间形状 —— 这行就是 golden 的内容：**形状变了就该有人看出来**。
struct Row
{
    const Scene *sc = nullptr;
    MonsterEncounter enc = MonsterEncounter::INVALID;
    int monsters = 0;
    int actions = 0;
    int distinct = 0;
    int potionActions = 0;
    int maxT = -1;   // 卡牌目标字段（0 = 不需要目标；1..maxMonsters = 第 n 只怪）
    int maxPt = -1;  // 药水目标字段（0 = 不需要目标；1..maxMonsters = 第 n 只怪；6 = 丢弃）
};

// ── 1. 段宽之间的算术关系（编译期常量；`actionToIdx` 的全部算术就靠这几条）────────────
void checkActionGrid()
{
    TEST_ASSERT(handStride == maxMonsters + 1);    // 无目标 + monster 0..4
    TEST_ASSERT(potionStride == maxMonsters + 2);  // 无目标 + monster 0..4 + 丢弃
    TEST_ASSERT(potionBase == handSlots * handStride);
    TEST_ASSERT(endTurnIdx == potionBase + potionSlots * potionStride);
    TEST_ASSERT(unifiedActionCount == endTurnIdx + 1);
    // 策略头就是按这个宽度建的（Python 侧 `aznet.POLICY_DIM`）；动它 = 换网络。
    TEST_ASSERT(unifiedActionCount == 96);
    std::fprintf(stderr, "  ✓ 网格：手牌 %d×%d + 药水 %d×%d + END_TURN ⇒ %d（END_TURN 下标 %d）\n",
                 handSlots, handStride, potionSlots, potionStride, unifiedActionCount, endTurnIdx);
}

// ── 2. 药水目标字段的三个端点必须互不相同 ────────────────────────────────────────────
// 用一颗**需要目标**的药水直接编出端点（不依赖发牌运气）：打第 1 只 = pt 1、
// 打第 5 只 = pt 5（== `maxMonsters`）、丢弃 = pt 6（== `potionStride-1`）。
// ← 历史上 5 与"丢弃"撞过，这一节就是那次的对症断言。
void checkPotionTargetField()
{
    const Scene sc{0, 4};
    BattleContext bc =
        buildBattle(sc.seed, MonsterEncounter::LOTS_OF_SLIMES, sc.floor, ascensionOf(sc.seed));
    if (bc.monsters.monsterCount < maxMonsters)
    {
        fail("checkPotionTargetField", "这个场景不足 5 只怪 ⇒ 第 5 个端点就测不到");
    }
    TEST_ASSERT(bc.potionCapacity >= 1);
    bc.potions[0] = Potion::FIRE_POTION;  // potionRequiresTarget(FIRE_POTION) == true
    bc.potionCount = std::max(bc.potionCount, 1);

    const int idx1 = actionToIdx(bc, search::Action(search::ActionType::POTION, 0, 0));
    const int idx5 = actionToIdx(bc, search::Action(search::ActionType::POTION, 0, maxMonsters - 1));
    const int idxDiscard = actionToIdx(bc, search::Action(search::ActionType::POTION, 0, 99));
    TEST_ASSERT(idx1 == potionBase + 1);
    TEST_ASSERT(idx5 == potionBase + maxMonsters);
    TEST_ASSERT(idxDiscard == potionBase + potionStride - 1);
    TEST_ASSERT(idx5 != idxDiscard);
    // 反解也要回到同一件事（`idxToAction` 是搜索与标签的另一条出口）。
    TEST_ASSERT(actionToIdx(bc, idxToAction(bc, idx5)) == idx5);
    TEST_ASSERT(actionToIdx(bc, idxToAction(bc, idxDiscard)) == idxDiscard);
    TEST_ASSERT(idxToAction(bc, idxDiscard).getActionType() == search::ActionType::POTION);
    std::fprintf(stderr, "  ✓ 药水目标字段：第 1 只=%d、第 %d 只=%d、丢弃=%d（三者互不相同）\n",
                 idx1, maxMonsters, idx5, idxDiscard);
}

// ── 3. 全遭遇扫描：每个合法动作编出**唯一且落在策略头内**的下标，下标空间能反解 ──────────
Row scanOne(const Scene &sc, MonsterEncounter enc)
{
    const std::string where = whereOf(sc, enc);
    const BattleContext bc = buildBattle(sc.seed, enc, sc.floor, ascensionOf(sc.seed));
    if (bc.inputState == InputState::CARD_SELECT)
    {
        fail(where, "开局就落在 CARD_SELECT —— 那条路是另一套下标空间，不在本测试口径内");
    }
    const std::vector<search::Action> actions = enumerateActions(bc);

    Row r;
    r.sc = &sc;
    r.enc = enc;
    r.monsters = bc.monsters.monsterCount;
    r.actions = static_cast<int>(actions.size());
    std::set<int> seen;
    for (const search::Action &a : actions)
    {
        int idx = 0;
        try
        {
            idx = actionToIdx(bc, a);
        }
        catch (const std::exception &e)
        {
            fail(where, std::string("合法动作编不出下标：") + e.what());
        }
        if (idx < 0 || idx >= unifiedActionCount)
        {
            fail(where, "下标越界：" + std::to_string(idx));
        }
        if (!seen.insert(idx).second)
        {
            fail(where, "两个合法动作撞到同一个下标：" + std::to_string(idx));
        }
        // 反解回**同一个下标**（比"反解回同一个 Action 对象"更贴口径：标签与 π 用的是下标；
        // 非目标动作的 `targetIdx` 默认值在不同构造路径上并不保证逐位相同）。
        if (actionToIdx(bc, idxToAction(bc, idx)) != idx)
        {
            fail(where, "下标反解不回自己：" + std::to_string(idx));
        }
        switch (a.getActionType())
        {
        case search::ActionType::CARD:
            if (a.getTargetIdx() >= 0)
            {
                r.maxT = std::max(r.maxT, idx % handStride);
            }
            break;
        case search::ActionType::POTION:
            ++r.potionActions;
            r.maxPt = std::max(r.maxPt, (idx - potionBase) % potionStride);
            break;
        default:
            break;
        }
    }
    r.distinct = static_cast<int>(seen.size());
    return r;
}

std::vector<Row> checkEncoding()
{
    std::vector<Row> rows;
    for (const Scene &sc : SCENES)
    {
        for (MonsterEncounter enc : MonsterEncounterPool::allEnemies)
        {
            const Row r = scanOne(sc, enc);
            if (r.actions == 0)
            {
                fail(whereOf(sc, enc), "一个合法动作都没有 ⇒ 这局根本走不下去");
            }
            if (r.distinct != r.actions)
            {
                fail(whereOf(sc, enc), "可编码的动作数 != 合法动作数（有动作被静默丢了）");
            }
            rows.push_back(r);
        }
    }
    std::fprintf(stderr,
                 "  ✓ 动作编码：%zu 项（%zu 遭遇 × %zu 场景）全部单射、落在 [0, %d)，且下标能反解\n",
                 rows.size(), std::size(MonsterEncounterPool::allEnemies), std::size(SCENES),
                 unifiedActionCount);
    return rows;
}

// ── 4. 5 怪场景：卡牌必须能指定第 5 只（历史 bug 的正脸）─────────────────────────────
void checkFifthMonster()
{
    const Scene sc{1000, 12};
    const BattleContext bc =
        buildBattle(sc.seed, MonsterEncounter::LOTS_OF_SLIMES, sc.floor, ascensionOf(sc.seed));
    TEST_ASSERT(bc.monsters.monsterCount >= maxMonsters);
    bool saw5 = false;
    for (const search::Action &a : enumerateActions(bc))
    {
        if (a.getActionType() == search::ActionType::CARD && a.getTargetIdx() == maxMonsters - 1)
        {
            saw5 = true;
        }
    }
    if (!saw5)
    {
        fail(whereOf(sc, MonsterEncounter::LOTS_OF_SLIMES),
             "没有任何动作能指定第 5 只怪（目标字段又窄回去了？）");
    }
    std::fprintf(stderr, "  ✓ 5 只史莱姆：第 5 只怪可被指定（目标字段宽 %d）\n", handStride);
}

// ── 5. `expandChildren` == 「枚举 + 逐个 clone/execute/编码」（MCTS 每步展开都走它）────
// 它若与手工路径有差，搜索会在**没有任何报错**的情况下走到别的分支 ⇒ 三项逐位对上：
// 下标序列、孩子快照、终局标记。
void checkExpandChildren(const std::vector<Row> &rows)
{
    int n = 0;
    for (const Row &r : rows)
    {
        const Scene &sc = *r.sc;
        const std::string where = whereOf(sc, r.enc);
        const BattleContext bc = buildBattle(sc.seed, r.enc, sc.floor, ascensionOf(sc.seed));

        const auto expanded = expandChildren(bc);
        const std::vector<search::Action> actions = enumerateActions(bc);
        if (expanded.first.size() != actions.size() || expanded.second.size() != actions.size())
        {
            fail(where, "expandChildren 的孩子数与 enumerateActions 不同");
        }
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            if (expanded.first[i].first != actionToIdx(bc, actions[i]) ||
                !(expanded.first[i].second == actions[i]))
            {
                fail(where, "第 " + std::to_string(i) + " 个孩子的下标/动作与手工路径不同");
            }
            BattleContext manual = bc;
            actions[i].execute(manual);
            if (snapshotOf(manual) != snapshotOf(*expanded.second[i]))
            {
                fail(where, "第 " + std::to_string(i) + " 个孩子的快照与手工路径不同");
            }
            if ((manual.outcome != Outcome::UNDECIDED) !=
                (expanded.second[i]->outcome != Outcome::UNDECIDED))
            {
                fail(where, "第 " + std::to_string(i) + " 个孩子的终局标记与手工路径不同");
            }
        }
        n += static_cast<int>(actions.size());
    }
    std::fprintf(stderr,
                 "  ✓ expandChildren == 枚举 + 逐个 clone/execute/编码（%d 个孩子逐位相同）\n", n);
}

// ── 6. 快照的资源块：列 == **引擎真值**（布局本身不写断言 —— 它是 `Snap` 的常量链）────────
// 这一节从 `_chk_res.py` 搬来：布局 / 资源块与训练过程**没有关系**（`azlayout` 自己也只是
// `LAYOUT = sts.snapshot_layout()`，全派生自引擎）⇒ 以前那几条"布局断言"是把引擎的常量链
// 读回来再比一遍，恒真。真正有内容的是**语义**：快照的列真的写着引擎的那个值吗。
// 布局本身的回归由 golden 里的 `layout` 行拦（所有块偏移被冻住，动了就得显式重基线）。
struct PotionFixture
{
    StagedBattle st;
    int seed = 0;
};

PotionFixture findStagedWithPotion()
{
    // 逐槽块的取样必须有药水（否则全是 0，等于没测）—— 场景按 seed 发资源，扫出第一个。
    for (int seed = 0; seed < 200; ++seed)
    {
        StagedBattle st = buildStaged(seed, MonsterEncounter::THREE_SENTRIES, 12, ascensionOf(seed));
        for (int p : st.potions)
        {
            if (p != static_cast<int>(Potion::INVALID) &&
                p != static_cast<int>(Potion::EMPTY_POTION_SLOT))
            {
                return {std::move(st), seed};
            }
        }
    }
    fail("findStagedWithPotion", "前 200 个种子里没有一个带药水 ⇒ 场景是不是变了？");
}

std::vector<std::string> checkSnapshot()
{
    const PotionFixture fx = findStagedWithPotion();
    BattleContext bc = fx.st.b;
    const auto snap0 = snapshotOf(bc);
    const std::string where = "snapshot seed=" + std::to_string(fx.seed);

    TEST_ASSERT(snap0.size() == static_cast<std::size_t>(Snap::snapshotLen));
    TEST_ASSERT(bc.floorNum == 12);

    // ⭐ 总牌组块 == 造局记下的牌组（**集合口径**：两个方向都按排序后的非零槽比）。
    //    它与三个牌堆的区别正在这里：排除战斗造出来的临时/状态牌、且不随战斗漂移。
    std::vector<int> want;
    want.reserve(fx.st.deck.size());
    for (const std::pair<int, int> &d : fx.st.deck)
    {
        want.push_back(d.first * 2 + d.second);
    }
    std::sort(want.begin(), want.end());
    std::vector<int> got;
    for (int j = 0; j < Snap::deckSlots; ++j)
    {
        const int v = static_cast<int>(snap0[Snap::deckOff + j]);
        TEST_ASSERT(v == (j < bc.masterDeckSize ? static_cast<int>(bc.masterDeck[j]) : 0));
        if (v > 0)
        {
            got.push_back(v);
        }
    }
    std::sort(got.begin(), got.end());
    if (got != want)
    {
        fail(where, "总牌组块 != 造局时的牌组（" + std::to_string(got.size()) + " vs " +
                        std::to_string(want.size()) + "）");
    }
    TEST_ASSERT(static_cast<int>(got.size()) == bc.masterDeckSize);

    // ⭐ run 上下文：列乘回除数（`s.push<50>(bc.floorNum)` / `s.push<999>(bc.player.gold)`）
    //    必须等于引擎那两个字段 —— act / isElite / isBoss **不进**快照。
    TEST_ASSERT(std::lround(snap0[Snap::runCtxOff] * 50) == bc.floorNum);
    TEST_ASSERT(std::lround(snap0[Snap::runCtxOff + 1] * 999) == bc.player.gold);

    // ⭐ 逐槽块 == `bc.potions`（逐槽，空槽映 0）；非零槽数 == potionCount；
    //    取值都在 `[0, Potion::INVALID)` 里。哨兵 INVALID / EMPTY_POTION_SLOT 都映成 0。
    const int empty0 = static_cast<int>(Potion::INVALID);
    const int empty1 = static_cast<int>(Potion::EMPTY_POTION_SLOT);
    const auto emptyAt = [&](int k) {
        return k >= bc.potionCapacity || bc.potions[k] == Potion::INVALID ||
               bc.potions[k] == Potion::EMPTY_POTION_SLOT;
    };
    int nonzero = 0;
    for (int k = 0; k < Snap::potionSlotLen; ++k)
    {
        const int v = static_cast<int>(snap0[Snap::potionSlotOff + k]);
        const int wantSlot = k < Snap::potionSlots && !emptyAt(k)
                                 ? static_cast<int>(bc.potions[k])
                                 : empty0;
        TEST_ASSERT(v == wantSlot);
        // 空槽映 0；其余必须是**真药水** id（`EMPTY_POTION_SLOT=1` 这个哨兵不许出现在快照里）。
        TEST_ASSERT(v == 0 ||
                    (v > static_cast<int>(Potion::EMPTY_POTION_SLOT) && v < potionEnumCount));
        if (v != 0)
        {
            ++nonzero;
        }
    }
    TEST_ASSERT(nonzero == bc.potionCount);
    TEST_ASSERT(std::lround(snap0[Snap::potionOff] * Snap::potionSlots) == bc.potionCount);
    TEST_ASSERT(std::lround(snap0[Snap::potionOff + 1] * Snap::potionSlots) == bc.potionCapacity);
    // 跨生产者：逐槽块 == **造局时**（run 侧）发的药水。袋块删掉之后这是逐槽块唯一的独立核对。
    for (int k = 0; k < Snap::potionSlots; ++k)
    {
        const int staged = fx.st.potions[static_cast<std::size_t>(k)];
        const int wantSlot = (staged == empty0 || staged == empty1) ? empty0 : staged;
        TEST_ASSERT(static_cast<int>(snap0[Snap::potionSlotOff + k]) == wantSlot);
    }

    // ⭐ 推进若干步：牌堆会动，**总牌组块必须逐位不变**（临时/状态牌只进牌堆）。
    int steps = 0;
    for (; steps < 4 && bc.outcome == Outcome::UNDECIDED; ++steps)
    {
        const std::vector<search::Action> actions = enumerateActions(bc);
        if (actions.empty())
        {
            break;
        }
        actions.front().execute(bc);
    }
    TEST_ASSERT(steps > 0);  // 一步都走不了 ⇒ 下面的"不变"是空转
    const auto snap1 = snapshotOf(bc);
    for (int j = 0; j < Snap::deckSlots; ++j)
    {
        TEST_ASSERT(snap0[Snap::deckOff + j] == snap1[Snap::deckOff + j]);
    }
    std::fprintf(stderr,
                 "  ✓ 快照资源块：总牌组块 == 造局牌组（%d 张，推进 %d 步后逐位不变）/ "
                 "run ctx floor·gold / 逐槽块 == 造局药水（非零 %d 瓶 == potionCount）\n",
                 bc.masterDeckSize, steps, nonzero);

    std::ostringstream os;  // 两行：布局偏移（回归闸）+ 本次取样读数
    os << "layout snapshot_len=" << Snap::snapshotLen << " hand_off=" << Snap::handOff
       << " deck_off=" << Snap::deckOff << " deck_slots=" << Snap::deckSlots
       << " monster_off=" << Snap::monsterOff << " monster_fields=" << Snap::monsterFields
       << " run_ctx_off=" << Snap::runCtxOff << " potion_off=" << Snap::potionOff
       << " potion_slot_off=" << Snap::potionSlotOff << " potion_slots=" << Snap::potionSlots;
    std::ostringstream os2;
    os2 << "snapshot seed=" << fx.seed << " floor=" << bc.floorNum << " gold=" << bc.player.gold
        << " deck_n=" << bc.masterDeckSize << " potion_count=" << bc.potionCount
        << " potion_capacity=" << bc.potionCapacity;
    return {os.str(), os2.str()};
}

// ── 7. 「死了」和「要复活」必须分得开 + 判胜的边界（从 `_chk_revive.py` 搬来）────────────
// 生死/去留**只有 `targetable` 一列**；而 `monsterCount` 只增不减（死怪一直占槽）⇒ 光看
// `targetable` 分不出「永久尸体」和「下回合满血站起来」。**为什么不能打写在意图里**：
//   觉醒者一阶段倒下当帧 → `AWAKENED_ONE_REBIRTH`；暗灵倒下当帧 → `DARKLING_REGROW`
//   →（空过一回合）`DARKLING_REINCARNATE`；逃跑 → `LOOTER_ESCAPE`；永久尸体 → 最后一次滚出的招式。
// 这一节是那件事的**唯一**证据来源，另加两条判胜边界（半死不算可选中 / 判胜不看计数器）。
struct Frame
{
    std::vector<float> snap;  // 归一化快照：本节只读**名义列**（monsterId / intentMoveId / targetable）
    std::array<int, Snap::monsterSlots> curHp{};
    std::array<int, Snap::monsterSlots> maxHp{};
    std::array<int, Snap::monsterSlots> miscInfo{};
    int monsterCount = 0;
    int monstersAlive = 0;
    int deckSize = 0;
};

Frame frameOf(const BattleContext &bc)
{
    Frame f;
    const auto s = snapshotOf(bc);
    f.snap.assign(s.begin(), s.end());
    for (int k = 0; k < Snap::monsterSlots && k < bc.monsters.monsterCount; ++k)
    {
        f.curHp[k] = bc.monsters.arr[k].curHp;
        f.maxHp[k] = bc.monsters.arr[k].maxHp;
        f.miscInfo[k] = bc.monsters.arr[k].miscInfo;
    }
    f.monsterCount = bc.monsters.monsterCount;
    f.monstersAlive = bc.monsters.monstersAlive;
    f.deckSize = bc.masterDeckSize;
    return f;
}

int monIdOf(const Frame &f, int k)
{
    return static_cast<int>(f.snap[Snap::monsterOff + Snap::monsterFields * k + 0]);
}

int intentOf(const Frame &f, int k)
{
    return static_cast<int>(f.snap[Snap::monsterOff + Snap::monsterFields * k + 1]);
}

int targetableOf(const Frame &f, int k)
{
    return static_cast<int>(f.snap[Snap::monsterOff + Snap::monsterFields * k + 5]);
}

int aliveBySlots(const Frame &f)
{
    int n = 0;
    for (int k = 0; k < Snap::monsterSlots; ++k)
    {
        n += f.curHp[k] > 0 ? 1 : 0;
    }
    return n;
}

std::string moveName(int m)
{
    return m >= 0 && m < static_cast<int>(std::size(monsterMoveStrings)) ? monsterMoveStrings[m] : "?";
}

// 「待复活」的意图招式（`azlayout.REVIVE_MOVES` 的 C++ 正身）。
bool isReviveMove(int m)
{
    return m == static_cast<int>(MMID::AWAKENED_ONE_REBIRTH) ||
           m == static_cast<int>(MMID::DARKLING_REGROW) ||
           m == static_cast<int>(MMID::DARKLING_REINCARNATE);
}

// 走完选牌屏：场景会发"回合末强制选牌"的遗物（`NILRYS_CODEX` → `CardSelectTask::CODEX`）⇒
// `endTurn()` 会停在 `CARD_SELECT` 上、**怪物回合根本没跑**（本守卫曾经就这么静默空转过）。
void walkCardSelect(BattleContext &bc)
{
    for (int i = 0; i < 16 && bc.inputState == InputState::CARD_SELECT &&
                    bc.outcome == Outcome::UNDECIDED;
         ++i)
    {
        const std::vector<search::Action> acts = enumerateActions(bc);
        if (acts.empty())
        {
            break;
        }
        acts.front().execute(bc);
    }
}

// 一步"傻"策略：有牌就出第一张，没牌就结束回合。**故意不用 MCTS** —— 教师轨迹跨机器不保证
// 一致（`_chk_revive.py` 自己写着这条），而本节只关心"普通战里会不会自然出现尸体"。
bool greedyStep(BattleContext &bc)
{
    if (bc.outcome != Outcome::UNDECIDED)
    {
        return false;
    }
    const std::vector<search::Action> acts = enumerateActions(bc);
    if (acts.empty())
    {
        return false;
    }
    for (const search::Action &a : acts)
    {
        if (a.getActionType() == search::ActionType::CARD)
        {
            a.execute(bc);
            walkCardSelect(bc);
            return true;
        }
    }
    endTurn(bc);
    walkCardSelect(bc);
    return true;
}

void endTurnThrough(BattleContext &bc, std::vector<Frame> &frames)
{
    if (bc.outcome != Outcome::UNDECIDED)
    {
        return;
    }
    endTurn(bc);
    walkCardSelect(bc);
    frames.push_back(frameOf(bc));
}

int slotOfId(const BattleContext &bc, MonsterId id)
{
    for (int k = 0; k < bc.monsters.monsterCount; ++k)
    {
        if (bc.monsters.arr[k].id == id)
        {
            return k;
        }
    }
    return -1;
}

// 在**克隆**上扫一遍合法动作，找出第一个能让 `slot` 的意图变成 `move` 的那一下（返回动作下标）。
// ⚠️ 不能指望搜索去补那一刀：打死 1 血的觉醒者等于它下回合满血复活，MCTS 完全看得懂 ⇒ 它会
// **故意不碰** Boss（实测教师场场被小怪打死）。
int findKiller(const BattleContext &bc, int slot, int move)
{
    const std::vector<search::Action> acts = enumerateActions(bc);
    for (std::size_t i = 0; i < acts.size(); ++i)
    {
        BattleContext c = bc;
        acts[i].execute(c);
        if (intentOf(frameOf(c), slot) == move)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<std::string> checkRevive()
{
    std::vector<std::string> out;

    // ---- 1. counts 块：monstersAlive / deckSize 逐帧自洽 + 自然打出来的永久尸体 -------------
    BattleContext b1 = buildBattle(30001, MonsterEncounter::CULTIST, 4, ascensionOf(30001));
    std::vector<Frame> f1;
    for (int steps = 0; steps < 400 && b1.outcome == Outcome::UNDECIDED; ++steps)
    {
        f1.push_back(frameOf(b1));
        if (!greedyStep(b1))
        {
            break;
        }
    }
    f1.push_back(frameOf(b1));
    TEST_ASSERT(f1.size() >= 2);
    for (std::size_t i = 0; i < f1.size(); ++i)
    {
        if (f1[i].monstersAlive != aliveBySlots(f1[i]))
        {
            fail("普通战", "帧 " + std::to_string(i) + "：monstersAlive != 逐槽 curHp>0");
        }
        int nz = 0;
        for (int j = 0; j < Snap::deckSlots; ++j)
        {
            nz += f1[i].snap[Snap::deckOff + j] > 0 ? 1 : 0;
        }
        if (nz != f1[i].deckSize)
        {
            fail("普通战", "帧 " + std::to_string(i) + "：deckSize != 牌组块非零槽数");
        }
        if (f1[i].deckSize != f1[0].deckSize)
        {
            fail("普通战", "deckSize 在战斗内变了（牌组是 run 级不变量）");
        }
        if (i > 0 && f1[i].monsterCount < f1[i - 1].monsterCount)
        {
            fail("普通战", "monsterCount 回退了（尸体被压缩？）");
        }
    }
    int corpseFrame = -1;
    int corpseSlot = -1;
    for (std::size_t i = 0; i < f1.size() && corpseFrame < 0; ++i)
    {
        for (int k = 0; k < Snap::monsterSlots; ++k)
        {
            if (monIdOf(f1[i], k) > 0 && f1[i].curHp[k] <= 0 && targetableOf(f1[i], k) == 0 &&
                !isReviveMove(intentOf(f1[i], k)))
            {
                corpseFrame = static_cast<int>(i);
                corpseSlot = k;
                break;
            }
        }
    }
    if (corpseFrame < 0)
    {
        fail("普通战", "这一场没打出永久尸体 ⇒ 第 2 节那个对照就没了（换 seed？）");
    }
    std::fprintf(stderr,
                 "  ✓ counts：普通战 %zu 帧 monstersAlive == 逐槽 curHp>0 / monsterCount 只增不减 / "
                 "deckSize == 牌组块非零槽数且战斗内不变；自然尸体在帧 %d 槽 %d\n",
                 f1.size(), corpseFrame, corpseSlot);

    // ---- 2. 觉醒者：倒下当帧的意图 = 复活招式 ----------------------------------------------
    BattleContext bw = buildBattle(30000, MonsterEncounter::AWAKENED_ONE, 50, ascensionOf(30000));
    const int bossSlot = slotOfId(bw, MonsterId::AWAKENED_ONE);
    if (bossSlot < 0)
    {
        fail("AWAKENED_ONE", "遭遇里没有觉醒者（编成变了？）");
    }
    bw.monsters.arr[bossSlot].curHp = 1;  // 转阶段这条路是**可确定**的，不去"公平打一场"
    const std::vector<search::Action> bossActs = enumerateActions(bw);
    const int killerIdx =
        findKiller(bw, bossSlot, static_cast<int>(MMID::AWAKENED_ONE_REBIRTH));
    if (killerIdx < 0)
    {
        fail("AWAKENED_ONE", "扫完 " + std::to_string(bossActs.size()) +
                                 " 个合法动作都找不到能打掉一阶段的那一下");
    }
    bossActs[static_cast<std::size_t>(killerIdx)].execute(bw);
    std::vector<Frame> fr;
    fr.push_back(frameOf(bw));
    // ⚠️ 这里**只结束回合、不请教师**：玩家不出牌 ⇒ 不会顺手清掉小怪 ⇒ 复活路径必然走到。
    for (int round = 0; round < 6 && bw.outcome == Outcome::UNDECIDED; ++round)
    {
        endTurnThrough(bw, fr);
    }
    int downFrame = -1;
    for (std::size_t i = 0; i < fr.size() && downFrame < 0; ++i)
    {
        if (intentOf(fr[i], bossSlot) == static_cast<int>(MMID::AWAKENED_ONE_REBIRTH))
        {
            downFrame = static_cast<int>(i);
        }
    }
    if (downFrame < 0)
    {
        fail("AWAKENED_ONE", "没抓到「倒下当帧意图 = REBIRTH」（帧数 " + std::to_string(fr.size()) + "）");
    }
    const Frame down = fr[static_cast<std::size_t>(downFrame)];
    TEST_ASSERT(down.curHp[bossSlot] <= 0);              // 倒下但占着槽
    TEST_ASSERT(targetableOf(down, bossSlot) == 0);       // 生死/去留全部信息就这一位
    TEST_ASSERT(monIdOf(down, bossSlot) > 0);             // 槽位没被压缩，身份还在
    // 跨场对照：同一组 (curHp, targetable) 在**两场不同的战斗**里意思是两件事，只有意图能分开。
    const Frame corpse = f1[static_cast<std::size_t>(corpseFrame)];
    if (!(corpse.curHp[corpseSlot] == down.curHp[bossSlot] &&
          targetableOf(corpse, corpseSlot) == targetableOf(down, bossSlot)))
    {
        fail("AWAKENED_ONE", "倒下帧与永久尸体的 (curHp, targetable) 不同 ⇒ 对照失效");
    }
    TEST_ASSERT(!isReviveMove(intentOf(corpse, corpseSlot)));
    int reviveFrame = -1;
    for (std::size_t i = static_cast<std::size_t>(downFrame) + 1; i < fr.size(); ++i)
    {
        if (fr[i].curHp[bossSlot] > 0)
        {
            reviveFrame = static_cast<int>(i);
            break;
        }
    }
    if (reviveFrame < 0)
    {
        fail("AWAKENED_ONE", "同一槽后来没有复活（outcome=" + std::to_string(static_cast<int>(bw.outcome)) + "）");
    }
    const Frame back = fr[static_cast<std::size_t>(reviveFrame)];
    // 引擎里复活是满血（`MonsterSpecific.cpp` 的 `AWAKENED_ONE_REBIRTH`：`curHp = maxHp`），但观测帧
    // 往往已经跨过一次回合开始（如水银沙漏的回合开始伤害）⇒ 只断言"二阶段上限 + 接近满血"。
    const int wantPhase2Hp = ascensionOf(30000) >= 9 ? 320 : 300;
    TEST_ASSERT(back.maxHp[bossSlot] == wantPhase2Hp);
    TEST_ASSERT(back.curHp[bossSlot] >= (wantPhase2Hp * 9) / 10);
    TEST_ASSERT(intentOf(back, bossSlot) == static_cast<int>(MMID::AWAKENED_ONE_DARK_ECHO));
    std::fprintf(stderr,
                 "  ✓ 觉醒者：倒下帧 %d（curHp=0 targetable=0 id>0 intent=REBIRTH，与永久尸体逐位相同而"
                 "意图不同）⇒ 帧 %d 复活到 %d/%d intent=DARK_ECHO\n",
                 downFrame, reviveFrame, back.curHp[bossSlot], back.maxHp[bossSlot]);
    out.push_back("revive corpse=" + moveName(intentOf(corpse, corpseSlot)) + " frame=" +
                  std::to_string(corpseFrame) + " slot=" + std::to_string(corpseSlot) + " id=" +
                  std::to_string(monIdOf(corpse, corpseSlot)));
    out.push_back("revive awakened_one=" + moveName(intentOf(down, bossSlot)) + " down_frame=" +
                  std::to_string(downFrame) + " revive_frame=" + std::to_string(reviveFrame) +
                  " hp=" + std::to_string(back.curHp[bossSlot]) + "/" +
                  std::to_string(back.maxHp[bossSlot]) + " intent=" + moveName(intentOf(back, bossSlot)));

    // ---- 3. 暗灵：REGROW → REINCARNATE → 半数血复活 ---------------------------------------
    BattleContext bd = buildBattle(30000, MonsterEncounter::THREE_DARKLINGS, 34, ascensionOf(30000));
    bd.player.curHp = bd.player.maxHp = 999;  // 夹具：别让它复活前就被打死（那会变成运气）
    const Frame d0 = frameOf(bd);
    int victim = -1;
    int liveCount = 0;
    for (int k = 0; k < Snap::monsterSlots; ++k)
    {
        if (d0.curHp[k] > 0)
        {
            ++liveCount;
            if (victim < 0)
            {
                victim = k;
            }
        }
    }
    if (liveCount < 2)
    {
        fail("THREE_DARKLINGS", "活怪不足 2 只 ⇒ 倒下的那只可能正好是最后一只");
    }
    bd.monsters.arr[victim].curHp = 1;
    bd.monsters.arr[victim].block = 0;
    const std::vector<search::Action> dActs = enumerateActions(bd);
    const int killer2 = findKiller(bd, victim, static_cast<int>(MMID::DARKLING_REGROW));
    if (killer2 < 0)
    {
        fail("THREE_DARKLINGS", "找不到「打死非最后一只暗灵」的那一下（合法动作 " +
                                    std::to_string(dActs.size()) + " 个）");
    }
    dActs[static_cast<std::size_t>(killer2)].execute(bd);
    std::vector<Frame> fr2;
    fr2.push_back(frameOf(bd));
    for (int round = 0; round < 8 && bd.outcome == Outcome::UNDECIDED; ++round)
    {
        endTurnThrough(bd, fr2);
    }
    TEST_ASSERT(intentOf(fr2[0], victim) == static_cast<int>(MMID::DARKLING_REGROW));
    bool sawReincarnate = false;
    for (const Frame &f : fr2)
    {
        sawReincarnate = sawReincarnate || intentOf(f, victim) == static_cast<int>(MMID::DARKLING_REINCARNATE);
    }
    TEST_ASSERT(sawReincarnate);  // REGROW 那回合空过 ⇒ rollMove 只可能滚出复活
    int backFrame = -1;
    for (std::size_t i = 0; i < fr2.size(); ++i)
    {
        if (fr2[i].curHp[victim] > 0)
        {
            backFrame = static_cast<int>(i);
            break;
        }
    }
    if (backFrame < 0)
    {
        fail("THREE_DARKLINGS", "同一槽后来没有复活（outcome=" + std::to_string(static_cast<int>(bd.outcome)) + "）");
    }
    const Frame dback = fr2[static_cast<std::size_t>(backFrame)];
    TEST_ASSERT(dback.curHp[victim] == dback.maxHp[victim] / 2);  // 半数血，与觉醒者满血复活不同
    std::fprintf(stderr,
                 "  ✓ 暗灵：REGROW →REINCARNATE ⇒ 帧 %d 复活到 %d/%d（半数血）\n", backFrame,
                 dback.curHp[victim], dback.maxHp[victim]);
    out.push_back("revive darkling regrow->reincarnate revive_frame=" + std::to_string(backFrame) +
                  " hp=" + std::to_string(dback.curHp[victim]) + "/" +
                  std::to_string(dback.maxHp[victim]));

    // ---- 4. 判胜边界①：一阶段觉醒者死 + **同回合**清小怪 ⇒ 不许判胜 -----------------------
    // 原版：一阶段觉醒者的 `die()` 在 `cannotLose` 期间是空操作（`isDying` 从不置位）⇒
    // `areMonstersBasicallyDead()` 恒 false ⇒ 打光小怪也赢不了。
    BattleContext bx = buildBattle(30000, MonsterEncounter::AWAKENED_ONE, 50, ascensionOf(30000));
    const int bossX = slotOfId(bx, MonsterId::AWAKENED_ONE);
    TEST_ASSERT(bossX >= 0);
    bx.monsters.arr[bossX].curHp = 1;
    const std::vector<search::Action> xActs = enumerateActions(bx);
    const int kxIdx = findKiller(bx, bossX, static_cast<int>(MMID::AWAKENED_ONE_REBIRTH));
    if (kxIdx < 0)
    {
        fail("判胜边界①", "找不到「打掉一阶段」的动作");
    }
    xActs[static_cast<std::size_t>(kxIdx)].execute(bx);
    // 一阶段半死 ⇒ 掉出「可选中」集合 ⇒ 计数器 -1（不是判胜）。
    TEST_ASSERT(bx.monsters.monstersAlive == bx.monsters.monsterCount - 1);
    for (int k = 0; k < bx.monsters.monsterCount; ++k)
    {
        if (bx.monsters.arr[k].curHp > 0)
        {
            bx.monsters.arr[k].curHp = 1;
            bx.monsters.arr[k].block = 0;
        }
    }
    bx.player.energy = 99;  // 夹具：保证同回合内有资源把小怪清完
    int killed = 0;
    for (int i = 0; i < 4; ++i)
    {
        int before = 0;
        for (int k = 0; k < bx.monsters.monsterCount; ++k)
        {
            before += bx.monsters.arr[k].curHp > 0 ? 1 : 0;
        }
        if (before == 0 || bx.outcome != Outcome::UNDECIDED)
        {
            break;
        }
        bool stepped = false;
        for (const search::Action &a : enumerateActions(bx))
        {
            BattleContext c = bx;
            a.execute(c);
            int after = 0;
            for (int k = 0; k < c.monsters.monsterCount; ++k)
            {
                after += c.monsters.arr[k].curHp > 0 ? 1 : 0;
            }
            if (after < before)  // 这一下真的打死了怪（AoE 可能一发多只）
            {
                bx = std::move(c);
                killed += before - after;
                stepped = true;
                break;
            }
        }
        if (!stepped)
        {
            break;
        }
    }
    TEST_ASSERT(killed == bx.monsters.monsterCount - 1);
    TEST_ASSERT(bx.monsters.monstersAlive == 0);
    TEST_ASSERT(bx.outcome != Outcome::PLAYER_VICTORY);  // 计数器归零也不许判胜
    std::fprintf(stderr,
                 "  ✓ 判胜边界①：同回合清掉 %d 只小怪后 alive=%d 仍 outcome=UNDECIDED"
                 "（半死靠状态算活，不看计数器）\n",
                 killed, bx.monsters.monstersAlive);
    out.push_back("revive boundary_no_victory killed_minions=" + std::to_string(killed) +
                  " alive=" + std::to_string(bx.monsters.monstersAlive) + " outcome=" +
                  std::to_string(static_cast<int>(bx.outcome)));

    // ---- 5. 判胜边界②：三只暗灵**同回合**清空 ⇒ 必须判胜 ---------------------------------
    // 原版 `Darkling.damage()`：同种全 halfDead 时 `cannotLose=false` 且人人 `die()` ⇒ 集体真死。
    // 这条正是防"用通配『有 halfDead 就不判胜』把它改成打不死"的闸。
    BattleContext bz = buildBattle(30000, MonsterEncounter::THREE_DARKLINGS, 34, ascensionOf(30000));
    bz.player.curHp = bz.player.maxHp = 999;
    for (int k = 0; k < bz.monsters.monsterCount; ++k)
    {
        bz.monsters.arr[k].curHp = 1;
        bz.monsters.arr[k].block = 0;
    }
    bz.player.energy = 99;  // 夹具：同回合清三只需要三下（或一发 AoE）
    int zkilled = 0;
    for (int i = 0; i < 6; ++i)
    {
        int before = 0;
        for (int k = 0; k < bz.monsters.monsterCount; ++k)
        {
            before += bz.monsters.arr[k].curHp > 0 ? 1 : 0;
        }
        if (before == 0 || bz.outcome != Outcome::UNDECIDED)
        {
            break;
        }
        bool stepped = false;
        for (const search::Action &a : enumerateActions(bz))
        {
            BattleContext c = bz;
            a.execute(c);
            int after = 0;
            for (int k = 0; k < c.monsters.monsterCount; ++k)
            {
                after += c.monsters.arr[k].curHp > 0 ? 1 : 0;
            }
            if (after < before)
            {
                bz = std::move(c);
                zkilled += before - after;
                stepped = true;
                break;
            }
        }
        if (!stepped)
        {
            break;
        }
    }
    TEST_ASSERT(zkilled == 3);
    TEST_ASSERT(bz.outcome == Outcome::PLAYER_VICTORY);
    std::fprintf(stderr, "  ✓ 判胜边界②：同回合清空 3 只暗灵 ⇒ outcome=PLAYER_VICTORY\n");
    out.push_back("revive darkling_wipe killed=3 outcome=" + std::to_string(static_cast<int>(bz.outcome)));
    return out;
}


// ── 8. C++ 搜索的**冻结输出**（net-free UCT = 教师用的那个；KataGo 的 `runsearchtests` 分法）──
// 从前这条由 `_chk_selfplay` ② 拿 Python `azsearch.SearchTree` 当**活参照**守着 —— 那是两份实现。
// KataGo 的做法是**冻 golden**：把搜索在固定局面上的输出打成纯文本，谁漂移了 diff 里就现形；
// 代价是"有意改搜索"必须显式重基线，而这正是我们要的（改搜索 = 换轨迹 = 换数据）。
// goldens 的脆弱点写在 `_chk_selfplay` ② 那边（net 相关的那半）：torch 版本一变就得重基线。
struct SearchCase
{
    const char *enc;
    int floor;
    std::uint64_t seed;
    std::int64_t sims;
};

const SearchCase SEARCH_CASES[] = {
    {"CULTIST", 4, 0, 64},          // 单怪：动作少，读数好读
    {"THREE_SENTRIES", 12, 0, 64},  // 多怪 + 多目标：策略分布更能看出漂移
    {"MAW", 45, 0, 64},
    {"AWAKENED_ONE", 50, 0, 64},    // 带半死的 Boss
};

MonsterEncounter encounterFromName(const char *name)
{
    for (std::size_t i = 0; i < std::size(monsterEncounterEnumNames); ++i)
    {
        if (std::strcmp(monsterEncounterEnumNames[i], name) == 0)
        {
            return static_cast<MonsterEncounter>(i);
        }
    }
    fail("encounterFromName", std::string("没有这个遭遇：") + name);
}

// 一行读数 = 根边访问分布 + 几个根读数。**只写访问数 > 0 的边**（0 的那些不携带信息）。
std::string searchLine(const SearchCase &c, const BattleContext &bc, const SearchStats &st)
{
    std::ostringstream os;
    os << "search enc=" << c.enc << " floor=" << c.floor << " seed=" << c.seed << " sims=" << c.sims
       << " playouts=" << st.playouts << " value=" << st.rootValue << " winrate=" << st.rootWinRate
       << " best=" << st.bestValue << " hp=" << st.bestPlayerHp << " visits=";
    bool first = true;
    for (const std::pair<search::Action, std::int64_t> &pr : st.policy)
    {
        if (pr.second == 0)
        {
            continue;
        }
        if (!first)
        {
            os << ",";
        }
        first = false;
        os << actionToIdx(bc, pr.first) << ":" << pr.second;
    }
    return os.str();
}

std::vector<std::string> checkSearch()
{
    std::vector<std::string> out;
    for (const SearchCase &c : SEARCH_CASES)
    {
        const MonsterEncounter enc = encounterFromName(c.enc);
        const std::string where = std::string(c.enc) + "@" + std::to_string(c.floor);
        BattleContext bc = buildBattle(c.seed, enc, c.floor, ascensionOf(c.seed));
        const SearchStats a = searchStats(bc, c.sims);
        const SearchStats b = searchStats(bc, c.sims);
        // ① 确定性：同参数两次调用必须逐位相同（挡未播种 RNG / 调度敏感 —— 这条是
        //    "golden 能不能当闸门"的前提）。
        TEST_ASSERT(a.playouts == b.playouts && a.rootValue == b.rootValue &&
                    a.rootWinRate == b.rootWinRate && a.bestValue == b.bestValue &&
                    a.bestPlayerHp == b.bestPlayerHp && a.policy.size() == b.policy.size());
        for (std::size_t i = 0; i < a.policy.size(); ++i)
        {
            TEST_ASSERT(a.policy[i].first == b.policy[i].first &&
                        a.policy[i].second == b.policy[i].second);
        }
        TEST_ASSERT(a.playouts == c.sims);
        out.push_back(searchLine(c, bc, a));
        std::fprintf(stderr, "  ✓ 搜索 %-16s sims=%lld playouts=%lld value=%.6f winrate=%.6f\n",
                     where.c_str(), static_cast<long long>(c.sims),
                     static_cast<long long>(a.playouts), a.rootValue, a.rootWinRate);
    }
    // ② 旋钮真接上了：`explorationParameter` 减到 0.5 必须改变读数（否则那个参数是死的，
    //    而"golden 对账"会一直绿着骗人）。
    {
        const SearchCase &c = SEARCH_CASES[1];  // THREE_SENTRIES：动作多，探索项影响最大
        BattleContext bc = buildBattle(c.seed, encounterFromName(c.enc), c.floor, ascensionOf(c.seed));
        const SearchStats def = searchStats(bc, c.sims);
        const SearchStats low = searchStats(bc, c.sims, 0.5);
        bool differ = def.policy.size() != low.policy.size() || def.rootValue != low.rootValue;
        for (std::size_t i = 0; !differ && i < std::min(def.policy.size(), low.policy.size()); ++i)
        {
            differ = def.policy[i].second != low.policy[i].second;
        }
        if (!differ)
        {
            fail("搜索旋钮", "把 explorationParameter 降到 0.5 读数完全没变 ⇒ 参数没接上");
        }
        std::fprintf(stderr, "  ✓ 探索常数真接上了（0.5 与默认 3√2 的读数不同）\n");
    }
    return out;
}


// ── dump / golden ────────────────────────────────────────────────────────────────────
std::string dumpLine(const Row &r)
{
    std::ostringstream os;
    os << "seed=" << r.sc->seed << " floor=" << r.sc->floor << " act=" << actOf(r.sc->floor)
       << " enc=" << encName(r.enc) << " monsters=" << r.monsters << " actions=" << r.actions
       << " distinct=" << r.distinct << " potion_acts=" << r.potionActions << " max_t=" << r.maxT
       << " max_pt=" << r.maxPt;
    return os.str();
}

std::string dumpAll(const std::vector<Row> &rows, const std::vector<std::string> &extra)
{
    std::string out =
        "# engine-test golden —— 刷新：engine-test --dump > collect/tests/golden_engine.txt\n";
    for (const Row &r : rows)
    {
        out += dumpLine(r);
        out += '\n';
    }
    for (const std::string &l : extra)
    {
        out += l;
        out += '\n';
    }
    return out;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        testutil::fatal("读不到 golden 文件: " + path);
    }
    std::ostringstream os;
    os << in.rdbuf();
    std::string s = os.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());  // golden 可能是 CRLF 检出
    return s;
}

// 这一行属于哪个小节？`dumpAll` 的顺序 = 小节顺序，但**单跑一个小节时按行首挑**才稳
// （否则会拿 "search 的输出" 去比 "golden 第 1 行 = 动作网格"，报出一堆看不懂的差异）。
bool lineOfSection(const std::string &line, const std::string &section)
{
    if (line.empty() || line[0] == '#') { return true; }      // 头行在所有小节都比
    if (section == "actions") { return line.rfind("seed=", 0) == 0; }
    if (section == "snapshot")
    {
        return line.rfind("layout ", 0) == 0 || line.rfind("snapshot ", 0) == 0;
    }
    if (section == "revive") { return line.rfind("revive ", 0) == 0; }
    if (section == "search") { return line.rfind("search ", 0) == 0; }
    return true;
}

std::string selectSectionLines(const std::string &golden, const std::string &section)
{
    std::string out;
    for (const std::string &l : testutil::splitLines(golden))
    {
        if (lineOfSection(l, section))
        {
            out += l;
            out += '\n';
        }
    }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    bool dumpOnly = false;
    std::string goldenPath;
    std::string section = "all";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--dump") == 0)
        {
            dumpOnly = true;
        }
        else if (std::strcmp(argv[i], "--golden") == 0 && i + 1 < argc)
        {
            goldenPath = argv[++i];
        }
        else if (argv[i][0] != '-')
        {
            section = argv[i];
        }
        else
        {
            std::fprintf(stderr,
                         "用法: engine-test [actions|snapshot|revive|search|all] [--dump] [--golden <path>]\n");
            return 2;
        }
    }
    if (section != "all" && section != "actions" && section != "snapshot" && section != "revive" && section != "search")
    {
        std::fprintf(stderr, "没有这个小节：%s（现有：actions / snapshot / revive / search）\n", section.c_str());
        return 2;
    }

    std::fprintf(stderr, "engine-test（小节 %s）\n", section.c_str());
    std::vector<Row> rows;
    std::vector<std::string> extra;
    if (section == "all" || section == "actions")
    {
        checkActionGrid();
        checkPotionTargetField();
        rows = checkEncoding();
        checkFifthMonster();
        checkExpandChildren(rows);
    }
    if (section == "all" || section == "snapshot")
    {
        const std::vector<std::string> lines = checkSnapshot();
        extra.insert(extra.end(), lines.begin(), lines.end());
    }
    if (section == "all" || section == "revive")
    {
        const std::vector<std::string> lines = checkRevive();
        extra.insert(extra.end(), lines.begin(), lines.end());
    }
    if (section == "all" || section == "search")
    {
        const std::vector<std::string> lines = checkSearch();
        extra.insert(extra.end(), lines.begin(), lines.end());
    }
    const std::string dump = dumpAll(rows, extra);

    if (dumpOnly)
    {
        std::fwrite(dump.data(), 1, dump.size(), stdout);
        return 0;
    }
    if (!goldenPath.empty())
    {
        const std::string raw = readFile(goldenPath);
        const bool all = (section == "all");
        const std::string want = all ? raw : selectSectionLines(raw, section);
        std::string scope;
        if (!all)
        {
            scope = "（只比 " + section + " 那几行）";
        }
        testutil::expectText("engine-test golden", dump, want);
        std::fprintf(stderr, "  ✓ 与 %s 逐行相同%s\n", goldenPath.c_str(), scope.c_str());
    }
    std::fprintf(stderr, "\n✅ 全过\n");
    return 0;
}
