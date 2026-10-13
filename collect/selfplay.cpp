//
// 自对弈热循环的实现（导出面在 `selfplay.h`）—— 形状（为什么长这样）在**那个头**里，这里只记
// 两条与**本 TU** 有关的事：
//
// ⚠️ 本 TU 链 libtorch（经 `torch_eval.h`）⇒ 进程里会先 `import torch`（现在是 `slaythespire`
//    包自己做的，不再要求每个调用方先 import；见 `bindings/slaythespire_pkg/__init__.py`）。
// ⚠️ 树**不存节点状态**（只有根 + 从根重放），与引擎 `BattleScumSearcher2` 同法 ⇒ 内存 O(1)。
//
#include "selfplay.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "action.h"     // 枚举 / 编码 / 反解 / 展开（`sts::collect`）
#include "encs.h"       // 遭遇表（`encJobs` 铺任务）
#include "scenebuild.h" // buildStaged / ascensionOf（造局面的唯一入口）
#include "shardio.h"    // ShardWriter / shardAddColumns（落盘容器）
#include "zvalue.h"     // terminalValue
#include "snapshot.h"   // snapshotOf（`snapshotObs` 的实现）
#include "torch_eval.h"

#include "sim/search/BattleScumSearcher2.h" // evaluateEndState

// ⚠️ `TorchEvaluator` 仍住在 `sts::py`（`bindings/torch_eval.h`）—— 本目录一律在
//    `sts::collect` 里，所以显式带进来；等它也搬进来就可以删掉。
using sts::py::TorchEvaluator;

namespace sts::collect
{
    Term readTerm(const BattleContext &b)
    {
        Term t;
        t.t_win = b.outcome == Outcome::PLAYER_VICTORY ? 1 : 0;
        t.t_hp = b.player.curHp;
        t.t_max_hp = b.player.maxHp;
        t.t_turn = b.turn;
        int cur = 0, mx = 0;
        for (int i = 0; i < b.monsters.monsterCount; ++i)
        {
            cur += b.monsters.arr[i].curHp;
            mx += b.monsters.arr[i].maxHp;
        }
        t.t_enemy = static_cast<double>(cur) / static_cast<double>(std::max(1, mx));
        t.t_escore = search::BattleScumSearcher2::evaluateEndState(b);
        return t;
    }

    std::array<float, sts::collect::Snap::snapshotLen> snapshotObs(const BattleContext &b)
    {
        return sts::collect::snapshotOf(b);
    }

    namespace
    {
        // 观测宽度（编译期定死：`Frame::obs` 就是 `std::array<float, snapshotLen>`）。
        constexpr int kObsLen = Snap::snapshotLen;

        // virtual loss。⚠️ 它在本形态下**只活着一个作用**：下降走到第 d≥1 层时，当前节点的 `nv`
        // 已被上一次 while 迭代加过 ⇒ `sq = sqrt(N + nv) = sqrt(N + 1)`（探索系数按"父节点多访问
        // 了一次"算）。`q` / `tot` 那两处是死的（候选子节点在本轮从未被标记）—— 别拿"标准
        // virtual loss 的语义"套它：删掉 `nv` 会让深层探索系数偏小、几十步后分叉。
        // 它曾经是 `SelfplayConfig::virtual_loss`，但没有任何调用方给它赋过别的值 ⇒ 收成常量。
        constexpr double kVirtualLoss = 1.0;

        // 与 Python `azsearch.MCTSNode` 同形；不回指父节点（回传靠 `path`，树是单向的）。
        // ⚠️ **不存 `BattleContext`**：那是引擎 `BattleScumSearcher2` 的做法（只有 `rootState` + 动作栈，
        //    每趟 playout 从根重放）。每节点存一份克隆会让内存 = O(节点数 × 2KB)、且展开时每动作 clone
        //    一次。改成"每棵树一份 state + 重放"后内存 = O(1)，而**数值完全不变** —— 原来的 `child.b`
        //    就是 `execute(parent.b, a)` 复合出来的，从根重放同一串动作的复合方式一模一样（确定性模拟）。
        // ⚠️ `nv` 不能省：它进了 `sq = sqrt(N + nv)`（见 `kVirtualLoss` 的注释）。
        struct Node
        {
            search::Action action; // 到本节点的动作（从父节点重放它就能到本节点）
            double prior = 0.0;
            double N = 0.0, W = 0.0, nv = 0.0;
            bool terminal = false;
            double tv = 0.0;                                             // 只有 terminal 有值
            std::vector<std::pair<int, std::unique_ptr<Node>>> children; // 插入序 = 合法动作枚举序

