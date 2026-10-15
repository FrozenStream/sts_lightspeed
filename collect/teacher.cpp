#include "teacher.h"

#include "bind.h" // `registerTeacher` 的声明（定义要签名对得上）

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
#include "shardio.h"    // ShardWriter / kPolicyDim / kSelectDim
#include "snapshot.h"   // snapshotObs（`Frame::obs` 的填充）

#include "sim/search/BattleScumSearcher2.h"

namespace sts::collect
{
    namespace
    {
        // get the most visited action, exploring with eps or tau
        search::Action pickTeacherAction(
            const std::vector<std::pair<search::Action, std::int64_t>> &edges,
            const std::vector<search::Action> &legal, const TeacherConfig &cfg,
            std::mt19937_64 &rng, bool exploring)
        {
            std::int64_t total = 0;
            for (const auto &e : edges)
                total += e.second;

            const double r = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
            // if rand < eps: random action
            if (exploring && cfg.eps > 0.0 && r < cfg.eps && !legal.empty())
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
            const bool exploring = cfg.eps > 0.0 || cfg.tau > 1e-8;
            GameOut g;
            for (int step = 0; step < cfg.max_steps && b.outcome == Outcome::UNDECIDED; ++step)
            {
                search::BattleScumSearcher2 searcher(b);
                searcher.search(cfg.sims);
                const bool is_sel = b.inputState == InputState::CARD_SELECT;
                const int dim = is_sel ? kSelectDim : kPolicyDim;
                Frame f;
                f.is_sel = is_sel;
                f.obs = snapshotObs(b);
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
                const search::Action pick = pickTeacherAction(edges, legal_acts, cfg, rng, exploring);
                f.a_idx = actionToIdx(b, pick);
                g.frames.push_back(std::move(f));
                pick.execute(b);
            }
            g.term = readTerm(b);
            g.outcome = b.outcome;
            return g;
        }

        void runTeacherAll(const std::vector<EncJob> &jobs, const TeacherConfig &cfg, std::size_t threads, const std::function<void(std::size_t, GameOut &&)> &emit)
        {
            const std::size_t n = jobs.size();
            const std::size_t W = std::min(threads > 0 ? threads : std::thread::hardware_concurrency(), n);

            // 槽位 = 环的单元，`S > W`（= 2×线程数）给了**写侧余量**：线程取 `j` 只需"槽
            // `j % S` 空"，等价于"`j - S` 已被写走" ⇒ 线程不必等写头追上自己，只受"写头落后
            // 不超过 S 场"约束（写延迟尖峰被余量吸收）。
            // 两个旗标缺一不可：
            //   `busy` = 槽被占用（线程在算 / 结果待取）；`done` = 结果已填好、等写侧取。
            //   只用 `busy` 分不清"正在算"与"算完了"（写侧会读到半成品）；只用 `done` 分不清
            //   "槽可领"与"结果待取"（线程会覆盖掉待写的结果）。
            struct Slot
            {
                GameOut out; // 结果（写侧 `move` 走 ⇒ 槽归空时内存已释放）
                bool busy = false;
                bool done = false;
            };
            const std::size_t S = 2 * W;
            std::vector<Slot> slots(S);

            std::size_t started = 0; // 已领走的场数（`j = started++`）
            std::size_t emitted = 0; // 已写出的场数（= 队头）
            std::exception_ptr err;
            std::mutex m;
            // ⚠️ **worker 共用一个 cv**：槽数 > 线程数之后，"槽 k 空"不再对应唯一等待者
            //    ⇒ `notify_one` 打在非等待者身上就是**丢失唤醒**。所以谓词是**全局的**
            //    （"在飞 < S"），任何被叫醒的人都能用这个空槽 ⇒ `notify_one` 语义正确、无惊群。
            std::condition_variable cv_free;  // worker 等"有空槽可领"
            std::condition_variable cv_write; // 主线程等"队头就绪"

            const auto t0 = std::chrono::steady_clock::now();
            const std::size_t mark = std::max<std::size_t>(1, n / 20);

            auto work = [&]
            {
                try
                {
                    for (;;)
                    {
                        std::size_t j = 0;
                        {
                            std::unique_lock<std::mutex> lk(m);
                            // 领号前等"有空槽"：在飞 = `started - emitted`，满 `S` 就等写侧腾位。
                            // ⚠️ `err` 必须在谓词里：写侧一旦因异常 `break`，就再没人腾位 ⇒
                            //    只靠 catch 里那一次 `notify_all` 会**永久睡下去**（`join` 死锁）。
                            while (!err && started < n && started - emitted >= S)
                            {
                                cv_free.wait(lk);
                            }
                            if (err || started >= n)
                            {
                                return; // 出错 / 领完 ⇒ 收工（剩下的在槽里，不再需要）
                            }
                            j = started++;
                            slots[j % S].busy = true;
                            slots[j % S].done = false;
                        }
                        const EncJob &job = jobs[j];
                        BattleContext b = buildBattle(job.seed, job.enc, job.floor, ascensionOf(job.seed));
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
                    std::lock_guard<std::mutex> lk(m);
                    err = std::current_exception();
                    cv_write.notify_all();
                    cv_free.notify_all();
                }
            };

            std::vector<std::thread> pool;
            pool.reserve(W);
            for (std::size_t w = 0; w < W; ++w)
            {
                pool.emplace_back(work);
            }
            // 主线程：唯一的写者，只写**队头**。写完 `move` 出结果、槽归空 ⇒ 精确唤醒拥有它的线程。
            for (;;)
            {
                GameOut g;
                {
                    std::unique_lock<std::mutex> lk(m);
                    while (!(err || emitted >= n || slots[emitted % S].done))
                    {
                        cv_write.wait(lk);
                    }
                    if (err)
                    {
                        break;
                    }
                    if (emitted >= n)
                    {
                        break; // 全写完
                    }
                    Slot &s = slots[emitted % S];
                    g = std::move(s.out);
                    s.done = false;
                    s.busy = false; // 归空 ⇒ 在飞减 1
                }
                cv_free.notify_one(); // 腾出一个空槽 ⇒ 叫醒一个 worker
                emit(emitted, std::move(g));
                ++emitted;
                if (emitted % mark == 0)
                {
                    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::fprintf(stderr, "  教师 %zu/%zu 场（%.0fs）\n", emitted, n, sec);
                }
            }
            for (auto &t : pool)
            {
                t.join();
            }
            if (err)
            {
                std::rethrow_exception(err);
            }
        }

    } // namespace

