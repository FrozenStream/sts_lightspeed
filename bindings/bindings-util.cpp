//
// Created by keega on 9/24/2021.
//
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include "sim/ConsoleSimulator.h"
#include "sim/BattleSimulator.h"
#include "sim/search/ScumSearchAgent2.h"
#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/Action.h"
#include "sim/SimHelpers.h"
#include "sim/PrintHelpers.h"
#include "game/Game.h"
#include "game/Map.h"
#include "combat/BattleContext.h"
#include "combat/CardManager.h"
#include "constants/Potions.h"

#include "slaythespire.h"

namespace sts {

    NNInterface::NNInterface() :
            cardEncodeMap(createOneHotCardEncodingMap()),
            bossEncodeMap(createBossEncodingMap()) {}

    int NNInterface::getCardIdx(Card c) const {
        int idx = cardEncodeMap[static_cast<int>(c.id)] * 2;

        if (c.isUpgraded()) {
            idx += 1;
        }

        return idx;
    }

    std::array<int,NNInterface::observation_space_size> NNInterface::getObservation(const GameContext &gc) const {
        std::array<int,observation_space_size> ret {};

        int offset = 0;

        ret[offset++] = std::min(gc.curHp, playerHpMax);
        ret[offset++] = std::min(gc.maxHp, playerHpMax);
        ret[offset++] = std::min(gc.gold, playerGoldMax);
        ret[offset++] = gc.floorNum;

        int bossEncodeIdx = offset + bossEncodeMap.at(gc.boss);
        ret[bossEncodeIdx] = 1;
        offset += 10;

        for (auto c : gc.deck.cards) {
            int encodeIdx = offset + getCardIdx(c);
            ret[encodeIdx] = std::min(ret[encodeIdx]+1, cardCountMax);
        }
        offset += 220;

        for (auto r : gc.relics.relics) {
            int encodeIdx = offset + static_cast<int>(r.id);
            ret[encodeIdx] = 1;
        }
        offset += 178;

        return ret;
    }

    std::array<int,NNInterface::observation_space_size> NNInterface::getObservationMaximums() const {
        std::array<int,observation_space_size> ret {};
        int spaceOffset = 0;

        ret[0] = playerHpMax;
        ret[1] = playerHpMax;
        ret[2] = playerGoldMax;
        ret[3] = 60;
        spaceOffset += 3;

        std::fill(ret.begin()+spaceOffset, ret.end(), 1);
        spaceOffset += 10;

        std::fill(ret.begin()+spaceOffset, ret.end(), cardCountMax);
        spaceOffset += 220;

        std::fill(ret.begin()+spaceOffset, ret.end(), 1);
        spaceOffset += 178;

        return ret;
    }

    std::vector<int> NNInterface::createOneHotCardEncodingMap() {
        std::vector<CardId> redCards;
        for (int i = static_cast<int>(CardId::INVALID); i <= static_cast<int>(CardId::ZAP); ++i) {
            auto cid = static_cast<CardId>(i);
            auto color = getCardColor(cid);
            if (color == CardColor::RED) {
                redCards.push_back(cid);
            }
        }

        std::vector<CardId> colorlessCards;
        for (int i = 0; i < srcColorlessCardPoolSize; ++i) {
            colorlessCards.push_back(srcColorlessCardPool[i]);
        }
        std::sort(colorlessCards.begin(), colorlessCards.end(), [](auto a, auto b) {
            return std::string(getCardEnumName(a)) < std::string(getCardEnumName(b));
        });

        std::vector<int> encodingMap(372);
        std::fill(encodingMap.begin(), encodingMap.end(), 0);

        int hotEncodingIdx = 0;
        for (auto x : redCards) {
            encodingMap[static_cast<int>(x)] = hotEncodingIdx++;
        }
        for (auto x : colorlessCards) {
            encodingMap[static_cast<int>(x)] = hotEncodingIdx++;
        }

        return encodingMap;
    }

    std::unordered_map<MonsterEncounter, int> NNInterface::createBossEncodingMap() {
        std::unordered_map<MonsterEncounter, int> bossMap;
        bossMap[ME::SLIME_BOSS] = 0;
        bossMap[ME::HEXAGHOST] = 1;
        bossMap[ME::THE_GUARDIAN] = 2;
        bossMap[ME::CHAMP] = 3;
        bossMap[ME::AUTOMATON] = 4;
        bossMap[ME::COLLECTOR] = 5;
        bossMap[ME::TIME_EATER] = 6;
        bossMap[ME::DONU_AND_DECA] = 7;
        bossMap[ME::AWAKENED_ONE] = 8;
        bossMap[ME::THE_HEART] = 9;
        return bossMap;
    }

