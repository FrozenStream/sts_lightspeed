#include "scenebuild.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

#include "constants/CardPools.h" // RarityCardPool（红池三档）/ curseCardPool
#include "constants/Cards.h"     // CardId / CardRarity / CardColor
#include "constants/Potions.h"
#include "constants/RelicPools.h" // Ironclad::*RelicPool（各档遗物的定义源）
#include "constants/Relics.h"
#include "game/Deck.h"
#include "game/Game.h"   // ascensionOf
#include "game/Random.h" // sts::Random（引擎自己的流）与 java::Random

namespace sts::collect
{

    namespace
    {

        constexpr int STAGE_EDGES[5] = {9, 17, 26, 34, 41};

        // common uncommon rare curse special
        constexpr double DECK[6][5] = {{1.1, 1.4, 0.5, 0.2, 1.0},
                                       {2.4, 2.9, 0.7, 0.3, 1.0},
                                       {3.5, 4.6, 2.6, 0.4, 1.0},
                                       {4.5, 5.9, 2.7, 0.4, 1.0},
                                       {4.9, 8.0, 4.6, 0.3, 1.0},
                                       {5.6, 9.7, 4.8, 0.2, 1.0}};

        constexpr double RELICS[6][6] = {
            // common uncommon rare boss shop special
            {0.11, 0.08, 0.05, 0.03, 0.02, 0.02},
            {1.05, 0.75, 0.46, 0.33, 0.19, 0.16},
            {2.69, 1.91, 1.18, 0.94, 0.48, 0.42},
            {3.89, 2.76, 1.71, 1.22, 0.69, 0.60},
            {5.61, 3.98, 2.46, 1.86, 1.00, 0.87},
            {7.16, 5.08, 3.14, 2.24, 1.27, 1.11},
        };

        constexpr double UPGRADES[6] = {0, 2, 4, 6, 8, 10};
        constexpr double REMOVALS[6] = {0.0, 1.7, 1.7, 3.4, 3.4, 5.05};
        constexpr double HP_FRAC_LO = 0.35, HP_FRAC_HI = 0.95;

        constexpr CardId SPECIAL_POOL[4] = {CardId::APPARITION, CardId::BITE, CardId::RITUAL_DAGGER, CardId::JAX};

        // 场景**一件都不发**的遗物 —— 一条禁令、两条互不相干的原因（合并成一张表，是因为两条发放
        // 路径（档位池 / 事件表）只认"禁不禁"，不关心理由）：
        //   · `GAMBLING_CHIP`（RARE 池）/ `TOOLBOX`（SHOP 池）：它们的**拾取效果**会让采集崩或脏 ——
        //     前者回合开始强制进**弃牌多选屏**（引擎只枚举出 `MULTI_CARD_SELECT`，统一下标里没有它，
        //     `actionToIdx` 当场抛 ⇒ 整项作废）；后者开局三选一无色牌（屏**编码得出来**，挡它只是
        //     为了坐住"开局观测必须是 `PLAYER_NORMAL`"这条口径）。
        //   · `NEOWS_LAMENT` / `CIRCLET` / `RED_CIRCLET`：**根本不是事件遗物**（前者涅奥起点专用，
        //     后两个是奖池耗尽时的填充件）⇒ 只挡**事件**那条路。
        // ⚠️ 两半在对方那条路里都点不到（前三件不在 `eventRelics` 里，后三件不在任何档位池里）
        //    ⇒ 合成一张表**不改行为**，只是把"发不发"变成一个判据。加成员时先确认这一点。
        constexpr RelicId BANNED_RELICS[5] = {RelicId::GAMBLING_CHIP, RelicId::TOOLBOX,
                                              RelicId::NEOWS_LAMENT, RelicId::CIRCLET, RelicId::RED_CIRCLET};

        // 药水里不支持的几瓶（多选动作 / 生成药水 / 无色牌）—— 4 项。
        constexpr Potion BANNED_POTIONS[4] = {Potion::ELIXIR_POTION, Potion::GAMBLERS_BREW, Potion::ENTROPIC_BREW, Potion::COLORLESS_POTION};

