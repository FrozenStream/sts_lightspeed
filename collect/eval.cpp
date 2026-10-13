#include "eval.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>

#include "action.h"     // enumerateActions / expandChildren（策略执行要枚举合法动作）
#include "encs.h"       // encJobs（任务表：每个遭遇 × 它那个代表层）
#include "scenebuild.h" // buildStaged / ascensionOf（造局面的唯一入口）
#include "snapshot.h"   // snapshotObs / Snap（观测宽度）
#include "teacher.h"    // TeacherConfig / teacherPlay（teacher 策略）
#include "torch_eval.h" // TorchEvaluator（网络策略）

#include <pybind11/stl.h>

// ⚠️ `TorchEvaluator` 仍住在 `sts::py`（`bindings/torch_eval.h`）—— 与 `selfplay.cpp` 同。
using sts::py::TorchEvaluator;

namespace sts::collect
{

    namespace
    {
        // 观测宽度（编译期定死）—— 与 `selfplay.cpp` 那份同源（`Snap::snapshotLen`）。
        constexpr int kObsLen = Snap::snapshotLen;

        /// "random" 策略打完**一场**（`start` 是造好的局面）。
        Outcome playRandom(const BattleContext &start, std::uint64_t seed, int max_steps)
        {
            BattleContext b = start;
            std::mt19937_64 rng(seed * 1000003ULL + 29ULL);
            for (int step = 0; step < max_steps && b.outcome == Outcome::UNDECIDED; ++step)
            {
                const std::vector<search::Action> legal = enumerateActions(b);
                if (legal.empty())
                {
                    break; // 没得打（理论上不该发生）⇒ 记未决
                }
                std::uniform_int_distribution<std::size_t> ui(0, legal.size() - 1);
                search::Action pick = legal[ui(rng)];
                pick.execute(b);
            }
            return b.outcome;
        }

        /// "net" 策略但是**不搜索**（"裸网络"）：逐决策点直接取网络的先验 argmax。
        ///
        /// 为什么单独一条路：`selfplay` 的契约是"搜得出一份可记的 π"（`sims ≥ 2`），而门控要的
        /// 恰恰是**没有搜索遮挡**的那把尺子 ⇒ 直接前向 + 取候选里 logits 最大的动作。
        /// ⚠️ 门控每轮要打几万场，单场一场前向（batch=1）光调用开销就够呛 ⇒ **K 局并肩推进**：
        ///    每轮把"还没打完的那些局"的观测拼成一批，一次前向，各自走一步。
        void playNetBatch(TorchEvaluator &ev, const std::vector<BattleContext> &starts,
                          std::vector<Outcome> &out, int max_steps)
        {
            const std::size_t K = starts.size();
            std::vector<BattleContext> bs = starts; // 本函数独占（调用方给的是拷贝）
            std::vector<int> steps(K, 0);
            std::vector<float> buf;
            for (;;)
            {
                std::vector<std::size_t> live;
                buf.clear();
                for (std::size_t j = 0; j < K; ++j)
                {
                    if (bs[j].outcome != Outcome::UNDECIDED || steps[j] >= max_steps)
                    {
                        continue;
                    }
                    const auto o = snapshotObs(bs[j]);
                    buf.insert(buf.end(), o.begin(), o.end());
                    live.push_back(j);
                }
                if (live.empty())
                {
                    break; // 全打完了（或全到步数上限）
                }
                const auto fwd = ev.eval(buf.data(), static_cast<std::int64_t>(live.size()), kObsLen);
                const float *pa = fwd[0].data_ptr<float>();
                const float *ps = fwd[1].data_ptr<float>();
                const auto da = static_cast<std::size_t>(ev.p_act_dim);
                const auto ds = static_cast<std::size_t>(ev.p_sel_dim);
                for (std::size_t i = 0; i < live.size(); ++i)
                {
                    BattleContext &b = bs[live[i]];
                    const bool is_sel = b.inputState == InputState::CARD_SELECT;
                    const float *logits = (is_sel ? ps + i * ds : pa + i * da);
                    // 候选 = 合法动作（`expandChildren` 与搜索/采集同一份枚举）⇒ 只在候选里取最大。
                    // softmax 对 logits 单调 ⇒ 取最大 logits 就等于取先验 argmax，不必真算分布。
                    auto ex = expandChildren(b);
                    if (ex.first.empty())
                    {
                        out[live[i]] = b.outcome;
                        ++steps[live[i]];
                        continue;
                    }
                    std::size_t best = 0;
                    for (std::size_t c = 1; c < ex.first.size(); ++c)
                    {
                        if (logits[ex.first[c].first] > logits[ex.first[best].first])
                        {
                            best = c;
                        }
                    }
                    ex.first[best].second.execute(b);
                    ++steps[live[i]];
                }
            }
            for (std::size_t j = 0; j < K; ++j)
            {
                out[j] = bs[j].outcome;
            }
        }

