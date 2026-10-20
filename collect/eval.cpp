#include "eval.h"

#include "bind.h" // `registerEval` 的声明（定义要签名对得上）

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

#include "action.h"                 // enumerateActions / expandChildren（策略执行要枚举合法动作）
#include "encs.h"                   // encJobs（任务表：每个遭遇 × 它那个代表层）
#include "scenebuild.h"             // buildBattle / ascensionOf（造局面的唯一入口）
#include "snapshot.h"               // snapshotObs / Snap（观测宽度）
#include "teacher.h"                // TeacherConfig / teacherPlay（teacher 策略）
#include "sim/search/SimpleAgent.h" // SimpleAgent（原版手写优先级；对照用）
#include "net_eval.h"               // NetEvaluator（网络策略；唯一引擎 = ONNX Runtime）

#include <pybind11/stl.h>

#include "netprobe.h" // probeForwardChecked（裸网络会话的建立校验）

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
                std::uniform_int_distribution<std::size_t> ui(0, legal.size() - 1);
                search::Action pick = legal[ui(rng)];
                pick.execute(b);
            }
            return b.outcome;
        }

        void playNetOnly(NetEvaluator &ev, const std::vector<BattleContext> &starts,
                          std::vector<Outcome> &out, int max_steps,
                          std::vector<Term> *terms = nullptr)
        {
            const std::size_t K = starts.size();
            std::vector<BattleContext> bs = starts; // 本函数独占（调用方给的是拷贝）
            std::vector<int> steps(K, 0);
            std::vector<float> buf;
            while (true)
            {
                std::vector<std::size_t> live;
                buf.clear();
                for (std::size_t j = 0; j < K; ++j)
                {
                    if (bs[j].outcome != Outcome::UNDECIDED || steps[j] >= max_steps)
                    {
                        continue;
                    }
                    const auto o = sts::collect::snapshotOf(bs[j]);
                    buf.insert(buf.end(), o.begin(), o.end());
                    live.push_back(j);
                }
                if (live.empty())
                {
                    break; // 全打完了（或全到步数上限）
                }
                const auto fwd = ev.eval(buf.data(), static_cast<std::int64_t>(live.size()), Snap::snapshotLen);
                const float *pa = fwd.pa;
                const float *ps = fwd.ps;
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
                if (terms)
                {
                    (*terms)[j] = readTerm(bs[j]);
                }
            }
        }

        /// 裸网评估的线程数：`workers` 在 MCTS 下是树 worker 数，在裸网下复用为
        /// “独立 Session 数”。0/1 = 串行；每段一个独立 NetEvaluator（Session 非线程安全）。
        int bareThreadCount(const EvalSpec &spec, std::size_t games)
        {
            const int requested = spec.workers > 0 ? spec.workers : 1;
            return std::max(1, std::min<int>(requested, static_cast<int>(games)));
        }

        /// 把独立的局切成 T 段，每段一个线程 + 一个独立 Session，各自 lockstep 推进。
        /// 结果按原局序回填；线程内异常汇到 first_error，join 后重抛。
        void playNetBatchParallel(const std::vector<std::unique_ptr<NetEvaluator>> &evals,
                                  const std::vector<BattleContext> &starts,
                                  std::vector<Outcome> &out, std::vector<Term> *terms,
                                  int max_steps)
        {
            const std::size_t K = starts.size();
            if (evals.empty())
            {
                throw std::runtime_error("裸网评估没有可用 Session");
            }
            const std::size_t T = std::min(evals.size(), K);
            if (T <= 1)
            {
                playNetOnly(*evals.front(), starts, out, max_steps, terms);
                return;
            }

            std::vector<std::vector<Outcome>> part(T);
            std::vector<std::vector<Term>> part_terms(T);
            std::vector<std::exception_ptr> errors(T);
            std::vector<std::thread> threads;
            threads.reserve(T);
            for (std::size_t t = 0; t < T; ++t)
            {
                const std::size_t begin = K * t / T;
                const std::size_t end = K * (t + 1) / T;
                part[t].assign(end - begin, Outcome::UNDECIDED);
                part_terms[t].assign(end - begin, Term{});
                threads.emplace_back([&, t, begin, end]
                                     {
                                         try
                                         {
                                             std::vector<BattleContext> sub(starts.begin() + begin,
                                                                            starts.begin() + end);
                                             playNetOnly(*evals[t], sub, part[t], max_steps,
                                                          &part_terms[t]);
                                         }
                                         catch (...)
                                         {
                                             errors[t] = std::current_exception();
                                         } });
            }
            for (std::thread &th : threads)
            {
                th.join();
            }
            for (const std::exception_ptr &err : errors)
            {
                if (err)
                {
                    std::rethrow_exception(err);
                }
            }
            for (std::size_t t = 0; t < T; ++t)
            {
                const std::size_t begin = K * t / T;
                for (std::size_t i = 0; i < part[t].size(); ++i)
                {
                    out[begin + i] = part[t][i];
                    if (terms)
                    {
                        (*terms)[begin + i] = part_terms[t][i];
                    }
                }
            }
        }

        /// "net" 策略的配置（探索全关：门控比的是"当前策略强度"，不是"探索后的强度"）。
        SelfplayConfig netCfg(const EvalSpec &spec, int max_steps)
        {
            SelfplayConfig cfg;
            cfg.model_path = spec.model_path;
            cfg.device = spec.device;
            cfg.z_spec = spec.z_spec;
            cfg.sims = spec.sims;
            cfg.sims_cheap_frac = 0.0; // 门控量的是"某一档预算下的策略" ⇒ 不做随机化预算
            cfg.max_steps = max_steps;
            cfg.workers = spec.workers;
            cfg.model_path_cpu = spec.model_path_cpu;
            cfg.sample = false;
            cfg.eps = 0.0;
            cfg.temp_moves = 0;
            cfg.dirichlet_total = 0.0;
            cfg.dirichlet_eps = 0.0;
            return cfg;
        }

        void tally(Outcome o, const Term &t, int pre_hp, int pre_max_hp,
                   EvalSpecStats &st, MonsterEncounter enc)
        {
            ++st.games;
            st.hp_sum += t.cur_hp;
            st.max_hp_sum += t.max_hp;
            st.pre_hp_sum += pre_hp;
            st.pre_max_hp_sum += pre_max_hp;
            st.turn_sum += t.turn;
            if (o == Outcome::PLAYER_VICTORY)
            {
                ++st.wins;
                st.win_hp_sum += t.cur_hp;
                st.win_max_hp_sum += t.max_hp;
                st.win_pre_hp_sum += pre_hp;
                st.win_pre_max_hp_sum += pre_max_hp;
            }
            else if (o == Outcome::UNDECIDED)
            {
                ++st.undecided;
            }

            const std::size_t idx = static_cast<std::size_t>(enc);
            if (idx >= st.per_enc.size())
            {
                st.per_enc.resize(idx + 1);
            }
            EvalSpecStats::Enc &e = st.per_enc[idx];
            if (e.name.empty())
            {
                e.name = monsterEncounterStrings[static_cast<int>(enc)];
                e.hard = isHardStep(MonsterEncounterPool::stepOf(enc));
            }
            ++e.games;
            e.hp_sum += t.cur_hp;
            e.max_hp_sum += t.max_hp;
            e.pre_hp_sum += pre_hp;
            e.pre_max_hp_sum += pre_max_hp;
            e.turn_sum += t.turn;
            if (o == Outcome::PLAYER_VICTORY)
            {
                ++e.wins;
                e.win_hp_sum += t.cur_hp;
                e.win_max_hp_sum += t.max_hp;
                e.win_pre_hp_sum += pre_hp;
                e.win_pre_max_hp_sum += pre_max_hp;
            }
            else if (o == Outcome::UNDECIDED)
            {
                ++e.undecided;
            }
        }
    } // namespace

    std::vector<EvalSpecStats> evalRun(const std::vector<EvalSpec> &specs, int n_per,
                                       std::uint64_t start_seed, int games, int max_steps,
                                       bool hard_only)
    {
        if (specs.empty())
        {
            throw std::runtime_error("eval：至少要一个策略");
        }
        // n_per >= 1 由 `encJobs` 拦（见下）。

        // 任务流 = 采集那张表（`encs.h`）：每个遭遇 × 它那个代表层 × `n_per` 个 seed。
        // **所有策略跑同一份 `jobs`** ⇒ 同一 `(遭遇, 层, seed)` 的局面逐位相同、胜率可直接相减。
        // ⚠️ 局面**逐策略各造一次**（`buildBattle` 是 seed/enc/floor/ascension 的纯函数 ⇒ 每次都
        //    逐位一样）：缓着不造只省一点点构造成本，却要让所有策略共享可变的 `BattleContext`。
        std::vector<EncJob> jobs = encJobs(n_per, start_seed);
        if (hard_only)
        {
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                      [](const EncJob &j)
                                      { return !isHardStep(j.step); }),
                       jobs.end());
        }
        const std::size_t width = static_cast<std::size_t>(std::max(1, games));

        std::vector<EvalSpecStats> out(specs.size());
        for (std::size_t si = 0; si < specs.size(); ++si)
        {
            const EvalSpec &spec = specs[si];
            EvalSpecStats &st = out[si];
            const bool is_net = spec.kind == "net";
            // 裸网络（`sims == 0`）：不起搜索，也不走 `selfplay`（那条路要求 `sims ≥ 2`）。
            // 前向器**每个 spec 建一次**（加载一张 ONNX 图不便宜，别按块重建）。
            const bool bare_net = is_net && spec.sims < 2;
            std::vector<std::unique_ptr<NetEvaluator>> bare_evals;
            if (bare_net)
            {
                const int t = bareThreadCount(spec, width);
                bare_evals.reserve(static_cast<std::size_t>(t));
                for (int i = 0; i < t; ++i)
                {
                    auto ev = std::make_unique<NetEvaluator>(spec.model_path, spec.device, 1);
                    // 头宽 / 执行计划在首次前向后才有；宽度不对当场抛（否则 `logits[下标]` 会越界读）。
                    probeForwardChecked(*ev);
                    bare_evals.push_back(std::move(ev));
                }
            }
            for (std::size_t at = 0; at < jobs.size(); at += width)
            {
                const std::size_t n = std::min(width, jobs.size() - at);
                std::vector<BattleContext> battles;
                battles.reserve(n);
                for (std::size_t k = 0; k < n; ++k)
                {
                    const EncJob &j = jobs[at + k];
                    battles.push_back(buildBattle(j.seed, j.enc, floorForStep(j.step), ascensionOf(j.seed)));
                }
                std::vector<int> pre_hp(n), pre_max_hp(n);
                for (std::size_t k = 0; k < n; ++k)
                {
                    pre_hp[k] = battles[k].player.curHp;
                    pre_max_hp[k] = battles[k].player.maxHp;
                }
                if (bare_net)
                {
                    std::vector<Outcome> got(n);
                    std::vector<Term> terms(n);
                    playNetBatchParallel(bare_evals, battles, got, &terms, max_steps);
                    for (std::size_t i = 0; i < got.size(); ++i)
                    {
                        tally(got[i], terms[i], pre_hp[i], pre_max_hp[i], st, jobs[at + i].enc);
                    }
                    continue;
                }
                if (is_net)
                {
                    // 一次前向吃 `width` 棵树（与采集同形）⇒ 别一棵一棵调，那会把批宽压成 1。
                    // 结果按场序回调 ⇒ 只收终局判定（评估不需要那些行）。
                    std::vector<Outcome> got(n);
                    std::vector<Term> terms(n);
                    SelfplayConfig cfg = netCfg(spec, max_steps);
                    cfg.pool = static_cast<int>(n); // 池宽 = 本轮并肩的局数（`selfplay` 用局数当池宽：一次全开）
                    selfplay(battles, cfg,
                             [&](std::size_t, GameOut &&g)
                             { got[g.task] = g.outcome; terms[g.task] = g.term; });
                    for (std::size_t i = 0; i < got.size(); ++i)
                    {
                        tally(got[i], terms[i], pre_hp[i], pre_max_hp[i], st, jobs[at + i].enc);
                    }
                    continue;
                }
                for (std::size_t k = 0; k < n; ++k)
                {
                    const EncJob &j = jobs[at + k];
                    if (spec.kind == "simple")
                    {
                        // 原版启发式：无搜索、无网络；同一批局面直接对照。
                        // `playoutBattle` 会读 `curGameContext->act`（受击伤害判断），
                        // 但评估只有 BattleContext ⇒ 给一个只填 act 的壳。
                        search::SimpleAgent agent;
                        GameContext shell;
                        shell.act = actOf(floorForStep(j.step));
                        agent.curGameContext = &shell;
                        agent.playoutBattle(battles[k]);
                        tally(battles[k].outcome, readTerm(battles[k]), pre_hp[k], pre_max_hp[k], st, j.enc);
                    }
                    else if (spec.kind == "random")
                    {
                        const Outcome o = playRandom(battles[k], j.seed, max_steps);
                        tally(o, readTerm(battles[k]), pre_hp[k], pre_max_hp[k], st, j.enc);
                    }
                    else if (spec.kind == "teacher")
                    {
                        TeacherConfig cfg;
                        cfg.sims = spec.sims;
                        cfg.max_steps = max_steps;
                        GameOut g = teacherPlayGame(battles[k], cfg, j.seed);
                        tally(g.outcome, g.term, pre_hp[k], pre_max_hp[k], st, j.enc);
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
            .def_readwrite("model_path", &EvalSpec::model_path)
            .def_readwrite("device", &EvalSpec::device)
            .def_readwrite("z_spec", &EvalSpec::z_spec)
            .def_readwrite("sims", &EvalSpec::sims)
            .def_readwrite("workers", &EvalSpec::workers)
            .def_readwrite("model_path_cpu", &EvalSpec::model_path_cpu);

        py::class_<EvalSpecStats>(m, "EvalSpecStats")
            .def_readonly("games", &EvalSpecStats::games)
            .def_readonly("wins", &EvalSpecStats::wins)
            .def_readonly("undecided", &EvalSpecStats::undecided)
            .def_readonly("hp_sum", &EvalSpecStats::hp_sum)
            .def_readonly("max_hp_sum", &EvalSpecStats::max_hp_sum)
            .def_readonly("pre_hp_sum", &EvalSpecStats::pre_hp_sum)
            .def_readonly("pre_max_hp_sum", &EvalSpecStats::pre_max_hp_sum)
            .def_readonly("win_hp_sum", &EvalSpecStats::win_hp_sum)
            .def_readonly("win_max_hp_sum", &EvalSpecStats::win_max_hp_sum)
            .def_readonly("turn_sum", &EvalSpecStats::turn_sum)
            .def_property_readonly(
                "win_rate",
                [](const EvalSpecStats &s)
                { return s.games > 0 ? static_cast<double>(s.wins) / s.games : 0.0; })
            .def_property_readonly(
                "avg_hp",
                [](const EvalSpecStats &s)
                { return s.games > 0 ? static_cast<double>(s.hp_sum) / s.games : 0.0; })
            .def_property_readonly(
                "hp_ratio",
                [](const EvalSpecStats &s)
                { return s.max_hp_sum > 0 ? static_cast<double>(s.hp_sum) / s.max_hp_sum : 0.0; })
            .def_property_readonly(
                "damage_ratio",
                [](const EvalSpecStats &s)
                { return s.pre_max_hp_sum > 0
                             ? static_cast<double>(s.pre_hp_sum - s.hp_sum) / s.pre_max_hp_sum
                             : 0.0; })
            .def_property_readonly(
                "avg_damage",
                [](const EvalSpecStats &s)
                { return s.games > 0
                             ? static_cast<double>(s.pre_hp_sum - s.hp_sum) / s.games
                             : 0.0; })
            .def_property_readonly(
                "win_damage_ratio",
                [](const EvalSpecStats &s)
                { return s.win_pre_max_hp_sum > 0
                             ? static_cast<double>(s.win_pre_hp_sum - s.win_hp_sum) /
                                   s.win_pre_max_hp_sum
                             : 0.0; })
            .def_property_readonly(
                "avg_win_hp_ratio",
                [](const EvalSpecStats &s)
                { return s.win_max_hp_sum > 0
                             ? static_cast<double>(s.win_hp_sum) / s.win_max_hp_sum
                             : 0.0; })
            .def_property_readonly(
                "avg_turns",
                [](const EvalSpecStats &s)
                { return s.games > 0 ? static_cast<double>(s.turn_sum) / s.games : 0.0; })
            .def_readonly("per_enc", &EvalSpecStats::per_enc);

        py::class_<EvalSpecStats::Enc>(m, "EvalEncStats")
            .def_readonly("name", &EvalSpecStats::Enc::name)
            .def_readonly("hard", &EvalSpecStats::Enc::hard)
            .def_readonly("games", &EvalSpecStats::Enc::games)
            .def_readonly("wins", &EvalSpecStats::Enc::wins)
            .def_readonly("undecided", &EvalSpecStats::Enc::undecided)
            .def_readonly("hp_sum", &EvalSpecStats::Enc::hp_sum)
            .def_readonly("max_hp_sum", &EvalSpecStats::Enc::max_hp_sum)
            .def_readonly("pre_hp_sum", &EvalSpecStats::Enc::pre_hp_sum)
            .def_readonly("pre_max_hp_sum", &EvalSpecStats::Enc::pre_max_hp_sum)
            .def_readonly("win_hp_sum", &EvalSpecStats::Enc::win_hp_sum)
            .def_readonly("win_max_hp_sum", &EvalSpecStats::Enc::win_max_hp_sum)
            .def_readonly("turn_sum", &EvalSpecStats::Enc::turn_sum)
            .def_property_readonly(
                "win_rate",
                [](const EvalSpecStats::Enc &s)
                { return s.games > 0 ? static_cast<double>(s.wins) / s.games : 0.0; })
            .def_property_readonly(
                "hp_ratio",
                [](const EvalSpecStats::Enc &s)
                { return s.max_hp_sum > 0 ? static_cast<double>(s.hp_sum) / s.max_hp_sum : 0.0; })
            .def_property_readonly(
                "damage_ratio",
                [](const EvalSpecStats::Enc &s)
                { return s.pre_max_hp_sum > 0
                             ? static_cast<double>(s.pre_hp_sum - s.hp_sum) / s.pre_max_hp_sum
                             : 0.0; })
            .def_property_readonly(
                "avg_damage",
                [](const EvalSpecStats::Enc &s)
                { return s.games > 0
                             ? static_cast<double>(s.pre_hp_sum - s.hp_sum) / s.games
                             : 0.0; })
            .def_property_readonly(
                "win_damage_ratio",
                [](const EvalSpecStats::Enc &s)
                { return s.win_pre_max_hp_sum > 0
                             ? static_cast<double>(s.win_pre_hp_sum - s.win_hp_sum) /
                                   s.win_pre_max_hp_sum
                             : 0.0; })
            .def_property_readonly(
                "avg_win_hp_ratio",
                [](const EvalSpecStats::Enc &s)
                { return s.win_max_hp_sum > 0
                             ? static_cast<double>(s.win_hp_sum) / s.win_max_hp_sum
                             : 0.0; })
            .def_property_readonly(
                "avg_turns",
                [](const EvalSpecStats::Enc &s)
                { return s.games > 0 ? static_cast<double>(s.turn_sum) / s.games : 0.0; });

        m.def(
            "eval_run",
            [](const std::vector<EvalSpec> &specs, int n_per, std::uint64_t start_seed,
               int games, int max_steps, bool hard_only)
            {
                std::vector<EvalSpecStats> stats;
                {
                    // 整段不碰 Python ⇒ 放 GIL（与 `sts.teacher_run` / `sts.selfplay_run` 同）。
                    py::gil_scoped_release release;
                    stats = sts::collect::evalRun(specs, n_per, start_seed, games, max_steps, hard_only);
                }
                return stats;
            },
            py::arg("specs"), py::arg("n_per"), py::arg("start_seed"), py::arg("games") = 8,
            py::arg("max_steps") = 1000, py::arg("hard_only") = false,
            "**配对评估**（任务表 + 造局面 + 搜索 + 统计全在 C++）：同一批 `(遭遇, 层, seed)`\n"
            "喂给所有策略，返回与 `specs` 同序的统计（`games` / `wins` / `undecided` / `win_rate`）。");
    }

} // namespace sts::collect