        int stage(int floor)
        {
            int s = 0;
            for (int e : STAGE_EDGES)
                if (floor >= e)
                    ++s;
            return s;
        }

        int drawPoisson(double mean, std::mt19937_64 &rng)
        {
            if (mean <= 0.0)
                return 0;

            return std::poisson_distribution<int>(mean)(rng);
        }

        bool isBannedRelic(RelicId r)
        {
            for (RelicId b : BANNED_RELICS)
                if (b == r)
                    return true;
            return false;
        }
        CardId obtainShapedCard(int tier, std::mt19937_64 &rng)
        {
            if (tier < 3) // common / uncommon / rare
            {
                const CardRarity rarity = static_cast<CardRarity>(tier);
                const int n = RarityCardPool::getPoolSize(CardColor::RED, rarity);
                const int idx = std::uniform_int_distribution<int>(0, n - 1)(rng);
                return RarityCardPool::getCardFromPool(CardColor::RED, rarity, idx);
            }
            const int n = (tier == 3) ? curseCardPoolSize : 4;
            const CardId *pool = (tier == 3) ? curseCardPool : SPECIAL_POOL;
            return pool[std::uniform_int_distribution<int>(0, n - 1)(rng)];
        }

        // 「一份遗物池」= 首指针 + 长度。档位那五张池是**五个不同名字、长度还各不相同的**
        // `std::array`（不是 `T[5]`），事件表又是另一个长度 ⇒ 传指针 + 长度，省得为每种长度
        // 各写一遍循环。
        struct RelicSpan
        {
            const RelicId *data = nullptr;
            std::size_t size = 0;
        };

        // 从 `pool` 里**按值**抽一件遗物发放；禁用件（`isBannedRelic`）**在抽中那一刻重掷**
        // （上限 64 次）。返回是否真的发出了（池空 / 64 次全撞禁用 ⇒ false）。
        // **档位池与事件表共用这一条** —— 两条路的差别只在"候选怎么来"（见 `obtainTierRelic`）。
        bool obtainPooledRelic(GameContext &gc, std::mt19937_64 &rng, RelicSpan pool)
        {
            for (int t = 0; t < 64 && pool.size > 0; ++t)
            {
                const RelicId r = pool.data[std::uniform_int_distribution<std::size_t>(0, pool.size - 1)(rng)];
                if (!isBannedRelic(r))
                {
                    gc.obtainRelic(r);
                    return true;
                }
            }
            return false;
        }

        // 事件（特殊）档：候选就是 `constants::eventRelics` 整表（那 18 件逐一点名，引擎里
        // 没有对应的池 —— `obtain_random_relic(SPECIAL)` 会段错误）。
        bool obtainEventRelic(GameContext &gc, std::mt19937_64 &rng)
        {
            return obtainPooledRelic(gc, rng, {eventRelics.data(), eventRelics.size()});
        }

        // 该档遗物池的**定义源**。⚠️ 场景是 IRONCLAD ⇒ 用 `Ironclad::*RelicPool` 那五张
        // **constexpr 表**（`constants/RelicPools.h`）—— 它们才是"这一档本来有哪些遗物"的权威；
        // `gc.commonRelicPool` 那几个成员是 `initRelics()` 把它们 **insert + shuffle** 之后的
        // 运行期副本（顺序被 `java::Random` 打乱，不适合当抽样基数）。
        // `CardRarity` 那套"枚举当索引"在这儿用不上：这五张是**五个不同名字、长度还各不相同**的
        // `std::array`，不是 `T[5]` ⇒ 只能点名取，各自包成一个 `RelicSpan`。
        RelicSpan ironcladRelicPool(RelicTier tier)
        {
            switch (tier)
            {
            case RelicTier::COMMON:
                return {Ironclad::commonRelicPool.data(), Ironclad::commonRelicPool.size()};
            case RelicTier::UNCOMMON:
                return {Ironclad::uncommonRelicPool.data(), Ironclad::uncommonRelicPool.size()};
            case RelicTier::RARE:
                return {Ironclad::rareRelicPool.data(), Ironclad::rareRelicPool.size()};
            case RelicTier::BOSS:
                return {Ironclad::bossRelicPool.data(), Ironclad::bossRelicPool.size()};
            case RelicTier::SHOP:
                return {Ironclad::shopRelicPool.data(), Ironclad::shopRelicPool.size()};
            default:
                return {}; // STARTER / SPECIAL / INVALID 没有池
            }
        }

