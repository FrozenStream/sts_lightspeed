#include "scenebuild.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
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
    int floorForStep(MonsterEncounterPool::PoolStep step) {
        using P = MonsterEncounterPool::PoolStep;
        switch (step) {
        case P::ACT1_WEAK:
            return 4;
        case P::ACT1_STRONG:
        case P::ACT1_ELITES:
            return 12;
        case P::ACT1_BOSS:
            return 16;
        case P::ACT2_WEAK:
            return 20;
        case P::ACT2_STRONG:
        case P::ACT2_ELITES:
            return 30;
        case P::ACT2_BOSS:
            return 33;
        case P::ACT3_WEAK:
            return 37;
        case P::ACT3_STRONG:
        case P::ACT3_ELITES:
            return 45;
        case P::ACT3_BOSS:
            return 50;
        case P::ACT4_ELITES:
        case P::ACT4_BOSS:
            return 53;
        default:
            return 12;
        }
    }

    bool isHardStep(MonsterEncounterPool::PoolStep step) {
        using P = MonsterEncounterPool::PoolStep;
        return step != P::INVALID && step != P::ACT1_WEAK && step != P::ACT2_WEAK && step != P::ACT3_WEAK;
    }

    namespace
    {
        constexpr int STAGE_EDGES[5] = { 9, 17, 26, 34, 41 };

        constexpr double DECK[6][6] = {
            // common uncommon rare basic special curse
            { 1.1, 1.4, 0.5, 0.0, 1.0, 0.2 },
            { 2.4, 2.9, 0.7, 0.0, 1.0, 0.3 },
            { 3.5, 4.6, 2.6, 0.0, 1.0, 0.4 },
            { 4.5, 5.9, 2.7, 0.0, 1.0, 0.4 },
            { 4.9, 8.0, 4.6, 0.0, 1.0, 0.3 },
            { 5.6, 9.7, 4.8, 0.0, 1.0, 0.2 }
        };

        constexpr double RELICS[6][7] = {
            // common uncommon rare boss shop starter event
            { 0.11, 0.08, 0.05, 0.03, 0.02, 0.0, 0.02 },
            { 1.05, 0.75, 0.46, 0.33, 0.19, 0.0, 0.16 },
            { 2.69, 1.91, 1.18, 0.94, 0.48, 0.0, 0.42 },
            { 3.89, 2.76, 1.71, 1.22, 0.69, 0.0, 0.60 },
            { 5.61, 3.98, 2.46, 1.86, 1.00, 0.0, 0.87 },
            { 7.16, 5.08, 3.14, 2.24, 1.27, 0.0, 1.11 },
        };

        constexpr double POTIONS[6][3] = {
            // common uncommon rare
            { 0.8, 0.3, 0.1 },
            { 0.8, 0.3, 0.1 },
            { 0.8, 0.3, 0.1 },
            { 0.8, 0.3, 0.1 },
            { 0.8, 0.3, 0.1 },
            { 0.8, 0.3, 0.1 },
        };

        constexpr double UPGRADES[6] = { 0, 2, 4, 6, 8, 10 };
        constexpr double REMOVALS[6] = { 0.0, 1.7, 1.7, 3.4, 3.4, 5.05 };
        constexpr double HP_FRAC_LO = 0.35, HP_FRAC_HI = 0.95;

        constexpr CardId SPECIAL_CARD_POOL[4] = { CardId::APPARITION, CardId::BITE, CardId::RITUAL_DAGGER, CardId::JAX };

        constexpr RelicId BANNED_RELICS[5] = { RelicId::GAMBLING_CHIP, RelicId::TOOLBOX, RelicId::NEOWS_LAMENT, RelicId::CIRCLET, RelicId::RED_CIRCLET };

        template <CharacterClass CC, PotionRarity R>
        constexpr auto buildPotionsTiresPool() {
            constexpr auto num = []() constexpr {
                std::vector<Potion> v;
                for (auto p : PotionPool::potionPool[static_cast<int>(CC)])
                    if (getPotionRarity(p) == R)
                        v.push_back(p);
                return v.size();
                }();

            std::array<Potion, num> arr{};
            int i = 0;
            for (auto p : PotionPool::potionPool[static_cast<int>(CC)])
                if (getPotionRarity(p) == R) {
                    arr[i] = p;
                    ++i;
                }
            return arr;
        }

        constexpr auto commonPotionPool = buildPotionsTiresPool<CharacterClass::IRONCLAD, PotionRarity::COMMON>();
        constexpr auto uncommonPotionPool = buildPotionsTiresPool<CharacterClass::IRONCLAD, PotionRarity::UNCOMMON>();
        constexpr auto rarePotionPool = buildPotionsTiresPool<CharacterClass::IRONCLAD, PotionRarity::RARE>();

        constexpr Potion BANNED_POTIONS[4] = { Potion::ELIXIR_POTION, Potion::GAMBLERS_BREW, Potion::ENTROPIC_BREW, Potion::COLORLESS_POTION };

        // 触发"没得选"的多选屏（`EXHAUST_MANY` / `GAMBLE`）的**卡**：引擎对这两类任务只枚举
        // "一张都不选"（`BattleScumSearcher2::enumerateCardSelectActions`），而统一下标空间里
        // 没有多选编码（`actionToIdx` 会抛）⇒ 防御是**别让它进牌组**。
        // ⚠️ `PURITY` 只在无色池里（`CardPools.h`），红池三档/诅咒/特殊池都够不到 ⇒ 造局面
        //    本来抽不到它；列在这里一是堵将来的池子变动，二是给 `applyRunBans`（整局追认）用。
        constexpr CardId BANNED_CARDS[1] = { CardId::PURITY };

        int calculateStage(int floor) {
            int s = 0;
            for (int e : STAGE_EDGES)
                if (floor >= e)
                    ++s;
            return s;
        }

        int drawPoisson(double mean, std::mt19937_64& rng) {
            if (mean <= 0.0)
                return 0;

            return std::poisson_distribution<int>(mean)(rng);
        }

        bool isBannedRelic(RelicId r) {
            for (RelicId b : BANNED_RELICS)
                if (b == r)
                    return true;
            return false;
        }

        bool isBannedPotion(Potion p) {
            for (Potion b : BANNED_POTIONS)
                if (b == p)
                    return true;
            return false;
        }

        bool isBannedCard(CardId c) {
            for (CardId b : BANNED_CARDS)
                if (b == c)
                    return true;
            return false;
        }

        inline void obtainTierCard(GameContext& gc, const std::span<const CardId> pool, std::size_t num, std::mt19937_64& rng) {
            const std::size_t take = std::min(num, pool.size());
            std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
            for (std::size_t i = 0; i < take; ++i)
            {
                // 抽数不变（RNG 流不动）⇒ 只是不把触发物发下去；golden 局面不受影响。
                const CardId c = pool[dist(rng)];
                if (!isBannedCard(c))
                    gc.deck.obtain(gc, c);
            }
        }

        void ironcladCardPool(GameContext& gc, CardRarity tier, std::size_t num, std::mt19937_64& rng) {
            switch (tier) {
            case CardRarity::COMMON:
            case CardRarity::UNCOMMON:
            case CardRarity::RARE:
            {
                const std::size_t off = static_cast<std::size_t>(RarityCardPool::getPoolOffset(CardColor::RED, tier));
                const std::size_t n = static_cast<std::size_t>(RarityCardPool::getPoolSize(CardColor::RED, tier));
                obtainTierCard(gc, std::span<const CardId>(RarityCardPool::cardBlob).subspan(off, n), num, rng);
                return;
            }
            case CardRarity::CURSE:
                obtainTierCard(gc, curseCardPool, num, rng);
                return;
            case CardRarity::SPECIAL:
                obtainTierCard(gc, SPECIAL_CARD_POOL, num, rng);
                return;
            default:
                return;
            }
        }

        inline void obtainTierRelic(GameContext& gc, const std::span<const sts::RelicId> pool, std::size_t num, std::mt19937_64& rng) {
            const std::size_t take = std::min(num, pool.size());
            std::vector<RelicId> result(take);
            std::sample(pool.begin(), pool.end(), result.begin(), take, rng);
            for (RelicId r : result)
                if (!isBannedRelic(r))
                    gc.obtainRelic(r);
        }

        void ironcladRelicPool(GameContext& gc, RelicTier tier, std::size_t num, std::mt19937_64& rng) {
            switch (tier) {
            case RelicTier::COMMON:
                obtainTierRelic(gc, Ironclad::commonRelicPool, num, rng);
                return;
            case RelicTier::UNCOMMON:
                obtainTierRelic(gc, Ironclad::uncommonRelicPool, num, rng);
                return;
            case RelicTier::RARE:
                obtainTierRelic(gc, Ironclad::rareRelicPool, num, rng);
                return;
            case RelicTier::BOSS:
                obtainTierRelic(gc, Ironclad::bossRelicPool, num, rng);
                return;
            case RelicTier::SHOP:
                obtainTierRelic(gc, Ironclad::shopRelicPool, num, rng);
                return;
            case RelicTier::SPECIAL:
                obtainTierRelic(gc, eventRelicsPool, num, rng);
                return;
            default:
                return;
            }
        }

        inline void obtainTierPotion(GameContext& gc, const std::span<const Potion> pool, std::size_t num, std::mt19937_64& rng) {
            const std::size_t take = std::min(num, pool.size());
            std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
            for (std::size_t i = 0; i < take; ++i)
                if (!isBannedPotion(pool[dist(rng)]))
                    gc.obtainPotion(pool[dist(rng)]);
        }

        void ironcladPotionPool(GameContext& gc, PotionRarity tier, std::size_t num, std::mt19937_64& rng) {
            switch (tier) {
            case PotionRarity::COMMON:
                obtainTierPotion(gc, commonPotionPool, num, rng);
                return;
            case PotionRarity::UNCOMMON:
                obtainTierPotion(gc, uncommonPotionPool, num, rng);
                return;
            case PotionRarity::RARE:
                obtainTierPotion(gc, rarePotionPool, num, rng);
                return;
            default:
                return;
            }
        }

        // 按层把牌组 / 升级 / 遗物 / 药水 / hp / 金币摆成该层的期望形状
        // （在 `reset_run_resources` **之后**调用；本函数自己不做 reset）。
        void shapeFloor(GameContext& gc, std::uint64_t seed) {
            const int st = calculateStage(gc.floorNum);
            std::mt19937_64 rng = std::mt19937_64(seed * 7919ULL + 13ULL);

            // 删初始牌
            const int n_remove = std::min(gc.deck.size(), drawPoisson(REMOVALS[st], rng));
            for (int i = 0; i < n_remove; ++i) {
                gc.deck.remove(gc, std::uniform_int_distribution<std::size_t>(0, gc.deck.size() - 1)(rng));
            }

            for (int tier = 0; tier < static_cast<int>(CardRarity::INVALID); ++tier) {
                const int k = drawPoisson(DECK[st][tier], rng);
                ironcladCardPool(gc, static_cast<CardRarity>(tier), static_cast<std::size_t>(k), rng);
            }

            const int n_up = drawPoisson(UPGRADES[st], rng);
            gc.deck.upgradeRandomCards(gc.miscRng, n_up);

            for (int tier = 0; tier < static_cast<int>(RelicTier::INVALID); ++tier) {
                const int k = drawPoisson(RELICS[st][tier], rng);
                ironcladRelicPool(gc, static_cast<RelicTier>(tier), static_cast<std::size_t>(k), rng);
            }

            for (int tier = 0; tier < static_cast<int>(PotionRarity::PLACEHOLDER); ++tier) {
                const int k = drawPoisson(POTIONS[st][tier], rng);
                ironcladPotionPool(gc, static_cast<PotionRarity>(tier), static_cast<std::size_t>(k), rng);
            }

            gc.curHp = std::max(1, std::uniform_int_distribution<int>(
                                       static_cast<int>(gc.maxHp * HP_FRAC_LO),
                                       static_cast<int>(gc.maxHp * HP_FRAC_HI))(rng));
            gc.gold = std::uniform_int_distribution<int>(0, 400)(rng);
        }
    } // namespace

    int ascensionOf(std::uint64_t seed) {
        std::mt19937_64 rng = std::mt19937_64(seed * 15485863ULL + 23ULL);
        return std::uniform_int_distribution<int>(0, 20)(rng);
    }

    int actOf(int floor) {
        return 1 + (floor >= 17) + (floor >= 34) + (floor >= 51);
    }

    std::vector<int> bannedRelics() {
        std::vector<int> out;
        out.reserve(std::size(BANNED_RELICS));
        for (RelicId r : BANNED_RELICS) {
            out.push_back(static_cast<int>(r));
        }
        return out;
    }

    std::vector<int> bannedPotions() {
        std::vector<int> out;
        out.reserve(std::size(BANNED_POTIONS));
        for (Potion p : BANNED_POTIONS) {
            out.push_back(static_cast<int>(p));
        }
        return out;
    }

    std::vector<int> bannedCards() {
        std::vector<int> out;
        out.reserve(std::size(BANNED_CARDS));
        for (CardId c : BANNED_CARDS) {
            out.push_back(static_cast<int>(c));
        }
        return out;
    }

    void applyRunBans(GameContext &gc) {
        // 追认式防御：奖励 / 商店 / 事件是按引擎自己的池发的，不受 `buildBattle` 的构造期过滤
        // 保护 ⇒ 每个非战斗动作后过一道，保证**进战斗时身上没有触发物**（多选屏因此永远不会
        // 出现，网络也就永远不面对无法编码的 `MULTI_CARD_SELECT`）。不消耗 RNG、确定 ⇒ 可复现。
        for (RelicId r : BANNED_RELICS)
        {
            if (gc.relics.has(r))
            {
                gc.relics.remove(r);
            }
        }
        for (int i = 0; i < gc.potionCapacity; ++i)
        {
            const auto slot = static_cast<std::size_t>(i);
            if (isBannedPotion(gc.potions[slot]))
            {
                // 直接清槽，不走 `discardPotionAtIdx`：那个在事件屏上有 assert
                // （`GameContext.cpp` 里同款直接赋值的写法见其注释）。
                gc.potions[slot] = Potion::EMPTY_POTION_SLOT;
                --gc.potionCount;
            }
        }
        gc.deck.removeAllMatching(gc, [](const Card &c) { return isBannedCard(c.getId()); });
    }

    static GameContext shapedContext(std::uint64_t seed, int floor, int ascension) {
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
    BattleContext buildBattle(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension) {
        GameContext gc = shapedContext(seed, floor, ascension);
        BattleContext b;
        b.init(gc, enc);
        return b;
    }

    // **诊断出口**：开战前的 run 级读数（`init` 会改写 hp / 遗物位图，所以不能从 `b` 反推）。
    // 只有测试读它；生产走 `buildBattle`。
    StagedBattle buildStaged(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension) {
        GameContext gc = shapedContext(seed, floor, ascension);

        StagedBattle out;
        out.cur_hp = gc.curHp;
        out.max_hp = gc.maxHp;
        out.gold = gc.gold;
        for (int j = 0; j < gc.deck.size(); ++j) {
            out.deck.emplace_back(static_cast<int>(gc.deck.cards[j].id), gc.deck.cards[j].isUpgraded() ? 1 : 0);
        }
        for (const auto& r : gc.relics.relics) {
            out.relics.push_back(static_cast<int>(r.id));
        }
        for (const auto& p : gc.potions) {
            out.potions.push_back(static_cast<int>(p));
        }

        out.b.init(gc, enc);
        return out;
    }

} // namespace sts::collect
