#include "eval.h"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <utility>

#include "action.h"     // enumerateActions（random 策略）
#include "encs.h"       // encJobs（任务表：每个遭遇 × 它那个代表层）
#include "scenebuild.h" // buildStaged / ascensionOf（造局面的唯一入口）
#include "teacher.h"    // TeacherConfig / teacherPlay（teacher 策略）

#include <pybind11/stl.h>

namespace sts::collect
{

    namespace
    {
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