        // 抽一件**该档**遗物并发放。与事件那路共用 `obtainPooledRelic`，这里只负责把候选挑出来。
        // 不走 `returnRandomRelic`：那个函数**一边抽一边动 gc**（弹池子 + 吃 RNG + 在池空时降档），
        // 而"抽到的到底是哪件"只能靠前后 diff `gc.relics` 推出来 ⇒ 换名/换档就会推错，
        // 禁用件还得再 `relics.remove()` 退回去。
        // 候选 = 池里 **还没获得**（`hasRelic`）且引擎认为 **允许出现**（`relicCanSpawn`）的 ——
        // 后一条本就是引擎自己的判据（它取池时靠"弹掉已有 / `relicCanSpawn` 不过就递归重抽"来
        // 实现），这里只是把它从隐式变显式。
        void obtainTierRelic(GameContext &gc, RelicTier tier, std::mt19937_64 &rng)
        {
            const RelicSpan pool = ironcladRelicPool(tier);
            std::vector<RelicId> cand;
            for (std::size_t i = 0; i < pool.size; ++i)
            {
                const RelicId r = pool.data[i];
                if (!gc.hasRelic(r) && gc.relicCanSpawn(r, false))
                {
                    cand.push_back(r);
                }
            }

            // 抽不出来（候选空 = 这一档能给的都拿过了 / 64 次全撞禁用）就什么都不发。忽略返回值是
            // 安全的：候选空时**一次 RNG 都不消耗**，所以调用方接着循环也等价于立刻停。
            (void)obtainPooledRelic(gc, rng, {cand.data(), cand.size()});
        }

        // 从池子的前 `n` 项里抽一瓶（禁用表**当场重掷**，同 `obtainTierRelic` 的形态，上限 64 次）。
        // 返回抽中的**下标** —— 调用方据此把它移出池子（"不放回"要移的就是**抽中的这一个**）。
        // 抽不出来返回 -1。
        int pickShapedPotion(GameContext &gc, std::mt19937_64 &rng,
                             const std::array<Potion, PotionPool::poolSize> &pool, std::size_t n)
        {
            for (int t = 0; t < 64; ++t)
            {
                const std::size_t at = std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
                const Potion p = pool[at];
                bool banned = false;
                for (Potion b : BANNED_POTIONS)
                {
                    if (b == p)
                    {
                        banned = true;
                    }
                }
                if (!banned)
                {
                    gc.obtainPotion(p);
                    return static_cast<int>(at);
                }
            }
            return -1;
        }

        // 发 0..3 瓶药水（不放回）。
        void grantShapedPotions(GameContext &gc, std::uint64_t seed)
        {
            std::mt19937_64 rng = std::mt19937_64(seed * 224737ULL + 17ULL);
            // 池子 = `potionPool[0]` 的**定长副本**（`std::array`：不进堆、不需要 `vector`），
            // `n` = 还没被抽走的前缀长度。
            std::array<Potion, PotionPool::poolSize> pool = std::to_array(PotionPool::potionPool[0]);
            std::size_t n = pool.size();
            const int k = std::min<int>(std::uniform_int_distribution<int>(0, 3)(rng), static_cast<int>(n));
            for (int i = 0; i < k; ++i)
            {
                const int at = pickShapedPotion(gc, rng, pool, n);
                if (at < 0)
                {
                    break; // 池子几乎全是禁用件（不该发生）
                }
                // 抽走的就是**这一个**：换到末尾再缩长（池子只被"均匀取下标"读，不要求保序）。
                pool[static_cast<std::size_t>(at)] = pool[--n];
            }
        }

