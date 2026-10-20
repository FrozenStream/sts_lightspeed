#include "selfplay_tree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "action.h"
#include "snapshot.h"
#include "zvalue.h"

namespace sts::collect::detail
{
    namespace
    {
        constexpr int kDecayFloorBare = 32;
    }

    Tree::Tree(std::size_t g, BattleContext b)
        : gid(g), real(std::move(b)), scratch(real), rng(real.seed)
    {
    }

    double Tree::termValue(const BattleContext &ctx, const std::string &z_spec)
    {
        return ctx.outcome != Outcome::UNDECIDED ? terminalValue(ctx, z_spec) : 0.0;
    }

    void Tree::replay()
    {
        scratch = real;
        for (const auto &action : actionStack)
        {
            action.execute(scratch);
        }
    }

    // 终局叶直接回传并返回 true；否则保存待前向的 leaf/path。
    bool descendOne(Tree &t, const SelfplayConfig &cfg)
    {
        Node *node = &t.root;
        t.leaf = nullptr;
        t.path.clear();
        t.actionStack.clear();
        t.path.push_back(node);
        while (!node->terminal && !node->edges.empty())
        {
            const double sq = std::sqrt(node->N);
            Edge *best = nullptr;
            double best_u = -std::numeric_limits<double>::infinity();
            for (auto &edge : node->edges)
            {
                const Node &ch = edge.node;
                const double q = ch.N > 0.0 ? ch.W / ch.N : cfg.q_init;
                const double u = q + cfg.c_puct * edge.prior * sq / (1.0 + ch.N);
                if (u > best_u)
                {
                    best_u = u;
                    best = &edge;
                }
            }
            if (best == nullptr)
            {
                break;
            }
            t.actionStack.push_back(best->action);
            node = &best->node;
            t.path.push_back(node);
        }

        if (!node->terminal)
        {
            t.leaf = node;
            return false;
        }
        const double tv = node->tv;
        for (Node *n : t.path)
        {
            n->N += 1.0;
            n->W += tv;
        }
        return true;
    }

    namespace
    {
        // 新根展开和子树提升各处理一次；关闭噪声时不消耗随机数。
        void mixRootNoise(Tree &t, const SelfplayConfig &cfg)
        {
            auto &edges = t.root.edges;
            if (cfg.dirichlet_eps <= 0.0 || edges.size() < 2)
            {
                return;
            }
            const double n = static_cast<double>(edges.size());
            const double alpha = cfg.dirichlet_total / n;
            std::gamma_distribution<double> gamma(alpha, 1.0);
            std::vector<double> eta(edges.size());
            double ysum = 0.0;
            for (double &y : eta)
            {
                y = gamma(t.rng);
                ysum += y;
            }
            for (std::size_t i = 0; i < edges.size(); ++i)
            {
                const double ee = ysum > 0.0 ? eta[i] / ysum : 1.0 / n;
                edges[i].prior = (1.0 - cfg.dirichlet_eps) * edges[i].prior + cfg.dirichlet_eps * ee;
            }
        }
    } // namespace

    void expandBackup(Tree &t, const SelfplayConfig &cfg, const float *logits, double val)
    {
        if (!std::isfinite(val))
        {
            throw std::runtime_error("网络返回了非有限价值");
        }
        Node *node = t.leaf;
        t.replay();
        BattleContext &leaf_state = t.scratch;
        if (node->edges.empty())
        {
            auto ex = expandChildren(leaf_state);
            std::vector<float> e;
            e.reserve(ex.first.size());
            float mx = -std::numeric_limits<float>::infinity();
            for (const auto &pr : ex.first)
            {
                if (!std::isfinite(logits[pr.first]))
                {
                    throw std::runtime_error("网络返回了非有限策略 logits");
                }
                mx = std::max(mx, logits[pr.first]);
            }
            float sum = 0.0f;
            for (const auto &pr : ex.first)
            {
                const float v = std::exp(logits[pr.first] - mx);
                e.push_back(v);
                sum += v;
            }
            if (!(sum > 0.0f) || !std::isfinite(sum))
            {
                throw std::runtime_error("网络策略 logits 无法归一化");
            }
            std::vector<double> priors;
            priors.reserve(e.size());
            for (float v : e)
            {
                priors.push_back(static_cast<double>(v / sum));
            }
            node->edges.reserve(ex.first.size());
            for (std::size_t c = 0; c < ex.first.size(); ++c)
            {
                const BattleContext &cb = *ex.second[c];
                const bool term = cb.outcome != Outcome::UNDECIDED;
                node->edges.push_back(Edge{ex.first[c].second, priors[c], {}});
                Node &child = node->edges.back().node;
                child.terminal = term;
                child.tv = term ? terminalValue(cb, cfg.z_spec) : 0.0;
            }

            if (node == &t.root)
            {
                mixRootNoise(t, cfg);
            }
        }
        for (Node *n : t.path)
        {
            n->N += 1.0;
            n->W += val;
        }
    }