        /// "net" 策略的配置（探索全关：门控比的是"当前策略强度"，不是"探索后的强度"）。
        SelfplayConfig netCfg(const EvalSpec &spec, int max_steps)
        {
            SelfplayConfig cfg;
            cfg.ts_path = spec.ts_path;
            cfg.device = spec.device;
            cfg.z_spec = spec.z_spec;
            cfg.sims = spec.sims;
            cfg.max_steps = max_steps;
            cfg.threads = 1;
            cfg.sample = false;
            cfg.eps = 0.0;
            cfg.temp_moves = 0;
            cfg.dirichlet_total = 0.0;
            cfg.dirichlet_eps = 0.0;
            return cfg;
        }

        void tally(Outcome o, EvalSpecStats &st)
        {
            ++st.games;
            if (o == Outcome::PLAYER_VICTORY)
            {
                ++st.wins;
            }
            else if (o == Outcome::UNDECIDED)
            {
                ++st.undecided;
            }
        }
    } // namespace

    std::vector<EvalSpecStats> evalRun(const std::vector<EvalSpec> &specs, int n_per,
                                       std::uint64_t start_seed, int games, int max_steps)
    {
        if (specs.empty())
        {
            throw std::runtime_error("eval：至少要一个策略");
        }
        if (n_per < 1)
        {
            throw std::runtime_error("eval：n_per 至少 1");
        }

        // 任务流 = 采集那张表（`encs.h`）：每个遭遇 × 它那个代表层 × `n_per` 个 seed。
        // **所有策略跑同一份 `jobs`** ⇒ 同一 `(遭遇, 层, seed)` 的局面逐位相同、胜率可直接相减。
        // ⚠️ 局面**逐策略各造一次**（`buildStaged` 是 seed/enc/floor/ascension 的纯函数 ⇒ 每次都
        //    逐位一样）：缓着不造只省一点点构造成本，却要让所有策略共享可变的 `BattleContext`。
        const std::vector<EncJob> jobs = encJobs(n_per, start_seed);
        const std::size_t width = static_cast<std::size_t>(std::max(1, games));

        std::vector<EvalSpecStats> out(specs.size());
        for (std::size_t si = 0; si < specs.size(); ++si)
        {
            const EvalSpec &spec = specs[si];
            EvalSpecStats &st = out[si];
            const bool is_net = spec.kind == "net";
            // 裸网络（`sims == 0`）：不起搜索，也不走 `selfplay`（那条路要求 `sims ≥ 2`）。
            // 前向器**每个 spec 建一次**（加载 TorchScript 不便宜，别按块重建）。
            const bool bare_net = is_net && spec.sims < 2;
            std::unique_ptr<TorchEvaluator> bare_eval;
            if (bare_net)
            {
                bare_eval = std::make_unique<TorchEvaluator>(spec.ts_path, spec.device, 1);
                std::vector<float> probe(static_cast<std::size_t>(kObsLen), 0.0f);
                bare_eval->eval(probe.data(), 1, kObsLen); // 头宽度 / 执行计划在首次前向后才有
            }
            for (std::size_t at = 0; at < jobs.size(); at += width)
            {
                const std::size_t n = std::min(width, jobs.size() - at);
                std::vector<BattleContext> battles;
                std::vector<std::uint64_t> seeds;
                battles.reserve(n);
                seeds.reserve(n);
                for (std::size_t k = 0; k < n; ++k)
                {
                    const EncJob &j = jobs[at + k];
                    battles.push_back(buildStaged(j.seed, j.enc, j.floor, ascensionOf(j.seed)).b);
                    seeds.push_back(j.seed);
                }
                if (bare_net)
                {
                    std::vector<Outcome> got(n);
                    playNetBatch(*bare_eval, battles, got, max_steps);
                    for (Outcome o : got)
                    {
                        tally(o, st);
                    }
                    continue;
                }
                if (is_net)
                {
                    // 一次前向吃 `width` 棵树（与采集同形）⇒ 别一棵一棵调，那会把批宽压成 1。
                    // 结果按场序回调 ⇒ 只收终局判定（评估不需要那些行）。
                    std::vector<Outcome> got(n);
                    SelfplayConfig cfg = netCfg(spec, max_steps);
                    cfg.games = static_cast<int>(n);
                    selfplay(battles, seeds, cfg,
                             [&](std::size_t j, GameOut &&g)
                             { got[j] = g.outcome; });
                    for (Outcome o : got)
                    {
                        tally(o, st);
                    }
                    continue;
                }
                for (std::size_t k = 0; k < n; ++k)
                {
                    const EncJob &j = jobs[at + k];
                    if (spec.kind == "random")
                    {
                        tally(playRandom(battles[k], j.seed, max_steps), st);
                    }
                    else if (spec.kind == "teacher")
                    {
                        TeacherConfig cfg;
                        cfg.sims = spec.sims;
                        cfg.max_steps = max_steps;
                        tally(teacherPlay(battles[k], cfg, j.seed), st);
                    }
                    else
                    {
                        throw std::runtime_error("eval：不认识的策略 kind=" + spec.kind);
                    }
                }
            }
        }
        return out;
    }