        // 按层把牌组 / 升级 / 遗物 / 药水 / hp / 金币摆成该层的期望形状
        // （在 `reset_run_resources` **之后**调用；本函数自己不做 reset）。
        void shapeFloor(GameContext &gc, std::uint64_t seed)
        {
            const int st = stage(gc.floorNum);
            std::mt19937_64 rng_card = std::mt19937_64(seed * 7919ULL + 13ULL);

            // 删初始牌。**每轮重扫现役牌组**再抽下标 —— 不要提前把候选下表算好复用：
            // `Deck::remove` 会左移后面的元素（`fixed_list::remove` 是 memmove 语义），
            // 旧表里的下标**除了被删的那个本身**全部失效 ⇒ 复用会静默删错牌（可能把
            // `ASCENDERS_BANE` 删掉，那正是这张过滤表要挡的东西）。
            // 牌组 ≤30 张、删 0..5 次，重扫是 async 噪声级的开销。
            const int n_remove = drawPoisson(REMOVALS[st], rng_card);
            for (int i = 0; i < n_remove; ++i)
            {
                std::vector<int> cand;
                for (int j = 0; j < gc.deck.size(); ++j)
                {
                    if (static_cast<int>(gc.deck.cards[j].id) != static_cast<int>(CardId::ASCENDERS_BANE))
                    {
                        cand.push_back(j);
                    }
                }
                if (cand.empty())
                {
                    break;
                }
                gc.deck.remove(gc, cand[std::uniform_int_distribution<std::size_t>(0, cand.size() - 1)(rng_card)]);
            }

            // 追加牌：五档各自抽（顺序同 `DECK` 的列序）。张数走 `rng_card`，**发哪张**也走 `rng_card`
            // —— 见 `obtainShapedCard` 的注记（不能用 `gc.miscRng`）。
            for (int tier = 0; tier < 5; ++tier)
            {
                const int k = drawPoisson(DECK[st][tier], rng_card);
                for (int i = 0; i < k; ++i)
                {
                    gc.deck.obtain(gc, Card(obtainShapedCard(tier, rng_card)));
                }
            }

            const int n_up = drawPoisson(UPGRADES[st], rng_card);
            gc.deck.upgradeRandomCards(gc.miscRng, n_up);

            // 遗物：六个档各自抽件数；特殊（事件）档按 id 发。
            // ⚠️ `RELICS[段][档]`：**第 0..4 列与 `RelicTier` 同号** ⇒ 不用另立 `TIERS[]` 查表；
            //    第 5 列是特殊（事件）档，单独发。
            static_assert(static_cast<int>(RelicTier::COMMON) == 0 && static_cast<int>(RelicTier::UNCOMMON) == 1 &&
                              static_cast<int>(RelicTier::RARE) == 2 && static_cast<int>(RelicTier::BOSS) == 3 &&
                              static_cast<int>(RelicTier::SHOP) == 4,
                          "RELICS 的列序 = COMMON/UNCOMMON/RARE/BOSS/SHOP，RelicTier 的头五个必须同号");
            std::mt19937_64 rng_relic = std::mt19937_64(seed * 104729ULL + 7ULL);
            for (int tier = 0; tier < 5; ++tier)
            {
                const int k = drawPoisson(RELICS[st][tier], rng_relic);
                for (int i = 0; i < k; ++i)
                {
                    obtainTierRelic(gc, static_cast<RelicTier>(tier), rng_relic);
                }
            }
            const int n_special = drawPoisson(RELICS[st][5], rng_relic);
            for (int i = 0; i < n_special; ++i)
            {
                if (!obtainEventRelic(gc, rng_relic))
                {
                    break; // 池子几乎全是豁免件（不该发生）
                }
            }

            // 药水
            grantShapedPotions(gc, seed);

            // hp / 金币
            std::mt19937_64 rng_hp = std::mt19937_64(seed * 65537ULL + 29ULL);
            gc.curHp = std::max(1, static_cast<int>(std::lround(
                                       gc.maxHp * std::uniform_real_distribution<double>(HP_FRAC_LO, HP_FRAC_HI)(rng_hp))));
            gc.gold = std::uniform_int_distribution<int>(0, 400)(rng_hp);
        }

    } // namespace