            Node() = default;
            Node(search::Action a, double p, bool term, double v)
                : action(a), prior(p), terminal(term), tv(v) {}
        };

        // 一场的活状态：**一棵树一个**，且**只被拥有它的那条线程**碰（各跑各的）⇒ 不用上锁。
        struct Tree
        {
            std::size_t gid = 0;   // 在原 `battles` 里的下标（结果按它归位）
            BattleContext real;    // 当前决策点的根状态（真实状态链由它推进）
            BattleContext scratch; // 复用缓冲：下降时"从根重放"就落这里（每棵树独占）
            std::unique_ptr<Node> root;
            Node *leaf = nullptr;     // 本轮要前向的叶
            std::vector<Node *> path; // 到该叶的路径（含根与叶）
            int steps = 0;
            bool finished = false;
            GameOut out;
            std::mt19937_64 rng;

            Tree(std::size_t g, BattleContext b, std::uint64_t seed)
                : gid(g), real(std::move(b)), scratch(real), rng(seed)
            {
                root = std::make_unique<Node>();
            }

            static double termValue(const BattleContext &ctx, const std::string &z_spec)
            {
                return ctx.outcome != Outcome::UNDECIDED ? terminalValue(ctx, z_spec) : 0.0;
            }

            // 从根重放到 `path`（沿路径依次 `execute`）—— 引擎 `BattleScumSearcher2` 的做法。
            void replay()
            {
                scratch = real;
                for (std::size_t i = 1; i < path.size(); ++i)
                {
                    path[i]->action.execute(scratch);
                }
            }
        };

        // 返回 true = 该叶是终局（已就地结算）；false = 有叶待前向（`t.leaf` 有效）。
        bool descendOne(Tree &t, const SelfplayConfig &cfg)
        {
            Node *node = t.root.get();
            t.leaf = nullptr;
            t.path.clear();
            t.path.push_back(node);
            while (!node->terminal && !node->children.empty())
            {
                const double sq = std::sqrt(node->N + node->nv);
                Node *best = nullptr;
                double best_u = -std::numeric_limits<double>::infinity();
                for (auto &pr : node->children)
                {
                    Node *ch = pr.second.get();
                    const double tot = ch->N + ch->nv;
                    const double q = tot > 0.0 ? (ch->W - ch->nv * kVirtualLoss) / tot : cfg.q_init;
                    const double u = q + cfg.c_puct * ch->prior * sq / (1.0 + tot);
                    if (u > best_u)
                    {
                        best_u = u;
                        best = ch;
                    }
                }
                if (best == nullptr)
                {
                    break;
                }
                best->nv += kVirtualLoss;
                node = best;
                t.path.push_back(node);
            }

            if (!node->terminal)
            {
                t.leaf = node;
                return false;
            }
            const double tv = node->tv;
            for (std::size_t i = 1; i < t.path.size(); ++i)
            {
                t.path[i]->nv -= kVirtualLoss;
            }
            for (Node *n : t.path)
            {
                n->N += 1.0;
                n->W += tv;
            }
            return true;
        }

