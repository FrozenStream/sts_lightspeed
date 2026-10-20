#ifndef STS_LIGHTSPEED_SCENEBUILD_H
#define STS_LIGHTSPEED_SCENEBUILD_H

#include <utility>
#include <vector>

#include "combat/BattleContext.h"
#include "constants/MonsterEncounters.h"
#include "game/GameContext.h"

namespace sts::collect
{
    int floorForStep(MonsterEncounterPool::PoolStep step);
    bool isHardStep(MonsterEncounterPool::PoolStep step);

    BattleContext buildBattle(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension);

    // 与 `buildBattle` 同一套 rng，但把整组**开战前**的 run 级读数一起交出来（诊断 / 对拍用）。
    // ⚠️ 不能从 `BattleContext` 反推：`init` 在开战时就会改两处 —— Blood Vial 的治疗
    //    （`player.curHp` ≠ 造局 hp）、Lizard Tail 按 `data` 清位图（遗物列表里有、位图里没有）。
    //    ⇒ 这份读数是"造局产出了什么"，`b` 是"进战斗后剩什么"，两层都得能看见。
    // ⚠️ 热路径不要用 `buildStaged(...).b`：它会额外复制 deck/relics/potions/gold。
    struct StagedBattle
    {
        BattleContext b;
        int cur_hp = 0, max_hp = 0, gold = 0;
        std::vector<std::pair<int, int>> deck; // [(CardId, upgraded?)]
        std::vector<int> relics;               // RelicId（发放顺序）
        std::vector<int> potions;              // Potion
    };
    
    StagedBattle buildStaged(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension);

    int ascensionOf(std::uint64_t seed);

    int actOf(int floor);

    std::vector<int> bannedRelics();
    std::vector<int> bannedPotions();
    std::vector<int> bannedCards();

    // 把 ban 表**追认**到一个已存在的整局上（`GameContext`）：移除被禁遗物 / 药水 / 卡。
    // 奖励 / 商店 / 事件走引擎自己的池，构造期过滤保护不到 ⇒ 每个非战斗动作后过一道
    // （`fullrun-test` 用；生产采集走 `buildBattle`，构造期已过滤）。
    void applyRunBans(GameContext &gc);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_SCENEBUILD_H