    int ascensionOf(std::uint64_t seed)
    {
        // 按 seed 抽本场的进阶等级（唯一决定 A10+ 的 `ASCENDERS_BANE`）。
        std::mt19937_64 rng = std::mt19937_64(seed * 15485863ULL + 23ULL);
        return std::uniform_int_distribution<int>(0, 20)(rng);
    }

    int actOf(int floor)
    {
        // 幕分界固定装置：act1 战斗在 floor 1..16、act2 从 18 起（17 是幕间节点）⇒ 17 / 34 / 51。
        // act4（51..53：盾与矛、心）在引擎里是一等公民 —— `transitionToAct(4)` 走 `Map::act4Map()` +
        // `boss = THE_HEART` + `eliteMonsterList = SHIELD_AND_SPEAR`（那条路**不**调 `generateMonsters()`，
        // 所以碰不到 `bosses[act-1]` 那个越界下标），`returnRandomRelicTier` / `shrineList` /
        // `SimpleAgent` 也都有 `act == 4` 分支。以前这里只有两项（act4 被标成 act3），
        // 与 python 原版 `scene.ACT_EDGES = (17, 34, 51)` 不一致 —— 已改回三条。
        return 1 + (floor >= 17) + (floor >= 34) + (floor >= 51);
    }

    std::vector<int> bannedRelics()
    {
        std::vector<int> out;
        out.reserve(std::size(BANNED_RELICS));
        for (RelicId r : BANNED_RELICS)
        {
            out.push_back(static_cast<int>(r));
        }
        return out;
    }

    std::vector<int> bannedPotions()
    {
        std::vector<int> out;
        out.reserve(std::size(BANNED_POTIONS));
        for (Potion p : BANNED_POTIONS)
        {
            out.push_back(static_cast<int>(p));
        }
        return out;
    }

    static GameContext shapedContext(std::uint64_t seed, int floor, int ascension)
    {
        GameContext gc(CharacterClass::IRONCLAD, seed, ascension);
        gc.floorNum = floor;
        gc.act = actOf(floor);

        while (gc.deck.size() > 0)
            gc.deck.remove(gc, gc.deck.size() - 1);
        while (!gc.relics.relics.empty())
            gc.relics.remove(gc.relics.relics.back().id);

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
        shapeFloor(gc, seed);
        return gc;
    }

    // **生产出口**：只要有状态的战斗。不碰任何诊断用的列。
    BattleContext buildBattle(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension)
    {
        GameContext gc = shapedContext(seed, floor, ascension);
        BattleContext b;
        b.init(gc, enc);
        return b;
    }

    // **诊断出口**（只有守卫用：`_chk_shape` 整条就是冲这些数来的、`_chk_res` 拿它当快照的
    // **独立**生产者对账）。生产走 `buildBattle` ⇒ 这些字段没有生产读者，别把它接进热路径。
    StagedBattle buildStaged(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension)
    {
        GameContext gc = shapedContext(seed, floor, ascension);

        StagedBattle out;
        out.cur_hp = gc.curHp;
        out.max_hp = gc.maxHp;
        out.gold = gc.gold;
        for (int j = 0; j < gc.deck.size(); ++j)
        {
            out.deck.emplace_back(static_cast<int>(gc.deck.cards[j].id), gc.deck.cards[j].isUpgraded() ? 1 : 0);
        }
        for (const auto &r : gc.relics.relics)
        {
            out.relics.push_back(static_cast<int>(r.id));
        }
        for (const auto &p : gc.potions)
        {
            out.potions.push_back(static_cast<int>(p));
        }

        out.b.init(gc, enc);
        return out;
    }

} // namespace sts::collect