        // 展开 + 回传。先重放出叶状态，再**只对候选位置**做 softmax（非法位置的 exp 不进分母）。
        void expandBackup(Tree &t, const SelfplayConfig &cfg, const float *logits, double val)
        {
            Node *node = t.leaf;
            t.replay();
            BattleContext &leaf_state = t.scratch;
            if (node->children.empty())
            {
                auto ex = expandChildren(leaf_state);
                std::vector<float> e;
                e.reserve(ex.first.size());
                float mx = -std::numeric_limits<float>::infinity();
                for (const auto &pr : ex.first)
                {
                    mx = std::max(mx, logits[pr.first]);
                }
                float sum = 0.0f;
                for (const auto &pr : ex.first)
                {
                    const float v = std::exp(logits[pr.first] - mx);
                    e.push_back(v);
                    sum += v;
                }
                std::vector<double> priors;
                priors.reserve(e.size());
                for (float v : e)
                {
                    priors.push_back(static_cast<double>(v / sum));
                }

                // 根节点混入 Dirichlet 噪声（只在**新建根孩子**时施加一次；与 Python 同条件）。
                if (node == t.root.get() && cfg.dirichlet_eps > 0.0 && priors.size() > 1)
                {
                    const double alpha = cfg.dirichlet_total / static_cast<double>(priors.size());
                    std::gamma_distribution<double> gamma(alpha, 1.0);
                    std::vector<double> eta(priors.size());
                    double ysum = 0.0;
                    for (double &y : eta)
                    {
                        y = gamma(t.rng);
                        ysum += y;
                    }
                    for (std::size_t i = 0; i < priors.size(); ++i)
                    {
                        const double ee = ysum > 0.0 ? eta[i] / ysum : 1.0 / static_cast<double>(eta.size());
                        priors[i] = (1.0 - cfg.dirichlet_eps) * priors[i] + cfg.dirichlet_eps * ee;
                    }
                }
                for (std::size_t c = 0; c < ex.first.size(); ++c)
                {
                    const BattleContext &cb = *ex.second[c];
                    const bool term = cb.outcome != Outcome::UNDECIDED;
                    const double tv = term ? terminalValue(cb, cfg.z_spec) : 0.0;
                    node->children.emplace_back(
                        ex.first[c].first,
                        std::make_unique<Node>(ex.first[c].second, priors[c], term, tv));
                }
            }
            for (std::size_t i = 1; i < t.path.size(); ++i)
            {
                t.path[i]->nv -= kVirtualLoss;
            }
            for (Node *n : t.path)
            {
                n->N += 1.0;
                n->W += val;
            }
        }

        // 记录本决策点：根边访问数 / 合法动作 / 是否选牌空间。
        // ⚠️ 根的状态 = `t.real`（树不存节点状态；根就是真实状态）。
        Frame recordFrame(Tree &t)
        {
            Node *root = t.root.get();
            Frame f;
            f.obs = snapshotObs(t.real);
            for (const auto &pr : root->children)
            {
                f.pi_idx.push_back(pr.first);
                f.pi_n.push_back(static_cast<int>(pr.second->N));
            }
            for (const auto &a : enumerateActions(t.real))
            {
                f.legal.push_back(actionToIdx(t.real, a));
            }
            f.is_sel = t.real.inputState == InputState::CARD_SELECT;
            return f;
        }

        // 把 `t.root` 换成"从 `t.real` 新建的只有根的树"（随机动作走出树外、无可复用子树时用）。
        void resetTree(Tree &t, const SelfplayConfig &cfg)
        {
            t.root = std::make_unique<Node>();
            t.root->terminal = t.real.outcome != Outcome::UNDECIDED;
            t.root->tv = Tree::termValue(t.real, cfg.z_spec);
        }

        // 一次决策点的动作选择 + 执行 + 树推进（与 Python `_explore_idx` 同序：eps → 采样 → argmax）。
        bool stepAndAdvance(Tree &t, const SelfplayConfig &cfg, Frame &f, bool exploring,
                            std::uniform_real_distribution<double> &u01)
        {
            Node *root = t.root.get();
            std::size_t pick = 0;
            bool took_edge = true;
            if (exploring && cfg.eps > 0.0 && u01(t.rng) < cfg.eps)
            {
                std::uniform_int_distribution<std::size_t> ui(0, f.legal.size() - 1);
                const int want = f.legal[ui(t.rng)];
                took_edge = false;
                for (std::size_t i = 0; i < root->children.size(); ++i)
                {
                    if (root->children[i].first == want)
                    {
                        pick = i;
                        took_edge = true;
                        break;
                    }
                }
                if (!took_edge)
                {
                    f.a_idx = want;
                    idxToAction(t.real, want).execute(t.real);
                    resetTree(t, cfg);
                    ++t.steps;
                    if (t.real.outcome != Outcome::UNDECIDED || t.steps >= cfg.max_steps)
                    {
                        t.out.term = readTerm(t.real);
                        t.out.outcome = t.real.outcome;
                        t.finished = true;
                    }
                    return t.finished;
                }
            }
            if (exploring && (cfg.sample || t.steps < cfg.temp_moves))
            {
                std::vector<double> w(root->children.size());
                for (std::size_t i = 0; i < w.size(); ++i)
                {
                    w[i] = static_cast<double>(root->children[i].second->N);
                }
                std::discrete_distribution<std::size_t> dd(w.begin(), w.end());
                pick = dd(t.rng);
            }
            else
            {
                std::int64_t bestN = -1;
                for (std::size_t i = 0; i < root->children.size(); ++i)
                {
                    const std::int64_t n = static_cast<std::int64_t>(root->children[i].second->N);
                    if (n > bestN)
                    {
                        bestN = n;
                        pick = i;
                    }
                }
            }
            f.a_idx = root->children[pick].first;
            root->children[pick].second->action.execute(t.real);
            // 该边的孩子成为新根（其余孩子与旧根一起丢）——先 `release` 再 `reset`（子树连统计一起保留）。
            Node *child = root->children[pick].second.release();
            t.root.reset(child);
            t.root->action = search::Action();
            ++t.steps;
            if (t.real.outcome != Outcome::UNDECIDED || t.steps >= cfg.max_steps)
            {
                t.out.term = readTerm(t.real);
                t.out.outcome = t.real.outcome;
                t.finished = true;
            }
            return t.finished;
        }

