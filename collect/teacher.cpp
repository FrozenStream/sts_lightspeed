#include "teacher.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

#include "action.h"     // 动作层（枚举 / 编码 / 反解；`sts::collect`）
#include "encs.h"       // 任务表（引擎派生）
#include "scenebuild.h" // buildBattle / ascensionOf
#include "snapshot.h"   // snapshotObs（`Frame::obs` 的填充）

#include "sim/search/BattleScumSearcher2.h"

#include "shard_out.h" // ShardSink（落盘外壳：目录 / 命名 / 切分 / 汇总）

namespace sts::collect
{
    namespace
    {
        // get the most visited action, exploring with eps or tau
        search::Action pickTeacherAction(const std::vector<std::pair<search::Action, std::int64_t>> &edges, const std::vector<search::Action> &legal, const TeacherConfig &cfg, std::mt19937_64 &rng)
        {
            std::int64_t total = 0;
            for (const auto &e : edges)
                total += e.second;

            const double r = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
            // if rand < eps: random action
            if (cfg.eps > 0.0 && r < cfg.eps && !legal.empty())
            {
                std::uniform_int_distribution<std::size_t> ui(0, legal.size() - 1);
                return legal[ui(rng)];
            }
            // elif tau > 0: sample ~ pi^(1/tau)
            if (cfg.tau > 1e-8 && total > 0)
            {
                std::vector<double> w;
                w.reserve(edges.size());
                for (const auto &e : edges)
                {
                    w.push_back(std::pow(static_cast<double>(e.second), 1.0 / cfg.tau));
                }
                std::discrete_distribution<std::size_t> dd(w.begin(), w.end());
                return edges[dd(rng)].first;
            }
            // else: pick most visited
            std::int64_t best = -1;
            search::Action pick;
            for (const auto &e : edges)
            {
                if (e.second > best)
                {
                    best = e.second;
                    pick = e.first;
                }
            }
            if (best < 0)
                throw std::runtime_error("教师搜不出可执行的动作（根边全 0 且无探索？）");
            return pick;
        }

        GameOut runTeacherGame(const BattleContext &start, std::uint64_t seed, const TeacherConfig &cfg)
        {
            BattleContext b = start;
            std::mt19937_64 rng(seed * 1000003ULL + 11ULL);
            GameOut g;
            for (int step = 0; step < cfg.max_steps && b.outcome == Outcome::UNDECIDED; ++step)
            {
                search::BattleScumSearcher2 searcher(b);
                searcher.search(cfg.sims);
                g.transitions += searcher.transitionCount;
                g.rollouts += cfg.sims;
                const bool is_sel = b.inputState == InputState::CARD_SELECT;
                Frame f;
                f.is_sel = is_sel;
                f.obs = sts::collect::snapshotOf(b);
                std::vector<std::pair<search::Action, std::int64_t>> edges;
                for (const auto &e : searcher.root.edges)
                {
                    f.pi_idx.push_back(actionToIdx(b, e.action));
                    f.pi_n.push_back(e.node.simulationCount);
                    edges.emplace_back(e.action, e.node.simulationCount);
                }
                const std::vector<search::Action> legal_acts = enumerateActions(b);
                for (const auto &a : legal_acts)
                {
                    f.legal.push_back(actionToIdx(b, a));
                }
                const search::Action pick = pickTeacherAction(edges, legal_acts, cfg, rng);
                f.a_idx = actionToIdx(b, pick);
                g.frames.push_back(std::move(f));
                pick.execute(b);
            }
            g.term = readTerm(b);
            g.outcome = b.outcome;
            g.decisions = static_cast<std::int64_t>(g.frames.size());
            return g;
        }