    void registerEval(pybind11::module_ &m)
    {
        namespace py = pybind11;

        py::class_<EvalSpec>(m, "EvalSpec")
            .def(py::init<>())
            .def_readwrite("kind", &EvalSpec::kind)
            .def_readwrite("label", &EvalSpec::label)
            .def_readwrite("ts_path", &EvalSpec::ts_path)
            .def_readwrite("device", &EvalSpec::device)
            .def_readwrite("z_spec", &EvalSpec::z_spec)
            .def_readwrite("sims", &EvalSpec::sims);

        py::class_<EvalSpecStats>(m, "EvalSpecStats")
            .def_readonly("games", &EvalSpecStats::games)
            .def_readonly("wins", &EvalSpecStats::wins)
            .def_readonly("undecided", &EvalSpecStats::undecided)
            .def_property_readonly(
                "win_rate",
                [](const EvalSpecStats &s)
                { return s.games > 0 ? static_cast<double>(s.wins) / s.games : 0.0; });

        m.def(
            "eval_run",
            [](const std::vector<EvalSpec> &specs, int n_per, std::uint64_t start_seed,
                int games, int max_steps)
            {
                std::vector<EvalSpecStats> stats;
                {
                    // 整段不碰 Python ⇒ 放 GIL（与 `sts.teacher_run` / `sts.selfplay_run` 同）。
                    py::gil_scoped_release release;
                    stats = sts::collect::evalRun(specs, n_per, start_seed, games, max_steps);
                }
                return stats;
            },
            py::arg("specs"), py::arg("n_per"), py::arg("start_seed"), py::arg("games") = 8,
            py::arg("max_steps") = 1000,
            "**配对评估**（任务表 + 造局面 + 搜索 + 统计全在 C++）：同一批 `(遭遇, 层, seed)`\n"
            "喂给所有策略，返回与 `specs` 同序的统计（`games` / `wins` / `undecided` / `win_rate`）。");
    }

} // namespace sts::collect