        // ── 前向服务：`submit(obs) -> 阻塞到结果`（KataGo `NNEvaluator::evaluate` 同形）────────
        //
        // 「批」是**独立的一层**：搜索线程各自阻塞在自己的请求上，服务线程攒批跑一次前向
        // ⇒ 树与树之间**不互相等**，批只由"当时到了谁"决定（原来的相位屏障让每轮所有树互相等）。
        //
        // ⚠️ 代价：批的组成不再只由树序决定 ⇒ **K>1 不再逐位可复现**（前向本来就与批宽相关：
        //    GEMM 归约顺序 / kernel 选择）。只有 **K=1**（批恒 1 行、单树）仍可与 golden 逐位对拍；
        //    K>1 的性质由"同参数两次跑一致"这类统计断言守（见 `_chk_selfplay.py`）。
        class NNEval
        {
        public:
            struct Req
            {
                std::array<float, kObsLen> obs; // 观测（**拷进来**，调用方随后可复用输入）
                std::vector<float> pa, ps, val; // 结果（服务线程填）
                std::size_t seq = 0;            // 入队序（拼批按它，批组成 = 提交顺序）
                bool done = false;
            };

            explicit NNEval(TorchEvaluator &eval) : eval_(eval) {}

            /// 起服务线程。⚠️ **必须在主线程跑过一次 `eval.eval(...)` 之后调**：
            /// `TorchEvaluator` 的 `p_act_dim` / CUDA 上下文 / 执行计划都是首次前向时才建立的，
            /// 在此之前并跑前向 = 访问违例（实测 K=1 就崩）。
            void start(int n_server)
            {
                p_act_dim_ = eval_.p_act_dim;
                p_sel_dim_ = eval_.p_sel_dim;
                if (p_act_dim_ <= 0 || p_sel_dim_ <= 0)
                {
                    throw std::runtime_error("NNEval：起服务前必须先跑过一次前向（头宽度未知）");
                }
                for (int t = 0; t < std::max(1, n_server); ++t)
                {
                    servers_.emplace_back([this]
                                          { serve(); });
                }
            }

            ~NNEval()
            {
                {
                    std::lock_guard<std::mutex> lk(m_);
                    stop_ = true;
                }
                cv_.notify_all();
                for (auto &th : servers_)
                {
                    th.join();
                }
            }

            NNEval(const NNEval &) = delete;
            NNEval &operator=(const NNEval &) = delete;

            /// 提交并**阻塞**到结果就绪（结果写在 `r` 里）。`Req` 可以复用（一棵树一个）。
            void submit(Req &r)
            {
                {
                    std::lock_guard<std::mutex> lk(m_);
                    r.seq = next_seq_++;
                    // ⚠️ 必须每次重置：`Req` 是**复用**的（树把它建在循环外），上一轮的 `done` 还是
                    //    true ⇒ 不复位就直接"返回"了，于是树上读到上一轮的结果、且与还在写它的服务
                    //    线程并发 ⇒ 数据竞争（实测：堆被踩烂，进程在收尾时 0xC0000005）。
                    r.done = false;
                    pending_.push_back(&r);
                }
                cv_.notify_all();
                std::unique_lock<std::mutex> lk(m_);
                while (!r.done)
                {
                    cv_done_.wait(lk);
                }
            }