        void runTeacherAll(const std::vector<EncJob> &jobs, const TeacherConfig &cfg, std::size_t threads, const std::function<void(std::size_t, GameOut &&)> &emit)
        {
            const std::size_t n = jobs.size();
            if (n == 0)
            {
                return;
            }
            const std::size_t requested = threads > 0 ? threads : std::thread::hardware_concurrency();
            const std::size_t W = std::max<std::size_t>(1, std::min(requested, n));

            // 槽位 = 环的单元，`S > W`（= 2×线程数）给了**写侧余量**：线程取 `j` 只需"槽
            // `j % S` 空"，等价于"`j - S` 已被写走" ⇒ 线程不必等写头追上自己，只受"写头落后
            // 不超过 S 场"约束（写延迟尖峰被余量吸收）。
            struct Slot
            {
                GameOut out;
                bool done = false;
            };
            const std::size_t S = 2 * W;
            std::vector<Slot> slots(S);

            std::size_t started = 0; // 已领走的场数（`j = started++`）
            std::size_t emitted = 0; // 已写出的场数（= 队头）
            std::exception_ptr err;
            std::mutex m;
            std::condition_variable cv_free;  // worker 等"有空槽可领"
            std::condition_variable cv_write; // 主线程等"队头就绪"

            const auto t0 = std::chrono::steady_clock::now();
            const std::size_t mark = std::max<std::size_t>(1, n / 20);

            auto notifyStop = [&]
            {
                cv_free.notify_all();
                cv_write.notify_all();
            };

            auto work = [&]
            {
                try
                {
                    while (true)
                    {
                        std::size_t j = 0;
                        {
                            std::unique_lock<std::mutex> lk(m);
                            // 领号前等"有空槽"：在飞 = `started - emitted`，满 `S` 就等写侧腾位。
                            while (!err && started < n && started - emitted >= S)
                            {
                                cv_free.wait(lk);
                            }
                            if (err || started >= n)
                            {
                                return; // 出错 / 领完 ⇒ 收工（剩下的在槽里，不再需要）
                            }
                            j = started++;
                            slots[j % S].done = false;
                        }
                        const EncJob &job = jobs[j];
                        BattleContext b = buildBattle(job.seed, job.enc, floorForStep(job.step), ascensionOf(job.seed));
                        GameOut g = runTeacherGame(b, job.seed, cfg);
                        {
                            std::lock_guard<std::mutex> lk(m);
                            Slot &s = slots[j % S];
                            s.out = std::move(g);
                            s.done = true;
                        }
                        cv_write.notify_one(); // awake the writer
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
                    }
                    notifyStop();
                }
            };

            std::vector<std::thread> pool;
            pool.reserve(W);
            try
            {
                for (std::size_t w = 0; w < W; ++w)
                {
                    pool.emplace_back(work);
                }

                // 主线程：唯一的写者，只写**队头**。写盘回调在锁外执行，避免阻塞 worker。
                while (true)
                {
                    GameOut g;
                    std::size_t current = 0;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        while (!err && emitted < n && !slots[emitted % S].done)
                        {
                            cv_write.wait(lk);
                        }
                        if (err || emitted >= n)
                        {
                            break;
                        }
                        current = emitted;
                        Slot &s = slots[current % S];
                        g = std::move(s.out);
                        s.done = false;
                    }

                    emit(current, std::move(g));
                    {
                        std::lock_guard<std::mutex> lk(m);
                        ++emitted;
                    }
                    cv_free.notify_one(); // 计数更新后腾出一个空槽
                    if (current + 1 == n || (current + 1) % mark == 0)
                    {
                        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                        std::fprintf(stderr, "  教师 %zu/%zu 场（%.0fs）\n", current + 1, n, sec);
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
                }
                notifyStop();
            }

            notifyStop();
            for (auto &t : pool)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }
            if (err)
            {
                std::rethrow_exception(err);
            }
        }

    } // namespace

    std::vector<std::string> teacherRun(int n_per, const TeacherConfig &cfg, std::uint64_t start_seed, const std::string &out_dir, int shard_rows)
    {
        const std::vector<EncJob> jobs = encJobs(n_per, start_seed);
        ShardSink sink(out_dir, shard_rows);
        runTeacherAll(jobs, cfg, cfg.threads, [&](std::size_t, GameOut &&g)
                      { sink.addGame(g); });
        return sink.finish();
    }

    // 评估用：把**给定局面**用教师策略打完（`runTeacherGame` 就是 `teacherRun` 逐场调的那个）。
    Outcome teacherPlay(const BattleContext &start, const TeacherConfig &cfg, std::uint64_t seed)
    {
        return runTeacherGame(start, seed, cfg).outcome;
    }

    GameOut teacherPlayGame(const BattleContext &start, const TeacherConfig &cfg, std::uint64_t seed)
    {
        return runTeacherGame(start, seed, cfg);
    }

    bool teacherTurnKills(const BattleContext &start, int target, const TeacherConfig &cfg,
                          std::uint64_t seed, int max_actions)
    {
        BattleContext b = start;
        std::mt19937_64 rng(seed * 1000003ULL + 17ULL);
        const int turn0 = b.turn;
        for (int step = 0; step < max_actions && b.outcome == Outcome::UNDECIDED; ++step)
        {
            if (target >= 0 && target < b.monsters.monsterCount && b.monsters.arr[target].curHp <= 0)
            {
                return true;
            }
            search::BattleScumSearcher2 searcher(b);
            searcher.search(cfg.sims);
            std::vector<std::pair<search::Action, std::int64_t>> edges;
            for (const auto &e : searcher.root.edges)
            {
                edges.emplace_back(e.action, e.node.simulationCount);
            }
            const std::vector<search::Action> legal = enumerateActions(b);
            const search::Action pick = pickTeacherAction(edges, legal, cfg, rng);
            pick.execute(b);
            if (b.turn != turn0)
            {
                break;
            }
        }
        return target >= 0 && target < b.monsters.monsterCount && b.monsters.arr[target].curHp <= 0;
    }

} // namespace sts::collect