    NNInterface* NNInterface::getInstance() {
        if (theInstance == nullptr) {
            theInstance = new NNInterface;
        }
        return theInstance;
    }

}

namespace sts::py {

    void play() {
        sts::SimulatorContext ctx;
        sts::ConsoleSimulator sim;
        sim.play(std::cin, std::cout, ctx);
    }

    search::ScumSearchAgent2* getAgent() {
        static search::ScumSearchAgent2 *agent = nullptr;
        if (agent == nullptr) {
            agent = new search::ScumSearchAgent2();
            agent->pauseOnCardReward = true;
        }
        return agent;
    }

    void playout(GameContext &gc) {
        auto agent = getAgent();
        agent->playout(gc);
    }

    std::vector<Card> getCardReward(GameContext &gc) {
        const bool inValidState = gc.outcome == GameOutcome::UNDECIDED &&
                                  gc.screenState == ScreenState::REWARDS &&
                                  gc.info.rewardsContainer.cardRewardCount > 0;

        if (!inValidState) {
            std::cerr << "GameContext was not in a state with card rewards, check that the game has not completed first." << std::endl;
            return {};
        }

        const auto &r = gc.info.rewardsContainer;
        const auto &cardList = r.cardRewards[r.cardRewardCount-1];
        return std::vector<Card>(cardList.begin(), cardList.end());
    }

    void pickRewardCard(GameContext &gc, Card card) {
        const bool inValidState = gc.outcome == GameOutcome::UNDECIDED &&
                                  gc.screenState == ScreenState::REWARDS &&
                                  gc.info.rewardsContainer.cardRewardCount > 0;
        if (!inValidState) {
            std::cerr << "GameContext was not in a state with card rewards, check that the game has not completed first." << std::endl;
            return;
        }
        auto &r = gc.info.rewardsContainer;
        gc.deck.obtain(gc, card);
        r.removeCardReward(r.cardRewardCount-1);
    }

    void skipRewardCards(GameContext &gc) {
        const bool inValidState = gc.outcome == GameOutcome::UNDECIDED &&
                                  gc.screenState == ScreenState::REWARDS &&
                                  gc.info.rewardsContainer.cardRewardCount > 0;
        if (!inValidState) {
            std::cerr << "GameContext was not in a state with card rewards, check that the game has not completed first." << std::endl;
            return;
        }

        if (gc.hasRelic(RelicId::SINGING_BOWL)) {
            gc.playerIncreaseMaxHp(2);
        }

        auto &r = gc.info.rewardsContainer;
        r.removeCardReward(r.cardRewardCount-1);
    }



    // BEGIN MAP THINGS ****************************

    std::vector<int> getNNMapRepresentation(const Map &map) {
        std::vector<int> ret;

        // 7 bits
        // push edges to first row
        for (int x = 0; x < 7; ++x) {
            if (map.getNode(x,0).edgeCount > 0) {
                ret.push_back(true);
            } else {
                ret.push_back(false);
            }
        }

        // for each node in a row, push valid edges to next row, 3 bits per node, 21 bits per row
        // skip 14th row because it is invariant
        // 21 * 13 == 273 bits
        for (int y = 0; y < 14; ++y) {
            for (int x = 0; x < 7; ++x) {

                bool localEdgeValues[3] {false, false, false};
                auto node = map.getNode(x,y);
                for (int i = 0; i < node.edgeCount; ++i) {
                    auto edge = node.edges[i];
                    if (edge < x) {
                        localEdgeValues[0] = true;
                    } else if (edge == x) {
                        localEdgeValues[1] = true;
                    } else {
                        localEdgeValues[2] = true;
                    }
                }
                ret.insert(ret.end(), localEdgeValues, localEdgeValues+3);
            }
        }

        // room types - for each node there are 6 possible rooms,
        // the first row is always monster, the 8th row is always treasure, 14th is always rest
        // this gives 14-3 valid rows == 11
        // 11 * 6 * 7 = 462 bits
        for (int y = 1; y < 14; ++y) {
            if (y == 8) {
                continue;
            }
            for (int x = 0; x < 7; ++x) {
                auto roomType = map.getNode(x,y).room;
                for (int i = 0; i < 6; ++i) {
                    ret.push_back(static_cast<int>(roomType) == i);
                }
            }
        }

        return ret;
    };