            int pa_dim() const { return p_act_dim_; }
            int ps_dim() const { return p_sel_dim_; }

        private:
            void serve()
            {
                for (;;)
                {
                    std::vector<Req *> batch;
                    {
                        std::unique_lock<std::mutex> lk(m_);
                        while (!stop_ && pending_.empty())
                        {
                            cv_.wait(lk);
                        }
                        if (pending_.empty())
                        {
                            return;
                        } // `stop_` 且队列空
                        std::sort(pending_.begin(), pending_.end(),
                                  [](Req *a, Req *b)
                                  { return a->seq < b->seq; });
                        batch.swap(pending_);
                    }
                    std::vector<float> buf(batch.size() * static_cast<std::size_t>(kObsLen));
                    for (std::size_t i = 0; i < batch.size(); ++i)
                    {
                        std::copy(batch[i]->obs.begin(), batch[i]->obs.end(),
                                  buf.begin() + static_cast<std::ptrdiff_t>(i * kObsLen));
                    }
                    auto fwd = eval_.eval(buf.data(), static_cast<std::int64_t>(batch.size()), kObsLen);
                    const float *pa = fwd[0].data_ptr<float>();
                    const float *ps = fwd[1].data_ptr<float>();
                    const float *val = fwd[2].data_ptr<float>();
                    const auto da = static_cast<std::size_t>(p_act_dim_);
                    const auto ds = static_cast<std::size_t>(p_sel_dim_);
                    for (std::size_t i = 0; i < batch.size(); ++i)
                    {
                        Req &r = *batch[i];
                        r.pa.assign(pa + i * da, pa + (i + 1) * da);
                        r.ps.assign(ps + i * ds, ps + (i + 1) * ds);
                        r.val.assign(val + i, val + i + 1);
                    }
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        for (Req *r : batch)
                        {
                            r->done = true;
                        }
                    }
                    cv_done_.notify_all();
                }
            }

