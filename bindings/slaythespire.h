//
// Created by keega on 9/24/2021.
//

#ifndef STS_LIGHTSPEED_SLAYTHESPIRE_H
#define STS_LIGHTSPEED_SLAYTHESPIRE_H

#include <vector>
#include <unordered_map>
#include <array>
#include <map>
#include <memory>
#include <cstdint>
#include <utility>

#include <cmath>

#include "constants/Rooms.h"
#include "snapshot.h"    // handSlots / maxMonsters / potionSlots（容量常量的单一来源）
#include "constants/RelicPools.h"                // sts::eventRelics（事件遗物池权威表）
#include "sim/search/Action.h"
#include "sim/search/BattleScumSearcher2.h"   // UCB 常数在它自己那条表达式里（`3*sqrt(2)`）

namespace sts {

    struct NNInterface {
        static constexpr int observation_space_size = 412;
        static constexpr int playerHpMax = 200;
        static constexpr int playerGoldMax = 1800;
        static constexpr int cardCountMax = 7;

        const std::vector<int> cardEncodeMap;
        const std::unordered_map<MonsterEncounter, int> bossEncodeMap;

        static inline NNInterface *theInstance = nullptr;

        NNInterface();

        int getCardIdx(Card c) const;
        std::array<int,observation_space_size> getObservationMaximums() const;
        std::array<int,observation_space_size> getObservation(const GameContext &gc) const;


        static std::vector<int> createOneHotCardEncodingMap();
        static std::unordered_map<MonsterEncounter, int> createBossEncodingMap();
        static NNInterface* getInstance();

    };

    namespace search {
        struct ScumSearchAgent2;
    }


    struct GameContext;
    struct Map;
    struct BattleContext;
    struct Player;
    struct Monster;
    struct MonsterGroup;

    namespace py {

        void play();

        search::ScumSearchAgent2* getAgent();
        void setGc(const GameContext &gc);
        GameContext* getGc();

        void playout();
        std::vector<Card> getCardReward(GameContext &gc);
        void pickRewardCard(GameContext &gc, Card card);
        void skipRewardCards(GameContext &gc);


        std::vector<int> getNNMapRepresentation(const Map &map);
        Room getRoomType(const Map &map, int x, int y);
        bool hasEdge(const Map &map, int x, int y, int x2);

        std::vector<std::string> getNeowOptionDescriptions(const GameContext &gc);

        int getMonsterCount(const MonsterGroup &group);
        Monster& getMonster(MonsterGroup &group, int idx);

        // ── 战斗：动作 / 搜索 / 扁平读取──
        

        inline constexpr int handStride = 6;        // [no target, enemy 1, enemy 2, enemy 3, enemy 4]
        inline constexpr int potionStride = 7;      // [no target, enemy 1, enemy 2, enemy 3, enemy 4, discard]
        // 容量常量的**唯一定义**在 `collect/snapshot.h`（快照与动作空间共用，`sts::collect`）。
        // ⚠️ 这里一律写**全限定名**：`bindings-util.cpp` / `engine-test.cpp` 那些 TU 同时在
        //    `sts`、`sts::py`、`sts::collect` 三个命名空间里做非限定查找，短名会撞成二义
        //    （实测 `reference to 'maxMonsters' is ambiguous`）。
        inline constexpr int potionBase = sts::collect::handSlots * handStride;
        inline constexpr int endTurnIdx = potionBase + sts::collect::potionSlots * potionStride;
        inline constexpr int unifiedActionCount = endTurnIdx + 1;

        struct SearchStats {
            // 根孩子访问次数：[(Action, N(s,a)), ...]
            std::vector<std::pair<search::Action, std::int64_t>> policy;
            std::int64_t playouts = 0;
            double rootValue = 0.0;
            double rootWinRate = 0.0;
            double bestValue = 0.0;
            int bestPlayerHp = 0;
        };

        void selectCard(BattleContext &bc, int idx);
        void endTurn(BattleContext &bc);
        void drinkPotion(BattleContext &bc, int idx, int target = 0);
        void discardPotion(BattleContext &bc, int idx);

        std::vector<std::vector<int>> getHand(const BattleContext &bc);
        std::vector<std::vector<int>> getDrawPile(const BattleContext &bc);
        std::vector<std::vector<int>> getDiscardPile(const BattleContext &bc);

        // 重排抽牌堆（`java::Random(seed)` 的一次均匀洗牌）。只改牌序：张数不变，
        // `shuffleRng` 等游戏随机流一律不动。用途 = 把「搜索看得见抽牌顺序」这个预言机
        // 泄漏变成可对照的显式变量。
        void shuffleDrawPile(BattleContext &bc, std::uint64_t seed);

        // ── C++ UCT ──
        void scumSearchStep(BattleContext &bc, std::int64_t simulations);
        // 从当前状态搜 N 次，返回根孩子 [(Action, 访问次数), ...]（= 原始 N(s,a)，调用方自己
        // 归一成 π）；终局返回空。
        std::vector<std::pair<search::Action, std::int64_t>> rootPolicy(BattleContext &bc, std::int64_t simulations);
        // `explorationParameter` 透传给它自己（不给就用引擎默认 3√2）；抽出来是为了能从 Python 扫参。
        // ⚠️ 默认值必须**照抄引擎那条表达式** `3*sqrt(2)`：折成字面常量（差 1 ULP）就是换轨迹，
        //    见 `.agent/MIGRATION.md` 里 24.0% → 31.5% 那次。
        SearchStats searchStats(BattleContext &bc, std::int64_t simulations,
                                double explorationParameter = 3 * std::sqrt(2));

        // ── 动作 ↔ 统一下标 ──
        // 2026-10-12 起整层搬进 `sts::collect`（`collect/action.{h,cpp}`）：本目录的一切都在
        // `sts::py` 里，而采集热循环在 `sts::collect` 里 ⇒ 放在这不免要跨命名空间 `using`。
        // 段宽常量仍在这里（唯一一份，见上面的 `handStride` 那几行）。


} // namespace py

} // namespace sts

#endif //STS_LIGHTSPEED_SLAYTHESPIRE_H