    Room getRoomType(const Map &map, int x, int y) {
        if (x < 0 || x > 6 || y < 0 || y > 14) {
            return Room::INVALID;
        }

        return map.getNode(x,y).room;
    }

    bool hasEdge(const Map &map, int x, int y, int x2) {
        if (x == -1) {
            return map.getNode(x2,0).edgeCount > 0;
        }

        if (x < 0 || x > 6 || y < 0 || y > 14) {
            return false;
        }


        auto node = map.getNode(x,y);
        for (int i = 0; i < node.edgeCount; ++i) {
            if (node.edges[i] == x2) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> getNeowOptionDescriptions(const GameContext &gc) {
        std::vector<std::string> descs;
        const auto &r = gc.info.neowRewards;
        for (int i = 0; i < 4; ++i) {
            std::string s = Neow::bonusStrings[static_cast<int>(r[i].r)];
            if (static_cast<int>(r[i].d) != 1) { // not NONE (1)
                s += " | " + std::string(Neow::drawbackStrings[static_cast<int>(r[i].d)]);
            }
            descs.push_back(s);
        }
        return descs;
    }

    int getMonsterCount(const MonsterGroup &group) { return group.monsterCount; }

    Monster &getMonster(MonsterGroup &group, int idx) {
        if (idx < 0 || idx >= group.monsterCount)
            throw std::out_of_range("monster idx out of range");
        return group.arr[idx];
    }

    namespace {
        void finishAction(BattleContext &bc) {
            bc.inputState = InputState::EXECUTING_ACTIONS;
            bc.executeActions();
        }

        std::vector<int> cardFullView(const CardInstance &c) {
            return {static_cast<int>(c.id), c.upgraded ? 1 : 0, c.cost, c.costForTurn};
        }
    }

    void scumSearchStep(BattleContext &bc, std::int64_t simulations) {
        search::BattleScumSearcher2 searcher(bc);
        searcher.search(simulations);
        if (!searcher.bestActionSequence.empty()) {
            searcher.bestActionSequence.front().execute(bc);
        }
    }

    std::vector<std::pair<search::Action, std::int64_t>>
    rootPolicy(BattleContext &bc, std::int64_t simulations) {
        search::BattleScumSearcher2 searcher(bc);
        searcher.search(simulations);

        std::vector<std::pair<search::Action, std::int64_t>> out;
        out.reserve(searcher.root.edges.size());
        for (const auto &e : searcher.root.edges) {
            out.emplace_back(e.action, e.node.simulationCount);
        }
        return out;
    }

    SearchStats searchStats(BattleContext &bc, std::int64_t simulations, double explorationParameter) {
        search::BattleScumSearcher2 searcher(bc);
        searcher.explorationParameter = explorationParameter;
        searcher.search(simulations);

        SearchStats out;
        out.playouts = searcher.playoutCount;
        out.bestValue = searcher.bestActionValue;
        out.bestPlayerHp = searcher.outcomePlayerHp;
        if (searcher.playoutCount > 0) {
            out.rootWinRate = static_cast<double>(searcher.winCount)
                            / static_cast<double>(searcher.playoutCount);
        }
        if (searcher.root.simulationCount > 0) {
            out.rootValue = searcher.root.evaluationSum
                          / static_cast<double>(searcher.root.simulationCount);
        }
        out.policy.reserve(searcher.root.edges.size());
        for (const auto &e : searcher.root.edges) {
            out.policy.emplace_back(e.action, e.node.simulationCount);
        }
        return out;
    }

    std::vector<search::Action> enumerateActions(const BattleContext &bc) {
        search::BattleScumSearcher2 searcher(bc);
        searcher.enumerateActionsForNode(searcher.root, bc);
        std::vector<search::Action> out;
        out.reserve(searcher.root.edges.size());
        for (const auto &e : searcher.root.edges) {
            out.push_back(e.action);
        }
        return out;
    }

    // 编不出来就是编不出来 —— **不返回 -1**：Python 侧已经没有降级/丢弃路径，
    // 而 -1 在 numpy 里是合法的末位下标（会静默污染标签）。
    [[noreturn]] void unencodable(const char *why) {
        throw std::runtime_error(std::string("动作无法编码进统一下标：") + why);
    }

    int actionToIdx(const BattleContext &bc, const search::Action &a) {
        switch (a.getActionType()) {
            case search::ActionType::CARD: {
                const int hand = a.getSourceIdx();
                if (hand < 0 || hand >= bc.cards.cardsInHand) { throw std::runtime_error("hand ID out of range"); }
                const auto &c = bc.cards.hand[hand];
                const int target = a.getTargetIdx();
                const int t = c.requiresTarget() ? target + 1 : 0;   // monster 0..4 -> 1..5
                if (t < 0 || t > maxMonsters) { unencodable("目标超出 5 只怪（目标字段只有 0..5）"); }
                return hand * handStride + t;
            }
            case search::ActionType::POTION: {
                const int slot = a.getSourceIdx();
                if (slot < 0 || slot >= bc.potionCapacity) { unencodable("药水槽越界"); }
                const auto p = bc.potions[slot];
                if (p == Potion::INVALID || p == Potion::EMPTY_POTION_SLOT) { unencodable("药水槽是空的"); }
                const int target = a.getTargetIdx();
                int pt;
                if (target > maxMonsters) {
                    pt = potionStride - 1;                    // 丢弃（target > 5 时 targets[5] 是「用在自己身上」的槽）
                } else if (potionRequiresTarget(p)) {
                    pt = target + 1;                          // monster 0..4 -> 1..5
                } else {
                    pt = 0;                                   // no target needed
                }
                if (pt < 0 || pt > potionStride - 1) { unencodable("药水目标字段越界"); }
                return potionBase + slot * potionStride + pt;
            }
            case search::ActionType::SINGLE_CARD_SELECT:
                // CARD_SELECT 语境专用；下标 = 候选块第 k 槽（越界由调用方当场崩）。
                return a.getSelectIdx();
            case search::ActionType::END_TURN:
                return endTurnIdx;
            default:
                unencodable("不认识的动作类型");
        }
    }

    std::pair<std::vector<std::pair<int, search::Action>>,
              std::vector<std::unique_ptr<BattleContext>>>
    expandChildren(const BattleContext &bc) {
        const auto actions = enumerateActions(bc);
        std::pair<std::vector<std::pair<int, search::Action>>,
                  std::vector<std::unique_ptr<BattleContext>>> out;
        out.first.reserve(actions.size());
        out.second.reserve(actions.size());
        for (const auto &a : actions) {
            // 克隆 + 执行，与 Python 侧原来的 `child = node.b.clone(); a.execute(child)` 同序
            auto child = std::make_unique<BattleContext>(bc);
            a.execute(*child);
            out.first.emplace_back(actionToIdx(bc, a), a);
            out.second.push_back(std::move(child));
        }
        return out;
    }

    search::Action idxToAction(const BattleContext &bc, int idx) {
        if (bc.inputState == InputState::CARD_SELECT) {
            if (idx < 0) { return {}; }
            return search::Action(search::ActionType::SINGLE_CARD_SELECT, idx);
        }
        if (idx == endTurnIdx) { return search::Action(search::ActionType::END_TURN); }
        if (idx < 0 || idx >= unifiedActionCount) { return {}; }
        if (idx < potionBase) {
            const int hand = idx / handStride;
            const int t = idx % handStride;
            if (hand >= bc.cards.cardsInHand) { return {}; }
            const auto &c = bc.cards.hand[hand];
            if (c.requiresTarget()) {
                if (t == 0) { return {}; }                    // must target a monster
                return search::Action(search::ActionType::CARD, hand, t - 1);
            }
            return search::Action(search::ActionType::CARD, hand);
        }
        const int pidx = idx - potionBase;
        const int slot = pidx / potionStride;
        const int pt = pidx % potionStride;
        if (slot >= bc.potionCapacity) { return {}; }
        const auto p = bc.potions[slot];
        if (p == Potion::INVALID || p == Potion::EMPTY_POTION_SLOT) { return {}; }
        if (pt == potionStride - 1) { return search::Action(search::ActionType::POTION, slot, 99); } // discard (>5)
        if (potionRequiresTarget(p)) {
            if (pt == 0) { return {}; }
            return search::Action(search::ActionType::POTION, slot, pt - 1);
        }
        return search::Action(search::ActionType::POTION, slot);
    }

    void selectCard(BattleContext &bc, int idx) {
        if (bc.inputState != InputState::CARD_SELECT) {
            return;
        }

        auto &info = bc.cardSelectInfo;
        switch (info.cardSelectTask) {

            case CardSelectTask::ARMAMENTS:
                if (idx >= 0 && idx < bc.cards.cardsInHand) bc.chooseArmamentsCard(idx);
                break;

            case CardSelectTask::CODEX:
                if (idx >= 0 && idx < 4 && idx != 3) { // 3 means skip
                    bc.chooseCodexCard(info.codexCards()[idx]);
                }
                break;

            case CardSelectTask::DISCOVERY:
                if (idx >= 0 && idx < 3) bc.chooseDiscoveryCard(info.discovery_Cards()[idx]);
                break;

            case CardSelectTask::DUAL_WIELD:
                if (idx >= 0 && idx < bc.cards.cardsInHand) bc.chooseDualWieldCard(idx);
                break;

            case CardSelectTask::EXHAUST_ONE:
                if (idx >= 0 && idx < bc.cards.cardsInHand) bc.chooseExhaustOneCard(idx);
                break;

            case CardSelectTask::EXHUME:
                if (idx >= 0 && idx < bc.cards.exhaustPile.size()) bc.chooseExhumeCard(idx);
                break;

            case CardSelectTask::FORETHOUGHT:
                if (idx >= 0 && idx < bc.cards.cardsInHand) bc.chooseForethoughtCard(idx);
                break;

            case CardSelectTask::HEADBUTT:
                if (idx >= 0 && idx < bc.cards.discardPile.size()) bc.chooseHeadbuttCard(idx);
                break;

            case CardSelectTask::HOLOGRAM:
                if (idx >= 0 && idx < bc.cards.discardPile.size()) bc.chooseDiscardToHandCard(idx, false);
                break;

            case CardSelectTask::LIQUID_MEMORIES_POTION:
                if (idx >= 0 && idx < bc.cards.discardPile.size()) bc.chooseDiscardToHandCard(idx, true);
                break;

            case CardSelectTask::SECRET_TECHNIQUE:
            case CardSelectTask::SECRET_WEAPON:
                if (idx >= 0 && idx < bc.cards.drawPile.size()) bc.chooseDrawToHandCards(&idx, 1);
                break;

            case CardSelectTask::WARCRY:
                if (idx >= 0 && idx < bc.cards.cardsInHand) bc.chooseWarcryCard(idx);
                break;

            case CardSelectTask::MEDITATE:
            case CardSelectTask::NIGHTMARE:
            case CardSelectTask::RECYCLE:
            case CardSelectTask::SEEK:
            case CardSelectTask::SETUP:
                // not implemented in the engine yet
                break;

            default:
                break;
        }

        finishAction(bc);
    }

    void endTurn(BattleContext &bc) {
        if (bc.inputState != InputState::PLAYER_NORMAL) {
            return;
        }
        bc.endTurn();
        finishAction(bc);
    }

    void drinkPotion(BattleContext &bc, int idx, int target) {
        if (bc.inputState != InputState::PLAYER_NORMAL) {
            return;
        }
        bc.drinkPotion(idx, target);
        finishAction(bc);
    }

    void discardPotion(BattleContext &bc, int idx) {
        bc.discardPotion(idx);
        finishAction(bc);
    }

    std::vector<std::vector<int>> getHand(const BattleContext &bc) {
        std::vector<std::vector<int>> ret;
        ret.reserve(bc.cards.cardsInHand);
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            ret.push_back(cardFullView(bc.cards.hand[i]));
        }
        return ret;
    }

    std::vector<std::vector<int>> getDrawPile(const BattleContext &bc) {
        std::vector<std::vector<int>> ret;
        ret.reserve(bc.cards.drawPile.size());
        for (const auto &c : bc.cards.drawPile) {
            ret.push_back(cardFullView(c));
        }
        return ret;
    }

    std::vector<std::vector<int>> getDiscardPile(const BattleContext &bc) {
        std::vector<std::vector<int>> ret;
        ret.reserve(bc.cards.discardPile.size());
        for (const auto &c : bc.cards.discardPile) {
            ret.push_back(cardFullView(c));
        }
        return ret;
    }

}