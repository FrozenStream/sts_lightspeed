//
// Created by keega on 9/17/2021.
//

#ifndef STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H
#define STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H

#include "sim/search/Action.h"

#include <functional>
#include <memory>
#include <random>
#include <iostream>
#include <limits>

namespace sts::search {

    typedef std::function<double (const BattleContext&)> EvalFnc;

    // to find a solution to a battle with tree pruning
    struct BattleScumSearcher2 {
        struct Edge;
        struct Node {
            std::int64_t simulationCount = 0;
            double evaluationSum = 0;
            std::vector<Edge> edges;
        };

        struct Edge {
            Action action;
            Node node;
        };

        std::unique_ptr<const BattleContext> rootState;
        Node root;

        EvalFnc evalFnc;
        // UCB 探索常数。
        // evaluateEdge 把质量项归一到 ~0.01 量级（avg / (bestActionValue - minActionValue)，
        // 而 evaluateEndState 赢 = +3500 / 输 ≈ -5，所以 evalRange ≈ 3505），
        // 探索项 = parameter * sqrt(log(N+1)/(n+1)) 在 n=100 时还有 ~1.0。
        // 所以这个值直接决定「q 值能不能压过探索」。默认 3*sqrt(2)。
        // 根节点上 n ≈ N/|E|，所以探索项 ∝ sqrt(|E|*log N / N) 随 N **递减**：
        // 低信号状态（开局，q 全 ~0）永远均匀，高信号状态（后段）q 能压过探索。
        // ⚠️ 保持**原表达式** `3*sqrt(2)`，不要折叠成别的字面常量！
        // 实测把 `3*sqrt(2)`（= 4.242640687119286）改成 4.242640687119285（差 **1 ULP**）
        // 后重编，评估集教师胜率从 24.0% 跳到 31.5%（400 局差 30 局），
        // 而训练集（seed 0~199）只差 1/200 局。机制未查明，很可能是搜索在近似平局处的
        // `>` 比较把微小扰动混沌放大。教训：本仓库任何 C++ 浮点常量的「无害重构」
        // 都必须当成行为变更，且重编后必须重测 `教师 500sims` 哨兵行。
        // 下面这个 constexpr 只是给绑定层当默认参数用，值与 `3*sqrt(2)` 逐位相同。
        static constexpr double defaultExplorationParameter = 4.242640687119286;  // == 3*sqrt(2)
        double explorationParameter = 3*sqrt(2);

        double bestActionValue = std::numeric_limits<double>::min();
        double minActionValue = std::numeric_limits<double>::max();
        int outcomePlayerHp = 0;

        // 从 root 出发的 playout 统计（供外部读取“逐状态价值”：随机 playout 下的期望）
        std::int64_t playoutCount = 0;
        std::int64_t winCount = 0;

        std::vector<Action> bestActionSequence;
        std::default_random_engine randGen;

        std::vector<Node*> searchStack;
        std::vector<Action> actionStack;

        explicit BattleScumSearcher2(const BattleContext &bc, EvalFnc evalFnc=&evaluateEndState);

        // public methods
        void search(int64_t simulations);
        void step();

        // private helpers
        void updateFromPlayout(const std::vector<Node*> &stack, const std::vector<Action> &actionStack, const BattleContext &endState);
        [[nodiscard]] bool isTerminalState(const BattleContext &bc) const;

        double evaluateEdge(const Node &parent, int edgeIdx);
        int selectBestEdgeToSearch(const Node &cur);
        int selectFirstActionForLeafNode(const Node &leafNode);

        void playoutRandom(BattleContext &state, std::vector<Action> &actionStack);

        void enumerateActionsForNode(Node &node, const BattleContext &bc);
        void enumerateCardActions(Node &node, const BattleContext &bc);
        void enumeratePotionActions(Node &node, const BattleContext &bc);
        void enumerateCardSelectActions(Node &node, const BattleContext &bc);
        static double evaluateEndState(const BattleContext &bc);

        void printSearchTree(std::ostream &os, int levels);
        void printSearchStack(std::ostream &os, bool skipLast=false);
    };

    extern thread_local BattleScumSearcher2 *g_debug_scum_search;

}


#endif //STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H