            TorchEvaluator &eval_;
            int p_act_dim_ = 0, p_sel_dim_ = 0;
            std::vector<std::thread> servers_;
            std::vector<Req *> pending_;
            std::size_t next_seq_ = 0;
            bool stop_ = false;
            std::mutex m_;
            std::condition_variable cv_, cv_done_;
        };

    } // namespace

    void selfplay(const std::vector<BattleContext> &battles, const std::vector<std::uint64_t> &seeds,
                  const SelfplayConfig &cfg, const std::function<void(std::size_t, GameOut &&)> &emit)
    {
        if (battles.size() != seeds.size())
        {
            throw std::runtime_error("局数与 seeds 数量不一致");
        }
        if (battles.empty())
        {
            return;
        }
        if (cfg.sims < 2)
        {
            // 与 Python 的 `sims ≥ 2` 契约同：1 次模拟只展开根、不给任何根边加访问 ⇒ π 全零。
            throw std::runtime_error("sims 太小：至少 2（否则本场搜不出可记的 π）");
        }
        TorchEvaluator eval(cfg.ts_path, cfg.device, cfg.threads);
        if (eval.p_act_dim == 0)
        {
            // 先要一次头宽度（1 行假输入）：维度在第一次前向后才有定义；也让 CUDA 上下文与执行计划
            // 在**本线程**上建好（`TorchEvaluator` 必须在同一线程首次使用）。
            std::vector<float> probe(static_cast<std::size_t>(kObsLen), 0.0f);
            eval.eval(probe.data(), 1, kObsLen);
        }
        terminalValue(battles[0], cfg.z_spec); // 拼错的 spec 要在这里响亮失败

        const std::size_t K = battles.size();
        std::vector<Tree> trees;
        trees.reserve(K);
        for (std::size_t j = 0; j < K; ++j)
        {
            trees.emplace_back(j, battles[j], seeds[j]);
        }

        std::uniform_real_distribution<double> u01(0.0, 1.0);
        const bool exploring = cfg.sample || cfg.eps > 0.0 || cfg.temp_moves > 0 || cfg.dirichlet_eps > 0.0;

        // ⚠️ 顺序：`TorchEvaluator` 的**首次前向在主线程**跑（下面这次 probe 就是它）⇒ 头宽度、
        //    CUDA 上下文与执行计划都建立好之后，前向服务才允许起线程（否则并跑 = 访问违例）。
        {
            std::vector<float> probe(static_cast<std::size_t>(kObsLen), 0.0f);
            eval.eval(probe.data(), 1, kObsLen);
        }
        NNEval nneval(eval);
        nneval.start(/*n_server=*/1);

        // 一棵树跑到底：**本线程独占它**（`trees[i]` 只被这里碰，不需要锁）。
        // 跑完把结果放进 `slots[i]`（= 第 i 场的**交付序**位置），叫醒写者 ⇒ 主线程按场序交付。
        std::vector<std::optional<GameOut>> slots(K);
        std::mutex m;
        std::condition_variable cv_done; // 写者等"队头就绪"
        std::size_t emitted = 0;         // 已交付的场数（= 队头）
        std::exception_ptr err;          // 线程里抛异常会 terminate ⇒ 录下、主线程重抛

        auto run_one = [&](std::size_t i)
        {
            try
            {
                Tree &t = trees[i];
                NNEval::Req req; // 一棵树一个（复用 ⇒ 每步不再为观测/结果重新分配）
                for (;;)
                {
                    if (t.finished)
                    {
                        break;
                    }
                    // ---- 搜索 `cfg.sims` 轮：下降 1 条路径 → 前向（阻塞）→ 展开回传 ----
                    for (int r = 0; r < cfg.sims; ++r)
                    {
                        if (t.finished)
                        {
                            break;
                        }
                        if (descendOne(t, cfg))
                        {
                            continue;
                        } // 该叶是终局：已就地结算
                        t.replay();
                        req.obs = snapshotObs(t.scratch);
                        nneval.submit(req); // **阻塞**到批跑完（其它树各跑各的）
                        const bool is_sel = t.scratch.inputState == InputState::CARD_SELECT;
                        const float *logits = (is_sel ? req.ps : req.pa).data();
                        expandBackup(t, cfg, logits, static_cast<double>(req.val[0]));
                    }
                    // ---- 记录本决策点 → 选动作 → 执行 → 树推进 ----
                    Frame f = recordFrame(t);
                    std::int64_t total = 0;
                    for (std::int64_t n : f.pi_n)
                    {
                        total += n;
                    }
                    if (total <= 0)
                    {
                        throw std::runtime_error("根没有任何被访问过的边（本场搜不出可记的 π）");
                    }
                    t.out.frames.push_back(std::move(f));
                    stepAndAdvance(t, cfg, t.out.frames.back(), exploring, u01);
                }
                std::lock_guard<std::mutex> lk(m);
                slots[t.gid] = std::move(t.out); // 槽 = 第 `gid` 场的**交付序**位置
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lk(m);
                if (!err)
                {
                    err = std::current_exception();
                }
            }
            cv_done.notify_all();
        };

        // 一棵树一条线程（树线程阻塞在自己的请求上、不占 CPU ⇒ 只有 `K` 才给得出"批宽 = K"）。
        // ⚠️ `i` 必须**按值**捕获：按引用捕获会让线程启动时读到循环结束后的值。
        std::vector<std::thread> pool;
        pool.reserve(K);
        for (std::size_t i = 0; i < K; ++i)
        {
            pool.emplace_back([&run_one, i]
                              { run_one(i); });
        }
        // 主线程 = **唯一的交付者**：只处理队头（`emitted`），写完当场释放那一场的内存。
        for (;;)
        {
            GameOut g;
            {
                std::unique_lock<std::mutex> lk(m);
                while (!(err || emitted >= K || slots[emitted].has_value()))
                {
                    cv_done.wait(lk);
                }
                if (err || emitted >= K)
                {
                    break;
                }
                g = std::move(*slots[emitted]);
                slots[emitted].reset();
            }
            emit(emitted, std::move(g));
            ++emitted;
        }
        for (auto &th : pool)
        {
            th.join();
        }
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    // ── 整段采集：任务表 → 造局面 → 搜索 → 落盘（与 `teacherRun` 同形）────────────────
    std::vector<std::string> selfplayRun(int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed,
                                         const std::string &out_dir, int shard_rows)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        ShardWriter w;
        shardAddColumns(w, "mcts");
        std::vector<std::string> written;
        int part = 0;
        char pathbuf[512];
        auto setPath = [&]
        {
            // ⚠️ 分片号**补零**：读侧按名字序拼行序 ⇒ `blk2` 必须排在 `blk10` 前面。
            std::snprintf(pathbuf, sizeof(pathbuf), "%s/blk%05d.bin", out_dir.c_str(), part);
            w.path = pathbuf;
        };
        setPath();

        // **跑一场、写一场**：`emit` 按交付序回调 ⇒ 落盘顺序即数据行序（同一场的行连续；主线程
        // 是唯一的写者，写完当场释放那一场的内存）⇒ 内存不随 `games` 涨（旧的"整块跑完再落盘"会）。
        // ⚠️ 分块只改内存峰值，不改数据：与 `shard_rows` 正交（那个管**文件**切多大）。
        const std::vector<EncJob> jobs = encJobs(n_per, start_seed);
        const std::size_t per = static_cast<std::size_t>(std::max(1, cfg.games));
        for (std::size_t i = 0; i < jobs.size(); i += per)
        {
            const std::size_t n = std::min(per, jobs.size() - i);
            std::vector<BattleContext> battles;
            std::vector<std::uint64_t> seeds;
            battles.reserve(n);
            seeds.reserve(n);
            for (std::size_t k = 0; k < n; ++k)
            {
                const EncJob &j = jobs[i + k];
                // 造局面的**唯一入口**（与 Python 的 `scene.stage_battle` 同一份实现）。
                battles.push_back(buildStaged(j.seed, j.enc, j.floor, ascensionOf(j.seed)).b);
                seeds.push_back(j.seed);
            }
            selfplay(battles, seeds, cfg,
                     [&](std::size_t, GameOut &&g)
                     {
                         for (const Frame &fr : g.frames)
                         {
                             w.b.add(fr, g.term);
                             w.endRow();
                             if (w.size() >= shard_rows)
                             {
                                 written.push_back(w.flush());
                                 ++part;
                                 setPath();
                             }
                         }
                     });
        }
        const std::string last = w.flush();
        if (!last.empty())
        {
            written.push_back(last);
        }
        return written;
    }

    void registerSelfplay(pybind11::module_ &m)
    {
        namespace py = pybind11;

        py::class_<SelfplayConfig>(m, "SelfplayConfig")
            .def(py::init<>())
            .def_readwrite("ts_path", &SelfplayConfig::ts_path)
            .def_readwrite("z_spec", &SelfplayConfig::z_spec)
            .def_readwrite("device", &SelfplayConfig::device)
            .def_readwrite("sims", &SelfplayConfig::sims)
            .def_readwrite("max_steps", &SelfplayConfig::max_steps)
            .def_readwrite("threads", &SelfplayConfig::threads)
            .def_readwrite("games", &SelfplayConfig::games)
            .def_readwrite("c_puct", &SelfplayConfig::c_puct)
            .def_readwrite("q_init", &SelfplayConfig::q_init)
            .def_readwrite("sample", &SelfplayConfig::sample)
            .def_readwrite("eps", &SelfplayConfig::eps)
            .def_readwrite("temp_moves", &SelfplayConfig::temp_moves)
            .def_readwrite("dirichlet_total", &SelfplayConfig::dirichlet_total)
            .def_readwrite("dirichlet_eps", &SelfplayConfig::dirichlet_eps);

        m.def(
            "selfplay_run",
            [](int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed,
               const std::string &out_dir, int shard_rows)
            {
                // 整段不碰 Python ⇒ 放 GIL（与 `teacher_run` 同）。
                py::gil_scoped_release release;
                return sts::collect::selfplayRun(n_per, cfg, start_seed, out_dir, shard_rows);
            },
            py::arg("n_per"), py::arg("cfg"), py::arg("start_seed"), py::arg("out_dir"),
            py::arg("shard_rows") = 200000,
            "**整段采集**（任务表 + 造局面 + 搜索 + 落盘全在 C++，不经过 Python）：每项 `n_per` 场，\n"
            "`cfg.games` = 并肩几棵树（= 前向批宽）。分片写进 `out_dir`，返回路径列表。");
    }

} // namespace sts::collect