    // 便宜档预算 = max(2, round(sims × frac))；frac = 0 返回 0（关）。
    static int cheapBudget(const SelfplayConfig &cfg)
    {
        if (!(cfg.sims_cheap_frac > 0.0))
        {
            return 0;
        }
        return std::max(2, static_cast<int>(std::llround(static_cast<double>(cfg.sims) * cfg.sims_cheap_frac)));
    }

    int nextSims(Tree &t, const SelfplayConfig &cfg)
    {
        int s = cfg.sims;
        const int cheap = cheapBudget(cfg);
        // 两档一样时不抽签（不耗 rng 流）。
        if (cheap > 0 && cheap < cfg.sims && cfg.full_frac < 1.0)
        {
            std::uniform_real_distribution<double> u01(0.0, 1.0);
            s = u01(t.rng) < cfg.full_frac ? cfg.sims : cheap;
        }

        if (cfg.long_decay_start > 0 && t.steps > cfg.long_decay_start)
        {
            const double mult = static_cast<double>(cfg.long_decay_start) / static_cast<double>(t.steps);
            const int floorSims = cheap > 0 ? cheap : kDecayFloorBare;
            const int scaled = static_cast<int>(std::llround(static_cast<double>(s) * mult));
            s = std::max(std::min(floorSims, cfg.sims), std::min(s, scaled));
        }
        return s;
    }

    Frame recordFrame(Tree &t)
    {
        Node *root = &t.root;
        Frame f;
        f.obs = sts::collect::snapshotOf(t.real);
        for (const auto &a : enumerateActions(t.real))
        {
            f.legal.push_back(actionToIdx(t.real, a));
        }

        if (f.legal.size() != root->edges.size())
        {
            throw std::runtime_error("根边与合法动作枚举对不上（π 的下标推不出来）");
        }
        for (std::size_t i = 0; i < root->edges.size(); ++i)
        {
            f.pi_idx.push_back(f.legal[i]);
            f.pi_n.push_back(static_cast<int>(root->edges[i].node.N));
        }
        f.is_sel = t.real.inputState == InputState::CARD_SELECT;
        return f;
    }

    namespace
    {
        void resetRoot(Tree &t, const SelfplayConfig &cfg)
        {
            t.root = Node{};
            t.root.terminal = t.real.outcome != Outcome::UNDECIDED;
            t.root.tv = Tree::termValue(t.real, cfg.z_spec);
        }
    } // namespace

    void resetTree(Tree &t, const SelfplayConfig &cfg)
    {
        resetRoot(t, cfg);
    }

    bool stepAndAdvance(Tree &t, const SelfplayConfig &cfg, Frame &f)
    {
        Node *root = &t.root;
        std::size_t pick = 0;
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        bool eps_pick = false;
        if (cfg.eps > 0.0 && u01(t.rng) < cfg.eps)
        {
            std::uniform_int_distribution<std::size_t> ui(0, f.legal.size() - 1);
            pick = ui(t.rng);
            eps_pick = true;
        }
        if (!eps_pick && (cfg.sample || t.steps < cfg.temp_moves))
        {
            std::vector<double> w(root->edges.size());
            for (std::size_t i = 0; i < w.size(); ++i)
            {
                w[i] = static_cast<double>(root->edges[i].node.N);
            }
            std::discrete_distribution<std::size_t> dd(w.begin(), w.end());
            pick = dd(t.rng);
        }
        else if (!eps_pick)
        {
            std::int64_t bestN = -1;
            for (std::size_t i = 0; i < root->edges.size(); ++i)
            {
                const std::int64_t n = static_cast<std::int64_t>(root->edges[i].node.N);
                if (n > bestN)
                {
                    bestN = n;
                    pick = i;
                }
            }
        }
        f.a_idx = f.legal[pick];
        root->edges[pick].action.execute(t.real);

        Node child = std::move(root->edges[pick].node);
        t.root = std::move(child);

        mixRootNoise(t, cfg);
        ++t.steps;
        if (t.real.outcome != Outcome::UNDECIDED || t.steps >= cfg.max_steps)
        {
            t.out.term = readTerm(t.real);
            t.out.outcome = t.real.outcome;
            t.finished = true;
        }
        return t.finished;
    }
} // namespace sts::collect::detail
