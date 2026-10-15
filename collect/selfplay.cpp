#include "selfplay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
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
#include "shardio.h"    // ShardWriter（落盘容器）
#include "zvalue.h"     // terminalValue
#include "snapshot.h"   // snapshotOf（`snapshotObs` 的实现）
#include "net_eval.h"

#include "sim/search/BattleScumSearcher2.h" // evaluateEndState

// ⚠️ `NetEvaluator` 仍住在 `sts::py`（`bindings/net_eval.h`，唯一引擎 = ONNX Runtime）—— 本目录一律在
//    `sts::collect` 里，所以显式带进来；等它也搬进来就可以删掉。
using sts::py::NetEvaluator;

namespace sts::collect
{
    Term readTerm(const BattleContext &b)
    {
        Term t;
        t.is_win = b.outcome == Outcome::PLAYER_VICTORY;
        t.cur_hp = b.player.curHp;
        t.max_hp = b.player.maxHp;
        t.turn = b.turn;
        for (int i = 0; i < b.monsters.monsterCount; ++i)
        {
            t.enemy_cur_hp += b.monsters.arr[i].curHp;
            t.enemy_max_hp += b.monsters.arr[i].maxHp;
        }
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

        struct Edge;

        struct Node
        {
            double N = 0.0, W = 0.0;
            bool terminal = false;
            double tv = 0.0;         // 只有 terminal 有值
            std::vector<Edge> edges; // 插入序 = 合法动作枚举序（⚠️ π 的下标由这条序推出来，别改成别的序）
        };

        // 边上**不存**统一下标：下标 = `Frame::legal` 的同位元素（根边的插入序就是枚举序，见上）。
        struct Edge
        {
            search::Action action;
            double prior = 0.0;
            Node node;
        };

        // 一场的活状态：**一棵树一个**，且**只被拥有它的那条线程**碰（各跑各的）⇒ 不用上锁。
        struct Tree
        {
            std::size_t gid = 0;   // 在原 `battles` 里的下标（结果按它归位）
            BattleContext real;    // 当前决策点的根状态（真实状态链由它推进）
            BattleContext scratch; // 复用缓冲：下降时"从根重放"就落这里（每棵树独占）
            Node root;
            Node *leaf = nullptr;     // 本轮要前向的叶
            std::vector<Node *> path; // 到该叶的路径（含根与叶）
            std::vector<search::Action> actionStack;
            int steps = 0;
            bool finished = false;
            GameOut out;
            std::mt19937_64 rng;

            Tree(std::size_t g, BattleContext b, std::uint64_t seed)
                : gid(g), real(std::move(b)), scratch(real), rng(seed)
            {
            }

            static double termValue(const BattleContext &ctx, const std::string &z_spec)
            {
                return ctx.outcome != Outcome::UNDECIDED ? terminalValue(ctx, z_spec) : 0.0;
            }

            // 从根重放到 `path`（沿路径依次 `execute`）—— 引擎 `BattleScumSearcher2` 的做法。
            void replay()
            {
                scratch = real;
                for (const auto &action : actionStack)
                {
                    action.execute(scratch);
                }
            }
        };

        // 返回 true = 该叶是终局（已就地结算）；false = 有叶待前向（`t.leaf` 有效）。
        bool descendOne(Tree &t, const SelfplayConfig &cfg)
        {
            Node *node = &t.root;
            t.leaf = nullptr;
            t.path.clear();
            t.actionStack.clear();
            t.path.push_back(node);
            while (!node->terminal && !node->edges.empty())
            {
                // 教科书 AlphaZero 的 PUCT：探索项用**父节点**的 `N`（全深度一致；`nv` 已删，见上）。
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

        // 把 Dirichlet 噪声混进**当前根的**先验（每个新根混一次）。`dirichlet_eps <= 0` 或根只有
        // 一个孩子时不动（也是**不消耗 rng** 的短路 ⇒ 关噪声时逐位不变）。
        //
        // ⚠️ 为什么是个函数而不只在展开里做：根**不止在展开时形成** —— 子树提升为新根时，它的孩子们
        //    是上一轮搜索里当**内部节点**展开的（那时 `node != &t.root` ⇒ 没混噪声），于是"每个决策
        //    点都有根噪声"这条不成立（历史 bug：只有每场的**第一个**决策点有噪声，之后全靠温度/采样
        //    探索）。现在展开（`expandBackup`）与提升（`stepAndAdvance`）各调一次，同一个根只混一次
        //    —— 提升过的根其孩子已非空，展开那条路的 `edges.empty()` 不再命中。
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

        // 展开 + 回传。先重放出叶状态，再**只对候选位置**做 softmax（非法位置的 exp 不进分母）。
        void expandBackup(Tree &t, const SelfplayConfig &cfg, const float *logits, double val)
        {
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
                // 这一层正好是根 ⇒ 新根的噪声在这里混（提升来的根见 `mixRootNoise` 的调用点）。
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

        // 下一个决策点的模拟预算（随机化上限，见 `SelfplayConfig::sims_cheap`）。
        // ⚠️ 关着（`sims_cheap <= 0`）时**不碰 rng**：否则探索/温度那几条流会跟着变。
        int nextSims(Tree &t, const SelfplayConfig &cfg)
        {
            if (cfg.sims_cheap <= 0 || cfg.full_frac >= 1.0)
            {
                return cfg.sims;
            }
            std::uniform_real_distribution<double> u01(0.0, 1.0);
            return u01(t.rng) < cfg.full_frac ? cfg.sims : cfg.sims_cheap;
        }

        // 记录本决策点：根边访问数 / 合法动作 / 是否选牌空间。
        // ⚠️ 根的状态 = `t.real`（树不存节点状态；根就是真实状态）。
        // ⚠️ π 的下标**不随边存**：根边的插入序 == `enumerateActions` 枚举序 ⇒ 第 i 条边的下标就是
        //    `legal[i]`（于是 `pi_idx` 与 `legal` 逐位相同；落盘仍各写一列，读侧口径不变）。
        Frame recordFrame(Tree &t)
        {
            Node *root = &t.root;
            Frame f;
            f.obs = snapshotObs(t.real);
            for (const auto &a : enumerateActions(t.real))
            {
                f.legal.push_back(actionToIdx(t.real, a));
            }
            // 长度不等 = 上面那条序不成立（根边不是从**当前状态**枚举来的）⇒ 响亮失败，别静默记下错位的 π。
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

        // 把 `t.root` 换成"从 `t.real` 新建的只有根的树"（随机动作走出树外、无可复用子树时用）。
        void resetTree(Tree &t, const SelfplayConfig &cfg)
        {
            t.root = Node{};
            t.root.terminal = t.real.outcome != Outcome::UNDECIDED;
            t.root.tv = Tree::termValue(t.real, cfg.z_spec);
        }

        // 一次决策点的动作选择 + 执行 + 树推进（与 Python `_explore_idx` 同序：eps → 采样 → argmax）。
        bool stepAndAdvance(Tree &t, const SelfplayConfig &cfg, Frame &f, bool exploring,
                            std::uniform_real_distribution<double> &u01)
        {
            Node *root = &t.root;
            std::size_t pick = 0;
            // ε 探索：**抽中就走**。⚠️ 曾经这里抽中之后还继续往下走采样 / argmax 那两段
            // ⇒ `pick` 被覆盖，ε 分支只在"抽到树外的动作"时真生效（抽中树内边等于白抽）。
            bool eps_pick = false;
            if (exploring && cfg.eps > 0.0 && u01(t.rng) < cfg.eps)
            {
                std::uniform_int_distribution<std::size_t> ui(0, f.legal.size() - 1);
                const int want = f.legal[ui(t.rng)];
                for (std::size_t i = 0; i < root->edges.size(); ++i)
                {
                    if (f.legal[i] == want)
                    {
                        pick = i;
                        eps_pick = true;
                        break;
                    }
                }
                if (!eps_pick)
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
            if (!eps_pick && exploring && (cfg.sample || t.steps < cfg.temp_moves))
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
            // 先移出子节点，再替换根，保留子树统计并避免销毁移动源。
            Node child = std::move(root->edges[pick].node);
            t.root = std::move(child);
            // 新根 = 新决策点 ⇒ 它的先验在这里重混一次噪声（展开那条路见 `expandBackup`）。
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

        // ── 前向服务：`push(obs)` 入队即返回，跑完一批再把"可以做下一步的树"交回 worker ──────────
        //
        // 「批」是**独立的一层**：worker 把当前要前向的观测塞进队列就**不占线程了**（谁抢到下一棵
        // 可推进的树谁就推进），服务线程攒批跑一次前向，跑完把批里各请求**所属的树**回投给 worker 池。
        // ⇒ 树与树**不互相等**，线程也**不绑定在任何一棵树上**；批只由"当时到了谁"决定。
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
                std::size_t owner = 0;          // 请求属于哪棵树（跑完按它回投 worker 池）
                bool done = false;
            };

            /// `cpu_evals`：尾段（窄批）专用的会话，**每个服务线程一个**（各持一份内部缓冲 ⇒ 线程
            /// 之间无共享）。空 = 关（全部走 `eval_`，与改造前**逐位同**）。`cpu_rows` = 待服行数 ≤
            /// 它就走 CPU（0 也当关）。
            NNEval(NetEvaluator &eval, std::vector<std::unique_ptr<NetEvaluator>> cpu_evals, int cpu_rows)
                : eval_(eval), cpu_evals_(std::move(cpu_evals)),
                  cpu_rows_(cpu_evals_.empty() ? 0 : std::max(0, cpu_rows)) {}

            /// 尾段分流是否开着（诊断/日志用）。
            bool hybrid() const { return !cpu_evals_.empty() && cpu_rows_ > 0; }

            /// 起服务线程：**1 条主设备**（宽批）+ `cpu_evals_` 条 CPU（窄批）。
            /// ⚠️ **必须在主线程跑过一次 `eval.eval(...)` 之后调**：
            /// `NetEvaluator` 的 `p_act_dim` / CUDA 上下文 / 执行计划都是首次前向时才建立的，
            /// 在此之前并跑前向 = 访问违例（实测 K=1 就崩）。CPU 会话同理（各自先探一次）。
            void start(int n_server)
            {
                p_act_dim_ = eval_.p_act_dim;
                p_sel_dim_ = eval_.p_sel_dim;
                if (p_act_dim_ <= 0 || p_sel_dim_ <= 0)
                {
                    throw std::runtime_error("NNEval：起服务前必须先跑过一次前向（头宽度未知）");
                }
                for (const auto &ce : cpu_evals_)
                {
                    if (ce->p_act_dim != p_act_dim_ || ce->p_sel_dim != p_sel_dim_)
                    {
                        throw std::runtime_error("NNEval：CPU 会话的头宽度与主会话不一致"
                                                 "（两份存档不是同一张图？）");
                    }
                }
                for (int t = 0; t < std::max(1, n_server); ++t)
                {
                    servers_.emplace_back([this]
                                          { serve(eval_, /*cpu_role=*/false); });
                }
                if (hybrid())
                {
                    // 日志一行：这一档分流是**新配方**（尾段进 CPU ⇒ 数据谱系里的数值路径变了）。
                    std::fprintf(stderr, "  前向服务：尾段分流 开（待服 ≤ %zu 行 → CPU ×%zu，宽批走主设备）\n",
                                 cpu_rows_, cpu_evals_.size());
                    for (auto &ce : cpu_evals_)
                    {
                        NetEvaluator *ev = ce.get(); // ⚠️ 按**指针**值捕获：`ce` 是循环变量
                        servers_.emplace_back([this, ev]
                                              { serve(*ev, /*cpu_role=*/true); });
                    }
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

            /// 入队（**不阻塞**）：worker 塞完就走，去抢别的可推进的树。
            /// `Req` 可以复用（一棵树一个）。
            void push(Req &r)
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
            }

            /// 每批跑完调一次：参数 = 该批各请求**所属的树**（由服务线程回投给 worker 池）。在服务
            /// 线程上调用、且**不持有** `m_`（回调会去抢 worker 池的锁）。必须在 `start()` 之前设好。
            std::function<void(const std::vector<std::size_t> &)> on_batch_;

            /// 批宽上限（一次前向最多吃几行；0 = 不设限 = 全拿走）。⚠️ 设上限只会**变窄**：批宽的分母
            /// 是"在飞的场数"（见 `selfplayPooled` 的池宽），与取批策略无关（实测无差，上限 256 更慢）。
            /// 必须在 `start()` 之前设好。
            std::size_t target_ = 0;

            /// **攒批**：队列非空之后再等，直到"这一轮该来的都来了"或到 `batch_wait_us_` 上限才发车。
            /// `want_rows_` 由调用方给：**当前还有几行会到**（实测 = 在飞的树数 − worker 数，那几条正在
            /// 被推）。⚠️ 为什么不能只用一个固定时长：不等时服务线程是"来一行发一行"，批宽被自锁在
            /// `到达率 × 单次调用耗时`（实测池宽 128 只有 17.2 行），而那 17 行是**分散**到达的 ⇒ 固定
            /// 等 2 ms 只能多攒十几行、白等（实测 600 s → 1038 s）。等"该来的都来"能把批拉到 ≈ 池宽
            /// （锁步）⇒ 每行摊到的固定开销（~200 次算子派发）降下来。0 = 不等。
            int batch_wait_us_ = 0;

            /// "还有几行会到"（诊断/攒批用；不持锁调，别在里面碰 `m_`）。空 = 不知道（只按 `target_` 早发）。
            std::function<std::size_t()> want_rows_;

            /// 诊断：当前队列深度（**不持锁读**，只给诊断用 —— 量级足够，别拿它做判断）。
            std::size_t pendingSize() const { return pending_.size(); }

            /// 前向服务的诊断（一行字）：批数 / 行数 / 最大批 / 三段耗时 + 引擎内部三段 + 尾段 CPU 分流。
            /// 只在 `selfplay()` 收尾打一次（此时服务线程已 join，读数无事）。
            std::string stats() const
            {
                char b[400];
                const std::size_t nb = n_batch_.load(), nr = n_row_.load();
                std::snprintf(b, sizeof(b),
                              "批 %zu / 行 %zu（均 %.1f，最大 %zu）本层 gather %.2fs 调用 %.2fs scatter %.2fs"
                              " | ORT 内部 H2D %.2fs 前向 %.2fs D2H %.2fs"
                              " | 尾段 CPU：批 %zu / 行 %zu",
                              nb, nr,
                              nb ? static_cast<double>(nr) / static_cast<double>(nb) : 0.0,
                              max_batch_.load(), t_gather_.load(), t_call_.load(), t_scatter_.load(),
                              eval_.t_h2d.load(), eval_.t_fwd.load(), eval_.t_d2h.load(),
                              n_cpu_batch_.load(), n_cpu_row_.load());
                return b;
            }

            int pa_dim() const { return p_act_dim_; }
            int ps_dim() const { return p_sel_dim_; }

        private:
            /// 服务循环。`cpu_role` = 这一条线程是**尾段（窄批）**那一侧：
            ///   · `false`（主设备）：只在 `pending_ > cpu_rows_` 时发车（宽批，走 `eval_`）
            ///   · `true`（CPU 会话）：只在 `0 < pending_ <= cpu_rows_` 时发车，**一次只吃 1 行**
            ///     （CPU 成本对行数近似线性 ⇒ 拆成 1 行不亏，还让多个会话并行起来）
            /// ⇒ 对任何 `pending_ > 0` **恰好有一侧够格**（不会互相饿死，也不会两侧都睡）。
            void serve(NetEvaluator &ev, bool cpu_role)
            {
                std::vector<Req *> batch;
                // ⚠️ `owners` **每服务线程一份**：多条服务线程共用一个成员 = 数据竞争。
                std::vector<std::size_t> owners;
                for (;;)
                {
                    {
                        std::unique_lock<std::mutex> lk(m_);
                        const auto eligible = [&]
                        {
                            if (pending_.empty())
                            {
                                return false;
                            }
                            return cpu_role ? (pending_.size() <= cpu_rows_) : (pending_.size() > cpu_rows_);
                        };
                        cv_.wait(lk, [&]
                                 { return stop_ || eligible(); });
                        if (!eligible())
                        {
                            return; // `stop_` 且本角色没有可服务的行
                        }
                        // **攒批**：等到"这一轮该来的都来了"（`want_rows_()`）或到 `batch_wait_us_` 上限。
                        // 见 `batch_wait_us_` 的注释 —— 不等 = 批宽自锁在十几行，固定等 = 白等。
                        // ⚠️ 只给主设备做（CPU 那一侧刻意 1 行/次，攒批没有意义）。
                        // ⚠️ `want_rows_()` 里只有原子读（别在里面碰 `m_`，这里正拿着它）。
                        if (!cpu_role && batch_wait_us_ > 0 &&
                            pending_.size() < (want_rows_ ? want_rows_() : 0))
                        {
                            const auto deadline = std::chrono::steady_clock::now() +
                                                  std::chrono::microseconds(batch_wait_us_);
                            for (;;)
                            {
                                const std::size_t want = want_rows_ ? want_rows_() : 0;
                                if (target_ > 0 && pending_.size() >= target_)
                                {
                                    break; // 攒够上限，提前发车
                                }
                                if (pending_.size() >= want)
                                {
                                    break; // 该来的都来了
                                }
                                if (cv_.wait_until(lk, deadline) == std::cv_status::timeout)
                                {
                                    break; // 兜底：别为了攒批把前向饿死
                                }
                            }
                        }
                        std::sort(pending_.begin(), pending_.end(),
                                  [](Req *a, Req *b)
                                  { return a->seq < b->seq; });
                        // **批宽上限**：一次只取 `target_` 行（按入队序取最老的），**其余留在队列里**。
                        // ⚠️ 别指望靠"留一部分"把批撑宽：**批宽的分母是在飞的场数**（一棵树同时只挂 1 个
                        //    在飞请求）⇒ 设上限只会**变窄**。实测（在飞 84 场）：不设限 84.2 行/批 vs
                        //    上限 256 → 73.6 行/批、墙钟反而 +9%；上限 1024/2016 与不设限无差。
                        //    要加宽只能加宽池子（`selfplayPooled` 的 `pool`）。这里留 `target_` 只为
                        //    受控对照/回归。
                        const std::size_t take = cpu_role ? 1
                                                         : ((target_ > 0 && pending_.size() > target_)
                                                                ? target_
                                                                : pending_.size());
                        batch.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(take));
                        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(take));
                    }
                    // 诊断：批大小分布 + 三段耗时（**多服务线程写 ⇒ 原子**；`stats()` 收尾读）。
                    n_batch_.fetch_add(1, std::memory_order_relaxed);
                    n_row_.fetch_add(batch.size(), std::memory_order_relaxed);
                    if (cpu_role)
                    {
                        n_cpu_batch_.fetch_add(1, std::memory_order_relaxed);
                        n_cpu_row_.fetch_add(batch.size(), std::memory_order_relaxed);
                    }
                    std::size_t prev = max_batch_.load(std::memory_order_relaxed);
                    while (batch.size() > prev &&
                           !max_batch_.compare_exchange_weak(prev, batch.size(),
                                                            std::memory_order_relaxed))
                    {
                    }
                    std::vector<float> buf(batch.size() * static_cast<std::size_t>(kObsLen));
                    const auto d0 = std::chrono::steady_clock::now();
                    for (std::size_t i = 0; i < batch.size(); ++i)
                    {
                        std::copy(batch[i]->obs.begin(), batch[i]->obs.end(),
                                  buf.begin() + static_cast<std::ptrdiff_t>(i * kObsLen));
                    }
                    const auto d1 = std::chrono::steady_clock::now();
                    const auto fwd = ev.eval(buf.data(), static_cast<std::int64_t>(batch.size()), kObsLen);
                    const auto d2 = std::chrono::steady_clock::now();
                    const float *pa = fwd.pa;
                    const float *ps = fwd.ps;
                    const float *val = fwd.val;
                    const auto da = static_cast<std::size_t>(p_act_dim_);
                    const auto ds = static_cast<std::size_t>(p_sel_dim_);
                    for (std::size_t i = 0; i < batch.size(); ++i)
                    {
                        Req &r = *batch[i];
                        r.pa.assign(pa + i * da, pa + (i + 1) * da);
                        r.ps.assign(ps + i * ds, ps + (i + 1) * ds);
                        r.val.assign(val + i, val + i + 1);
                    }
                    const auto d3 = std::chrono::steady_clock::now();
                    t_gather_.fetch_add(std::chrono::duration<double>(d1 - d0).count());
                    t_call_.fetch_add(std::chrono::duration<double>(d2 - d1).count());
                    t_scatter_.fetch_add(std::chrono::duration<double>(d3 - d2).count());
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        for (Req *r : batch)
                        {
                            r->done = true;
                        }
                    }
                    // 把批里各请求**所属的树**一次交回 worker 池（一次 `notify_all`，不是每行一次）。
                    // ⚠️ 必须在放掉 `m_` 之后调：回调会去抢 worker 池的锁（别把两把锁按反序拿）。
                    if (on_batch_)
                    {
                        owners.clear();
                        owners.reserve(batch.size());
                        for (const Req *r : batch)
                        {
                            owners.push_back(r->owner);
                        }
                        on_batch_(owners);
                    }
                }
            }

        private:
            // ⚠️ **声明顺序即初始化顺序**：`cpu_rows_` 的初值要用 `cpu_evals_`（见构造函数）。
            NetEvaluator &eval_;
            std::vector<std::unique_ptr<NetEvaluator>> cpu_evals_; // 尾段（窄批）的会话池（空 = 关）
            std::size_t cpu_rows_ = 0;                             // 走 CPU 的批宽上限（0 = 关）
            int p_act_dim_ = 0, p_sel_dim_ = 0;
            std::vector<std::thread> servers_;
            std::vector<Req *> pending_;
            std::size_t next_seq_ = 0;
            // 诊断计数（**多条服务线程写 ⇒ 原子**；`stats()` 在收尾读 ⇒ 那时服务线程已 join）
            std::atomic<std::size_t> n_batch_{0}, n_row_{0}, max_batch_{0};
            std::atomic<std::size_t> n_cpu_batch_{0}, n_cpu_row_{0}; // 尾段那一侧（窄批）
            std::atomic<double> t_gather_{0.0}, t_call_{0.0}, t_scatter_{0.0};
            bool stop_ = false;
            std::mutex m_;
            std::condition_variable cv_;
        };

    } // namespace

    void selfplayPooled(std::size_t n_task, std::size_t pool,
                        const std::function<TaskIn(std::size_t)> &make_task, const SelfplayConfig &cfg,
                        const std::function<void(std::size_t, GameOut &&)> &emit)
    {
        if (n_task == 0)
        {
            return;
        }
        if (cfg.sims < 2)
        {
            // 与 Python 的 `sims ≥ 2` 契约同：1 次模拟只展开根、不给任何根边加访问 ⇒ π 全零。
            throw std::runtime_error("sims 太小：至少 2（否则本场搜不出可记的 π）");
        }
        if (cfg.sims_cheap > 0 && cfg.sims_cheap < 2)
        {
            // 廉价档同样要 ≥ 2：否则那一档的 π 全零（`recordFrame` 会当场抛"定不出 π"）。
            throw std::runtime_error("sims_cheap 太小：至少 2（与 sims 同一条契约）");
        }
        if (cfg.sims_cheap > cfg.sims)
        {
            throw std::runtime_error("sims_cheap 大于 sims：两档反了（`sims` 才是完整预算）");
        }
        NetEvaluator eval(cfg.model_path, cfg.device, cfg.threads);
        if (eval.p_act_dim == 0)
        {
            // 先要一次头宽度（1 行假输入）：维度在第一次前向后才有定义；也让 CUDA 上下文与执行计划
            // 在**本线程**上建好（`NetEvaluator` 必须在同一线程首次使用）。
            std::vector<float> probe(static_cast<std::size_t>(kObsLen), 0.0f);
            eval.eval(probe.data(), 1, kObsLen);
        }
        {
            const TaskIn first = make_task(0);
            terminalValue(first.battle, cfg.z_spec); // 拼错的 spec 要在这里响亮失败
        }

        // 池宽：在飞的场数。**任务表从"整块一次性全开"改成滑窗**是这里唯一的形状改动 ——
        // 一棵树同时只能有 1 个在飞前向请求 ⇒ 批宽 ≤ 在飞的场数；不补场时短局跑完就走，
        // 后面只剩几十个长局 ⇒ 批宽锁死在几十行、每次前向那 ~200 次算子派发的固定开销摊不薄。
        const std::size_t S = std::min(std::max<std::size_t>(1, pool), n_task); // 树槽数 = 在飞的场数
        // **交付槽与树槽分开**（`D` = 交付环长，`S` = 树槽数）：一场**跑完就腾出树槽**接着开下一场，
        // 结果先按任务号落进交付槽（队头一到才交付给调用方）。
        // ⚠️ 曾经把两件事合成一个环（槽 = 任务号 mod 池宽）：跑完但没交付的场会一直占着树槽 ⇒ 交付被
        //    队头卡住时**在飞的场数塌掉**（实测池宽 128 只剩 ~49 在跑、均批从 ~84 掉到 **17.2**、
        //    每行 112 µs ⇒ 比"整表全开"还慢 2.5×）。`D - S` 就是允许"跑完待交付"的落后量。
        // ⚠️ **落后量必须留够**：交付是**按任务序**的，队头撞上一个长局（MAX_STEPS 1000 ⇒ 一步一行的
        //    话能占住十几秒）就会把落后的场堆到几百 ⇒ 一旦顶到 `D`，`claim()` 就不再开新场 ⇒ 池子
        //    塌掉（实测 `D = 2S` 时在飞只有 49）。每条落后结果只是一场的帧（≈ 11 帧 × 6.4 KB / 场），
        //    `D = 8S` 的内存代价可忽略。
        const std::size_t D = std::min(n_task, S * 8);
        // 环槽上的树（换任务时**就地重建**：`BattleContext` 只需要拷贝构造 —— 别用拷贝赋值，
        // 那会踩到"旧值里的自指/缓存指针"这类坑）。
        std::vector<std::optional<Tree>> trees(S);
        std::vector<std::size_t> task_of(S, 0); // 树槽 → 任务号（交付槽按它定位）

        std::uniform_real_distribution<double> u01(0.0, 1.0);
        const bool exploring = cfg.sample || cfg.eps > 0.0 || cfg.temp_moves > 0 || cfg.dirichlet_eps > 0.0;

        // ⚠️ 顺序：`NetEvaluator` 的**首次前向在主线程**跑（下面这次 probe 就是它）⇒ 头宽度、
        //    CUDA 上下文与执行计划都建立好之后，前向服务才允许起线程（否则并跑 = 访问违例）。
        {
            std::vector<float> probe(static_cast<std::size_t>(kObsLen), 0.0f);
            eval.eval(probe.data(), 1, kObsLen);
        }
        // 尾段（窄批）用的 CPU 会话池：**每个服务线程一个独立会话**（内部缓冲不共享 ⇒ 多线程安全）。
        // 与主会话同契约：各自先在主线程探一次（头宽度/执行计划那时才建立）。
        // ⚠️ 只在"主设备不是 CPU 且要分流"时建：`model_path_cpu` 空 ⇒ 分流关（守卫就是这一档）。
        std::vector<std::unique_ptr<NetEvaluator>> cpu_evals;
        if (!cfg.model_path_cpu.empty() && cfg.cpu_rows > 0 && cfg.cpu_sessions > 0)
        {
            std::vector<float> probe(static_cast<std::size_t>(kObsLen), 0.0f);
            for (int i = 0; i < cfg.cpu_sessions; ++i)
            {
                cpu_evals.push_back(std::make_unique<NetEvaluator>(cfg.model_path_cpu, "cpu", 1));
                cpu_evals.back()->eval(probe.data(), 1, kObsLen);
            }
        }
        NNEval nneval(eval, std::move(cpu_evals), cfg.cpu_rows);

        // 结果槽 + 写者：主线程按**交付序**（= 场序）交付，只取队头 ⇒ 每场一跑完就把行交出去。
        std::vector<std::optional<GameOut>> slots(D);
        std::mutex m;
        std::condition_variable cv_done; // 写者等"队头就绪"
        std::size_t started = 0;         // 已开局的任务数（≤ n_task）——以下三个计数都在 `m` 下改
        std::size_t emitted = 0;         // 已交付的场数（= 队头）
        std::size_t retired = 0;         // 已跑完的场数（≥ `emitted`，落后量 < D）
        std::exception_ptr err;          // 线程里抛异常会 terminate ⇒ 录下、主线程重抛

        // ── worker 池：**固定 W 条线程 + S 棵在飞的树**，树**不绑定线程** ────────────────────
        // 每棵树是一台可挂起的状态机（"协程"的朴素做法：状态 = 手上的 `Req` 有没有结果）：
        //   · worker 从 **ready 队列**取一棵 → 推进**一步**（降 1 条路径 → `push` 观测）；
        //   · `push` 完**不占线程**，立刻回去抢下一棵；被推的那棵树进入"在飞"；
        //   · 前向服务跑完一批，把这批各请求**所属的树**一次回投 ready 队列 ⇒ 谁抢到谁继续
        //     （先把结果 `expandBackup` 进去，再往下推一步）。
        // ⇒ 线程数（W ≈ 核数）与批宽（在飞树数 S）彻底解耦，且**没有线程会阻塞在某一棵树上**：
        //    worker 只在 ready 队列空时才睡（那时确实没活干）。
        // ⚠️ 别退回"一棵树一条线程"：那时每个批完成要 `notify_all` 叫醒全部 S 条线程抢同一把锁
        //    —— 实测"一树一线"K=2016 时 CPU 15.7 核（16 核 cgroup）而 GPU 只有 4~11%，几乎全是惊群开销。
        // ⚠️ 也别退回"把树静态分给线程"（每线程持 k 棵、攒够 k 行再一起等）：那让 worker 卡在自己
        //    那 k 棵上（尾块只能干等），而这里的 ready 队列是**抢占式**的，长短局自然均衡。
        const std::size_t W = std::min<std::size_t>(
            cfg.workers > 0 ? static_cast<std::size_t>(cfg.workers)
                            : static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
            S);
        std::vector<NNEval::Req> reqs(S); // 一棵树一个（复用 ⇒ 每步不再为观测/结果重新分配）

        // 每棵树的挂起状态：`left` = 本决策点还剩几轮；`waiting` = 手上有在飞的前向；
        // `live` = 这个环槽上有一场在跑（死树不允许再被推进/回投）。
        struct TreeState
        {
            int left = 0;
            bool waiting = false;
            bool live = false;
        };
        std::vector<TreeState> st(S);
        std::vector<std::size_t> ready; // 可推进的树（谁抢到谁推）；用栈序，无优先级
        ready.reserve(S);
        std::condition_variable cv_ready; // worker 等"有可推进的树"
        bool stop = false;
        // worker 侧诊断（多线程写 ⇒ 原子；只求量级）
        std::atomic<std::size_t> wait_n{0}, ready_hi{0};
        std::atomic<double> wait_t{0.0};
        // 树的分布诊断（每次 `push` 采样一次）：能立刻看出"在飞/可推/待服"三段各占多少 —— 批宽上不去
        // 时，先看这一段（在飞少 = 池子没料；待服少 = 服线程捞太快）。
        std::atomic<std::size_t> n_push{0}, sum_ready{0}, sum_live{0};
        std::atomic<unsigned long long> sum_pending{0};
        std::atomic<std::size_t> live_n{0};  // 当前活着的场数（`start_task` +1 / `retire` -1）
        std::atomic<std::size_t> live_hi{0}; // 活着的场数的峰值（诊断：顶上池宽才对）
        std::size_t lag_hi = 0;              // 交付落后的峰值（诊断：顶到 `D` 就会停开新场）
        const auto t_start = std::chrono::steady_clock::now();
        const std::size_t mark = std::max<std::size_t>(1, n_task / 20);

        // 领号：锁内决定"还能不能再开一场"（任务表没完 + 落后量没超 ⇒ 定下树槽要装哪个任务）。
        // 返回 `n_task` = 不再开。
        auto claim = [&]() -> std::size_t
        {
            std::lock_guard<std::mutex> lk(m);
            if (err || stop || started >= n_task || started - emitted >= D)
            {
                return n_task;
            }
            return started++;
        };

        // 开局：在树槽 `idx` 上开任务 `j` 并让它可被推进。`make_task`（造局面）**不持锁**。
        auto start_task = [&](std::size_t idx, std::size_t j)
        {
            TaskIn t = make_task(j);
            trees[idx].emplace(j, std::move(t.battle), t.seed); // 就地重建（旧树先析构）
            // 本场的**第一个**决策点的预算也走随机化那条路（与后续决策点同分布）。
            const int first_sims = nextSims(*trees[idx], cfg);
            {
                std::lock_guard<std::mutex> lk(m);
                task_of[idx] = j;
                st[idx] = TreeState{first_sims, false, true};
                ++live_n;
                if (!err && !stop)
                {
                    ready.push_back(idx);
                }
            }
            cv_ready.notify_all();
        };

        // 交付：跑完就把结果按**任务号**搬进交付槽（队头可能还轮不到它 —— 那是写者的事），
        // 并**立刻在这个树槽上开下一场**（不等交付！否则"跑完但没交付"的场占着树槽 ⇒ 池子塌掉）。
        // ⚠️ **必须叫醒写者**：写者等的是 `cv_done`（队头），漏掉这句就是"槽填好了没人取" —— 直接死锁
        //    （实测：K=1 那一跑搜完就挂住，进程 0.3 s CPU 再也不动）。
        auto retire = [&](std::size_t i)
        {
            std::size_t next = n_task;
            {
                std::lock_guard<std::mutex> lk(m);
                const std::size_t j = task_of[i];
                if (slots[j % D].has_value())
                {
                    // 不该发生：落后量计数错了（或交付槽被覆盖）。响亮失败，别静默丢数据。
                    throw std::runtime_error("交付槽被覆盖（池宽/落后量计数错了）");
                }
                slots[j % D] = std::move((*trees[i]).out);
                st[i].live = false;
                trees[i].reset(); // 当场把这一场的内存放掉
                ++retired;
                --live_n;
                if (retired - emitted > lag_hi)
                {
                    lag_hi = retired - emitted; // 交付落后最多到几场（诊断：它顶到 `D` 就会停开新场）
                }
                if (live_n.load() > live_hi)
                {
                    live_hi = live_n.load();
                }
            }
            // 进度按**跑完的场数**报，不按交付序（交付被"队头那场"卡住时，这一行仍能看出在动）。
            if (retired % mark == 0)
            {
                const double sec = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - t_start)
                                       .count();
                std::fprintf(stderr, "  自对弈（完成）%zu/%zu 场（%.0fs）\n", retired, n_task, sec);
            }
            cv_done.notify_all();
            next = claim(); // 腾出来的树槽马上装下一场
            if (next < n_task)
            {
                start_task(i, next);
            }
        };
        // 前向服务跑完一批后的回投（在服务线程上跑）。
        nneval.on_batch_ = [&](const std::vector<std::size_t> &owners)
        {
            {
                std::lock_guard<std::mutex> lk(m);
                ready.insert(ready.end(), owners.begin(), owners.end());
            }
            cv_ready.notify_all();
        };
        // 批宽上限（`cfg.batch_target`；0 = 不设限）+ 攒批（`cfg.batch_wait_us`，0 = 不等）。
        nneval.target_ = static_cast<std::size_t>(std::max(0, cfg.batch_target));
        nneval.batch_wait_us_ = std::max(0, cfg.batch_wait_us);
        // 攒批目标 = "还有几行会到" = 在飞的树 − worker 数（那几条正被 worker 推，马上就进队列）。
        nneval.want_rows_ = [&]() -> std::size_t
        {
            const std::size_t live = live_n.load();
            return live > W ? live - W : 0;
        };
        // ⚠️ 起服务线程必须在 `on_batch_` 设好之后：服务线程只在批完成时读它，早起 = 数据竞争。
        nneval.start(/*n_server=*/1);
        enum class Step
        {
            Ready,    // 还能立刻再推一步（终局叶 / 刚出手）—— 同一条线程接着推，省队列往返
            InFlight, // 已提交前向：服务线程跑完会把它回投 ready
            Done      // 已交付
        };
        // 推进**一步**：先消费回来的前向（若有）→ 本决策点搜完则记录+出手 → 降 1 条路径并提交前向。
        auto step = [&](std::size_t i) -> Step
        {
            Tree &t = *trees[i];
            TreeState &s = st[i];
            ++n_push;
            sum_ready += ready.size(); // ⚠️ 不持锁读（诊断用量级）
            sum_live += live_n.load();
            sum_pending += nneval.pendingSize();
            if (s.waiting)
            {
                s.waiting = false;
                const NNEval::Req &r = reqs[i];
                const bool is_sel = t.scratch.inputState == InputState::CARD_SELECT;
                const float *logits = (is_sel ? r.ps : r.pa).data();
                expandBackup(t, cfg, logits, static_cast<double>(r.val[0]));
            }
            if (t.finished)
            {
                retire(i);
                return Step::Done;
            }
            if (s.left <= 0) // 本决策点搜完：记录 → 选动作 → 推进（不需要前向）
            {
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
                s.left = nextSims(t, cfg); // 下一个决策点的预算（随机化上限；关着 = 恒 `cfg.sims`）
                if (t.finished)
                {
                    retire(i);
                    return Step::Done;
                }
            }
            --s.left; // 终局叶也消耗一轮（与原语义同）
            if (descendOne(t, cfg))
            {
                return Step::Ready; // 该叶是终局：已就地结算，不需要前向
            }
            t.replay();
            NNEval::Req &r = reqs[i];
            r.owner = i;
            r.obs = snapshotObs(t.scratch);
            s.waiting = true;
            nneval.push(r); // **不阻塞**：塞完就走，去抢下一棵可推进的树
            return Step::InFlight;
        };

        // DEBUG-BISECT: 起头放在起线程之后（旧行为）
        // 起头：把池子填满（任务 0..S-1 → 环槽 0..S-1）。**在起 worker 之前**填：省得 W 条线程先睡
        // 一觉再被叫醒；`ready` 反序入栈 ⇒ 出栈次序 0,1,2…（与"整表全开"的老行为同，别白白改 K>1
        // 那些数的批组成）。
        for (std::size_t i = 0; i < S; ++i)
        {
            const std::size_t j = claim();
            if (j >= n_task)
            {
                break;
            }
            start_task(i, j);
        }
        std::reverse(ready.begin(), ready.end());

        std::vector<std::thread> ths;
        ths.reserve(W);
        for (std::size_t w = 0; w < W; ++w)
        {
            ths.emplace_back(
                [&]
                {
                    try
                    {
                        for (;;)
                        {
                            std::size_t i = 0;
                            {
                                std::unique_lock<std::mutex> lk(m);
                                // ⚠️ `err` / `stop` 必须在谓词里：否则出错或全部交付后再也无人回投
                                //    ⇒ 永久睡下去（`join` 死锁）。
                                const auto w0 = std::chrono::steady_clock::now();
                                cv_ready.wait(lk, [&]
                                              { return !ready.empty() || err || stop; });
                                wait_t += std::chrono::duration<double>(
                                              std::chrono::steady_clock::now() - w0)
                                              .count();
                                ++wait_n;
                                if (ready.size() > ready_hi)
                                {
                                    ready_hi = ready.size();
                                }
                                if (ready.empty())
                                {
                                    return; // 出错或跑完
                                }
                                i = ready.back();
                                ready.pop_back();
                            }
                            // 同一条线程一次把这棵树推到"要么在飞、要么已交付"，省掉队列往返。
                            while (step(i) == Step::Ready)
                            {
                            }
                        }
                    }
                    catch (...)
                    {
                        {
                            std::lock_guard<std::mutex> lk(m);
                            if (!err)
                            {
                                err = std::current_exception();
                            }
                            stop = true;
                        }
                        cv_ready.notify_all();
                        cv_done.notify_all();
                    }
                });
        }
        // 主线程 = **唯一的交付者**：只处理队头（`emitted`），拿到当场 `emit` 出去、槽位复位
        // （在飞的场数不靠它补 —— 补场在 `retire` 里做，避免"交付慢了池子就塌"）。
        for (;;)
        {
            GameOut g;
            std::size_t idx = 0;
            {
                std::unique_lock<std::mutex> lk(m);
                while (!(err || emitted >= n_task || slots[emitted % D].has_value()))
                {
                    cv_done.wait(lk);
                }
                if (err || emitted >= n_task)
                {
                    break;
                }
                idx = emitted % D;
                g = std::move(*slots[idx]);
                slots[idx].reset();
                ++emitted;
            }
            emit(emitted - 1, std::move(g));
        }
        {
            std::lock_guard<std::mutex> lk(m);
            stop = true; // 任务表空了 ⇒ 剩余树跑完后 worker 自然退场（`ready` 空即 return）
        }
        cv_ready.notify_all();
        for (auto &th : ths)
        {
            th.join();
        }
        // 诊断（每块一行）：批大小分布 + gather/调用/scatter 三段 + ORT 内部三段 + worker 空转 + 树分布
        // —— 用来判断"GPU/CPU 都喂不满"卡在哪一段。
        std::fprintf(stderr, "  前向服务：%s | worker 等活 %zu 次 %.2fs（队列最深 %zu）\n",
                     nneval.stats().c_str(), wait_n.load(), wait_t.load(), ready_hi.load());
        if (n_push.load() > 0)
        {
            const auto np = static_cast<double>(n_push.load());
            std::fprintf(stderr,
                         "  树分布（每次 push 采样）：均 在飞 %.1f / 可推 %.1f / 待服 %.1f"
                         "｜池宽 %zu 峰值在飞 %zu｜交付落后峰值 %zu（上限 %zu）\n",
                         static_cast<double>(sum_live.load()) / np - static_cast<double>(sum_ready.load()) / np,
                         static_cast<double>(sum_ready.load()) / np,
                         static_cast<double>(sum_pending.load()) / np, S, live_hi.load(), lag_hi, D);
        }
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    void selfplay(const std::vector<BattleContext> &battles, const std::vector<std::uint64_t> &seeds,
                  const SelfplayConfig &cfg, const std::function<void(std::size_t, GameOut &&)> &emit)
    {
        if (battles.size() != seeds.size())
        {
            throw std::runtime_error("局数与 seeds 数量不一致");
        }
        const std::size_t K = battles.size();
        // 池宽 = 任务数 ⇒ 整表**一次全开、不补场**（K=1 时批恒 1 行 ⇒ 与 golden 逐位可对拍）。
        selfplayPooled(
            K, K, [&](std::size_t j)
            { return TaskIn{battles[j], seeds[j]}; }, cfg, emit);
    }

    // ── 整段采集：任务表 → 造局面 → 搜索 → 落盘（与 `teacherRun` 同形）────────────────
    std::vector<std::string> selfplayRun(int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed,
                                         const std::string &out_dir, int shard_rows)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        ShardWriter w;
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
        // **不再按 `cfg.pool` 切块**：整张任务表交给**一个滑窗池**（池宽 = `cfg.pool`，跑完一场立刻
        // 补一场）。切块会让每个块里的"在飞场数"从满衰减到空 —— 那正是批宽摊不薄的根因
        // （见 `selfplayPooled` 的 ⚠️）。
        const std::vector<EncJob> jobs = encJobs(n_per, start_seed);
        // 进度：每 5% 打一行（与 `teacher.cpp` 的 `教师 x/y 场` 同形）—— 一轮采集动辄一两小时，
        // 不打这一行的话外面（日志）**完全看不出在动**，而落盘要等 `shard_rows` 攒够才出第一个文件。
        const std::size_t mark = std::max<std::size_t>(1, jobs.size() / 20);
        std::size_t done = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto t_line = t0; // 上一行"交付"进度的时间（⚠️ 也**按时间**打：交付是**按任务序**的，
                          //   队头撞上一个长局时它可能几千秒不动，只按场数打会看起来像 hang）
        selfplayPooled(
            jobs.size(), static_cast<std::size_t>(std::max(1, cfg.pool)),
            [&](std::size_t j) -> TaskIn
            {
                const EncJob &e = jobs[j];
                // 造局面的**唯一入口**（与 Python 的 `scene.stage_battle` 同一份实现）。
                return TaskIn{buildStaged(e.seed, e.enc, e.floor, ascensionOf(e.seed)).b, e.seed};
            },
            cfg,
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
                if (++done % mark == 0 || std::chrono::steady_clock::now() - t_line > std::chrono::seconds(60))
                {
                    t_line = std::chrono::steady_clock::now();
                    const double sec = std::chrono::duration<double>(t_line - t0).count();
                    std::fprintf(stderr, "  自对弈 交付 %zu/%zu 场（%.0fs）\n", done, jobs.size(), sec);
                }
            });
        const std::string last = w.flush();
        if (!last.empty())
        {
            written.push_back(last);
        }
        return written;
    }

} // namespace sts::collect