    std::vector<std::string> teacherRun(int n_per, const TeacherConfig &cfg, std::uint64_t start_seed,
                                        const std::string &out_dir, int shard_rows)    {
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

        // 一批的场数：**常驻线程池** + 就地造局面（2026-10-11 重写；此前是"每批新建一组线程 +
        // 主线程一次造 batch 个局面"）—— 调度形状的完整说明在 `runTeacherBlock` 上方。
        // ⚠️ 分块只改内存峰值，不改数据：与 `shard_rows` 正交（那个管**文件**切多大）。
        // **跑一场、写一场**：`emit` 由 `runTeacherAll` 按**交付序**回调。落盘顺序即数据行序
        // （同一场的行连续；主线程是唯一的写者，写完当场释放那一场的内存）。
        const std::vector<EncJob> jobs = encJobs(n_per, start_seed);
        auto emit = [&](std::size_t j, GameOut &&g)
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
        };
        runTeacherAll(jobs, cfg, cfg.threads, emit);
        const std::string last = w.flush();
        if (!last.empty())
        {
            written.push_back(last);
        }
        return written;
    }

    // 评估用：把**给定局面**用教师策略打完（`runTeacherGame` 就是 `teacherRun` 逐场调的那个）。
    Outcome teacherPlay(const BattleContext &start, const TeacherConfig &cfg, std::uint64_t seed)
    {
        return runTeacherGame(start, seed, cfg).outcome;
    }

    void registerTeacher(pybind11::module_ &m)
    {
        namespace py = pybind11;
        py::class_<TeacherConfig>(m, "TeacherConfig")
            .def(py::init<>())
            .def_readwrite("sims", &TeacherConfig::sims)
            .def_readwrite("max_steps", &TeacherConfig::max_steps)
            .def_readwrite("tau", &TeacherConfig::tau)
            .def_readwrite("eps", &TeacherConfig::eps)
            .def_readwrite("threads", &TeacherConfig::threads);
        m.def(
            "teacher_run",
            [](int n_per, const TeacherConfig &cfg, std::uint64_t start_seed, const std::string &out_dir, int shard_rows)
            {
                return sts::collect::teacherRun(n_per, cfg, start_seed, out_dir, shard_rows);
            },
            py::arg("n_per"), py::arg("cfg"), py::arg("start_seed"), py::arg("out_dir"),
            py::arg("shard_rows") = 200000,
            "**整段采集**（任务表 + 造局面 + 搜索 + 落盘全在 C++，不经过 Python）：\n"
            "按**引擎的遭遇总表**（`MonsterEncounterPool::allEnemies`）铺开 `n_per` 场/项，分片写进\n"
            "`out_dir`，返回路径列表。每行自带 `enc` / `enc_floor` 两列 ⇒ 调用方不必知道表长什么样。");
    }

} // namespace sts::collect