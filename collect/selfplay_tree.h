#ifndef STS_LIGHTSPEED_SELFPLAY_TREE_H
#define STS_LIGHTSPEED_SELFPLAY_TREE_H

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "selfplay.h"
#include "sim/search/Action.h"

namespace sts::collect::detail
{
    struct Edge;

    struct Node
    {
        double N = 0.0, W = 0.0;
        bool terminal = false;
        double tv = 0.0;
        std::vector<Edge> edges;
    };

    struct Edge
    {
        search::Action action;
        double prior = 0.0;
        Node node;
    };

    struct Tree
    {
        std::size_t gid = 0;
        BattleContext real;
        BattleContext scratch;
        Node root;
        Node *leaf = nullptr;
        std::vector<Node *> path;
        std::vector<search::Action> actionStack;
        int steps = 0;
        bool finished = false;
        GameOut out;
        std::mt19937_64 rng;

        /// 探索用的 RNG **就从 `b.seed` 起**：造局是纯函数（`buildBattle`），同一 seed ⇔ 同一局面
        /// ⇒ 不再单带一个"探索 seed"（2026-10-18 删 `TaskIn`，见 `selfplay.h`）。
        Tree(std::size_t g, BattleContext b);

        static double termValue(const BattleContext &ctx, const std::string &z_spec);

        void replay();
    };

    // 终局叶直接回传并返回 true；否则保存待前向的 leaf/path。
    bool descendOne(Tree &t, const SelfplayConfig &cfg);
    void expandBackup(Tree &t, const SelfplayConfig &cfg, const float *logits, double val);
    int nextSims(Tree &t, const SelfplayConfig &cfg);
    Frame recordFrame(Tree &t);
    void resetTree(Tree &t, const SelfplayConfig &cfg);
    bool stepAndAdvance(Tree &t, const SelfplayConfig &cfg, Frame &f);
} // namespace sts::collect::detail

#endif // STS_LIGHTSPEED_SELFPLAY_TREE_H
