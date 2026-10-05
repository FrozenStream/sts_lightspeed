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

#include "constants/Rooms.h"
#include "sim/search/Action.h"
#include "sim/search/BattleScumSearcher2.h"   // defaultExplorationParameter（单一来源）

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
        inline constexpr int maxMonsters = 5;
        inline constexpr int handSlots = 10;
        inline constexpr int potionSlots = 5;
        inline constexpr int potionBase = handSlots * handStride;
        inline constexpr int endTurnIdx = potionBase + potionSlots * potionStride;
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

        // ── C++ UCT ──
        void scumSearchStep(BattleContext &bc, std::int64_t simulations);
        // 从当前状态搜 N 次，返回根孩子 [(Action, 访问次数), ...]（= 原始 N(s,a)，调用方自己
        // 归一成 π）；终局返回空。
        std::vector<std::pair<search::Action, std::int64_t>> rootPolicy(BattleContext &bc, std::int64_t simulations);
        // `explorationParameter` 透传给它自己（不给就用 3√2）；抽出来是为了能从 Python 扫参。
        SearchStats searchStats(BattleContext &bc, std::int64_t simulations,
                                double explorationParameter =
                                        search::BattleScumSearcher2::defaultExplorationParameter);

        // ── 动作 ↔ 统一下标（对齐 NN 策略输出与搜索树的 Action）──
        std::vector<search::Action> enumerateActions(const BattleContext &bc);
        int actionToIdx(const BattleContext &bc, const search::Action &action);  // 编不出来 = 抛
        // 非法索引返回默认构造的 Action（bits == 0xFFFFFFFF，即 C++ 的非法哨兵）。
        search::Action idxToAction(const BattleContext &bc, int idx);

        // MCTS 展开一个节点：枚举合法动作，并为每个动作**克隆 + 执行**出一个孩子。
        // 返回 `([(统一下标, Action), ...], [孩子 BattleContext, ...])`，两个列表同序；
        // 下标与 `actionToIdx` 同口径（编不出来就抛）。供 MCTS 用：一批孩子在**一次**
        // 跨语言调用里做完（原来是每个动作三次：clone / execute / action_to_idx）。
        std::pair<std::vector<std::pair<int, search::Action>>,
                  std::vector<std::unique_ptr<BattleContext>>>
        expandChildren(const BattleContext &bc);


} // namespace py

} // namespace sts

#endif //STS_LIGHTSPEED_SLAYTHESPIRE_H
