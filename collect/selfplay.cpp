#include "selfplay.h"

#include "selfplay_forward.h"
#include "selfplay_pool.h"
#include "selfplay_report.h"
#include "selfplay_tree.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "encs.h"
#include "netprobe.h" // probeForwardChecked（前向会话的建立校验）
#include "scenebuild.h"
#include "shard_out.h" // ShardSink（落盘外壳：目录 / 命名 / 切分 / 汇总）
#include "zvalue.h"
#include "snapshot.h"
#include "net_eval.h"

namespace sts::collect
{
    namespace
    {
        using detail::descendOne;
        using detail::expandBackup;
        using detail::ForwardService;
        using detail::nextSims;
        using detail::ReadyQueue;
        using detail::recordFrame;
        using detail::resetTree;
        using detail::RunStats;
        using detail::stepAndAdvance;
        using detail::TaskWindow;
        using detail::Tree;

        void validateSelfplayConfig(const SelfplayConfig &cfg)
        {
            if (cfg.sims < 2)
            {
                throw std::runtime_error("sims 太小：至少 2（否则本场搜不出可记的 π）");
            }
            if (!(cfg.sims_cheap_frac >= 0.0 && cfg.sims_cheap_frac < 1.0))
            {
                throw std::runtime_error("sims_cheap_frac 必须在 [0, 1) 内（0 = 关；1 = 两档没区别）");
            }
            if (cfg.max_steps <= 0)
            {
                throw std::runtime_error("max_steps 必须为正数");
            }
            if (!(cfg.full_frac >= 0.0 && cfg.full_frac <= 1.0))
            {
                throw std::runtime_error("full_frac 必须在 [0, 1] 内");
            }
            if (!(cfg.dirichlet_eps >= 0.0 && cfg.dirichlet_eps <= 1.0))
            {
                throw std::runtime_error("dirichlet_eps 必须在 [0, 1] 内");
            }
            if (cfg.dirichlet_eps > 0.0 &&
                !(std::isfinite(cfg.dirichlet_total) && cfg.dirichlet_total > 0.0))
            {
                throw std::runtime_error("启用根噪声时 dirichlet_total 必须为正数");
            }
            if (!(std::isfinite(cfg.c_puct) && cfg.c_puct >= 0.0 && std::isfinite(cfg.q_init)) ||
                !(cfg.eps >= 0.0 && cfg.eps <= 1.0))
            {
                throw std::runtime_error("搜索参数必须有限，c_puct ≥ 0，eps 在 [0, 1] 内");
            }
            terminalValue(BattleContext{}, cfg.z_spec);
        }

        void printLaunchOrder(const std::vector<EncJob> &jobs)
        {
            if (jobs.empty())
            {
                std::fprintf(stderr, "  上车序：（空）\n");
                return;
            }

            std::string line = "  上车序：";
            constexpr std::size_t kPreview = 6;
            for (std::size_t j = 0; j < jobs.size() && j < kPreview; ++j)
            {
                if (j > 0)
                {
                    line += ' ';
                }
                line += monsterEncounterStrings[static_cast<int>(jobs[j].enc)];
            }
            line += " … ";
            const std::size_t tail = jobs.size() >= 2 ? jobs.size() - 2 : jobs.size() - 1;
            line += monsterEncounterStrings[static_cast<int>(jobs[tail].enc)];
            line += ' ';
            line += monsterEncounterStrings[static_cast<int>(jobs.back().enc)];
            line += "（共 " + std::to_string(jobs.size()) + " 场）";
            std::fprintf(stderr, "%s\n", line.c_str());
        }

        BattleContext taskForJob(const EncJob &job)
        {
            return buildBattle(job.seed, job.enc, floorForStep(job.step), ascensionOf(job.seed));
        }

        std::size_t treeSlotCount(std::size_t task_count, std::size_t pool)
        {
            return std::min(std::max<std::size_t>(1, pool), task_count);
        }

        // 交付环容量 D = min(total, 8 × 池宽)：落后量只是内存，不能反过来卡住补场。
        std::size_t deliveryCapacity(std::size_t task_count, std::size_t tree_slots)
        {
            return tree_slots > task_count / 8 ? task_count : tree_slots * 8;
        }

        // 交付侧只关心“第几场写完了”；进度打印不再混进 `emit` 回调。
        class DeliveryProgress
        {
        public:
            explicit DeliveryProgress(std::size_t total)
                : total_(total), mark_(std::max<std::size_t>(1, total / 20)),
                  start_(std::chrono::steady_clock::now()), last_(start_)
            {
            }

            void tick()
            {
                ++done_;
                const auto now = std::chrono::steady_clock::now();
                if (done_ % mark_ != 0 && now - last_ <= std::chrono::seconds(60))
                {
                    return;
                }
                last_ = now;
                const double seconds = std::chrono::duration<double>(now - start_).count();
                std::fprintf(stderr, "  自对弈 交付 %zu/%zu 场（%.0fs）\n", done_, total_, seconds);
            }

        private:
            std::size_t total_;
            std::size_t mark_;
            std::size_t done_ = 0;
            std::chrono::steady_clock::time_point start_;
            std::chrono::steady_clock::time_point last_;
        };

        class PooledSelfplayRunner
        {
            struct TreeState
            {
                int sims_remaining = 0;
                bool awaiting_forward = false;
            };

            // 一个树槽：树（空 = 槽空闲）+ 状态机寄存器 + 前向往返缓冲（obs 出去，pa/ps/val 回来）。
            // 只归当前持有它的 worker 写；槽的分配、ready/idle 与交付环由 pool_mutex_ 保护。
            struct Slot
            {
                std::optional<Tree> tree;
                TreeState state;
                ForwardService::Query query;
            };

            struct StartClaim
            {
                std::size_t slot = 0;
                std::size_t task = 0;
            };

            enum class StepResult
            {
                Ready,
                InFlight,
                Done
            };

            // 单树状态机：四个动作 —— 做树 / 等前向 / 自己前向 / 状态更新回调。
            // 槽不绑定线程 ⇒ “等前向”只能让出线程（返回 InFlight）；结果到了由 `on_batch_ready`
            // 把槽重新压回 ready_，下次进 `advance` 从状态更新继续。
            class TreeStepper
            {
            public:
                TreeStepper(const SelfplayConfig &cfg, ForwardService &forward)
                    : cfg_(cfg), forward_(forward)
                {
                }

                StepResult advance(Tree &tree, TreeState &state, ForwardService::Query &query,
                                   std::size_t slot, NetEvaluator *local)
                {
                    if (state.awaiting_forward)
                    {
                        applyReply(tree, state, query);
                    }
                    if (tree.finished)
                    {
                        return StepResult::Done;
                    }

                    switch (doTree(tree, state))
                    {
                    case TreeStep::Done:
                        return StepResult::Done;
                    case TreeStep::Ready:
                        return StepResult::Ready;
                    case TreeStep::NeedForward:
                        break;
                    }
                    return local ? ownForward(tree, state, query, *local)
                                 : submitQuery(tree, state, query, slot);
                }

            private:
                enum class TreeStep
                {
                    Done,
                    Ready,
                    NeedForward
                };

                // 做树：模拟数用完就记一帧并推进局面，否则往下推一步（推得动就不用网络）。
                TreeStep doTree(Tree &tree, TreeState &state)
                {
                    if (state.sims_remaining <= 0)
                    {
                        Frame frame = recordFrame(tree);
                        std::int64_t visits = 0;
                        for (std::int64_t count : frame.pi_n)
                        {
                            visits += count;
                        }
                        if (visits <= 0)
                        {
                            throw std::runtime_error("根没有任何被访问过的边（本场搜不出可记的 π）");
                        }
                        tree.out.frames.push_back(std::move(frame));
                        stepAndAdvance(tree, cfg_, tree.out.frames.back());
                        state.sims_remaining = nextSims(tree, cfg_);
                        if (tree.finished)
                        {
                            return TreeStep::Done;
                        }
                    }

                    --state.sims_remaining;
                    return descendOne(tree, cfg_) ? TreeStep::Ready : TreeStep::NeedForward;
                }

                // 状态更新回调：用回复（pa/ps/val）更新树状态。
                void applyReply(Tree &tree, TreeState &state, const ForwardService::Query &query)
                {
                    state.awaiting_forward = false;
                    const bool selecting_card = tree.scratch.inputState == InputState::CARD_SELECT;
                    const float *logits = selecting_card ? query.ps.data() : query.pa.data();
                    expandBackup(tree, cfg_, logits, static_cast<double>(query.val));
                }

                // 提交：把观测交给共享前向服务并让出线程（回复到了由 Runner 重新入队）。
                StepResult submitQuery(Tree &tree, TreeState &state, ForwardService::Query &query,
                                       std::size_t slot)
                {
                    tree.replay();
                    query.slot = slot;
                    query.obs = sts::collect::snapshotOf(tree.scratch);
                    state.awaiting_forward = true;
                    forward_.push(query);
                    return StepResult::InFlight;
                }

                // 自己前向：尾段用本地 CPU 会话算这一行（下一轮 `applyForward` 消费）。
                StepResult ownForward(Tree &tree, TreeState &state, ForwardService::Query &query,
                                      NetEvaluator &local)
                {
                    tree.replay();
                    query.obs = sts::collect::snapshotOf(tree.scratch);
                    state.awaiting_forward = true;
                    const auto fwd = local.eval(query.obs.data(), 1,
                                                static_cast<std::int64_t>(query.obs.size()));
                    std::copy_n(fwd.pa, kPolicyDim, query.pa.begin());
                    std::copy_n(fwd.ps, kSelectDim, query.ps.begin());
                    query.val = fwd.val[0];
                    return StepResult::Ready;
                }

                const SelfplayConfig &cfg_;
                ForwardService &forward_;
            };

            struct WorkerWaiter
            {
                std::condition_variable cv;
                bool sleeping = false;
            };

            // 线程集合 + 收尾顺序：先置 stop，再停前向服务，最后 join。
            // 成员顺序即析构顺序，fleet_ 必须是 Runner 的最后一个成员。
            struct WorkerFleet
            {
                std::mutex &pool_mutex;
                std::atomic<bool> &stop;
                ForwardService &forward;
                std::vector<std::thread> threads;
                std::vector<std::unique_ptr<WorkerWaiter>> &waiters;

                void stopAndJoin()
                {
                    {
                        std::lock_guard<std::mutex> lock(pool_mutex);
                        stop = true;
                    }
                    forward.requestStop();
                    {
                        std::lock_guard<std::mutex> lock(pool_mutex);
                        for (const auto &waiter : waiters)
                        {
                            if (waiter->sleeping)
                            {
                                waiter->cv.notify_one();
                            }
                        }
                    }
                    for (std::thread &thread : threads)
                    {
                        if (thread.joinable())
                        {
                            thread.join();
                        }
                    }
                    forward.shutdown();
                }

                ~WorkerFleet()
                {
                    stopAndJoin();
                }
            };

        public:
            // 引用 / const / 无默认构造的成员只能走初始化列表；列表顺序与声明顺序一致。
            PooledSelfplayRunner(std::size_t task_count, std::size_t pool,
                                 const TaskFactory &make_task,
                                 const SelfplayConfig &cfg,
                                 const GameSink &emit)
                : make_task_(make_task), cfg_(cfg), emit_(emit),
                  eval_(cfg.model_path, cfg.device, 1),
                  tree_slot_count_(treeSlotCount(task_count, pool)),
                  slots_(tree_slot_count_),
                  window_(task_count, deliveryCapacity(task_count, tree_slot_count_)),
                  forward_(eval_, cfg.model_path_cpu.empty() ? cfg.model_path : cfg.model_path_cpu),
                  stepper_(cfg, forward_),
                  worker_count_(effectiveWorkerCount()),
                  fleet_{pool_mutex_, stop_, forward_, {}, worker_waiters_}
            {
                initializeIdleSlots();
                initializeWorkerWaiters();
                configureForwardService();
            }

            void run()
            {
                // 首次前向必须在主线程跑过：头宽 / 执行计划 / CUDA 上下文那时才建立。
                probeForwardChecked(eval_);
                forward_.start();
                topUpPool();
                seedReadyQueueInLaunchOrder();
                startWorkers();
                deliverResults();
                fleet_.stopAndJoin();
                printStats();
                rethrowFailure();
            }

        private:
            // ── 构造 / 生命周期 ────────────────────────────────────────────────
            void seedReadyQueueInLaunchOrder()
            {
                // ready_ 是 LIFO 队列；反转后首轮按任务号升序出队（与改造前逐位一致）。
                ready_.reverseInPlace();
            }

            std::size_t effectiveWorkerCount() const
            {
                const std::size_t requested =
                    cfg_.workers > 0
                        ? static_cast<std::size_t>(cfg_.workers)
                        : static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency()));
                return std::min(requested, tree_slot_count_);
            }

            // 锁内调用。只在新计数落定后检查，避免把诊断写成行为。
            void assertPoolInvariantsLocked() const
            {
#ifndef NDEBUG
                assert(window_.delivered() <= window_.finished());
                assert(window_.finished() <= window_.started());
                assert(window_.started() <= window_.total());
                assert(window_.started() - window_.delivered() <= window_.capacity());
#endif
            }

            bool canStartSlotLocked() const
            {
                return !idle_.empty() && !error_ && !stop_ && window_.canStart();
            }

            void initializeIdleSlots()
            {
                idle_.reserve(tree_slot_count_);
                for (std::size_t i = tree_slot_count_; i > 0; --i)
                {
                    idle_.push_back(i - 1);
                }
            }

            void initializeWorkerWaiters()
            {
                worker_waiters_.reserve(worker_count_);
                for (std::size_t i = 0; i < worker_count_; ++i)
                {
                    worker_waiters_.push_back(std::make_unique<WorkerWaiter>());
                }
            }

            void configureForwardService()
            {
                forward_.configure({
                    .on_batch_ready = [this](const std::vector<std::size_t> &slots)
                    { onForwardBatchReady(slots); },
                    .on_fail = [this](std::exception_ptr error)
                    { fail(error); },
                });
            }

            // ── 任务准入与补场 ────────────────────────────────────────────────
            std::vector<StartClaim> claimStartSlots()
            {
                std::vector<StartClaim> claims;
                {
                    std::lock_guard<std::mutex> lock(pool_mutex_);
                    while (canStartSlotLocked())
                    {
                        // 锁内预留树槽，锁外造局，避免完成线程与交付线程领到同一槽。
                        const std::optional<TaskWindow::Claim> claim = window_.claim();
                        if (!claim.has_value())
                        {
                            break;
                        }
                        claims.push_back(StartClaim{idle_.back(), claim->task});
                        idle_.pop_back();
                    }
                    assertPoolInvariantsLocked();
                }
                return claims;
            }

            void topUpPool()
            {
                for (const StartClaim &claim : claimStartSlots())
                {
                    launchGame(claim.slot, claim.task);
                }
            }

            void launchGame(std::size_t slot, std::size_t task)
            {
                Slot &s = slots_[slot];
                BattleContext battle = make_task_(task);
                s.tree.emplace(task, std::move(battle));
                if (s.tree->real.outcome != Outcome::UNDECIDED)
                {
                    s.tree.reset();
                    throw std::runtime_error("selfplay 收到已结束的初始战斗");
                }
                resetTree(*s.tree, cfg_);

                const int simulations = nextSims(*s.tree, cfg_);
                {
                    std::lock_guard<std::mutex> lock(pool_mutex_);
                    s.state = TreeState{simulations, false};
                    ++live_tree_count_;
                    stats_.observeLive(live_tree_count_.load());
                    if (!error_ && !stop_)
                    {
                        ready_.push(slot);
                    }
                    assertPoolInvariantsLocked();
                }
                wakeWorkers(1);
            }

            void finishGame(std::size_t slot)
            {
                {
                    std::lock_guard<std::mutex> lock(pool_mutex_);
                    Tree &tree = *slots_[slot].tree;
                    const std::size_t task = tree.gid;
                    detail::reportLongGame(tree);
                    tree.out.task = task;
                    completed_.push_back(std::move(tree.out));
                    slots_[slot].tree.reset();
                    idle_.push_back(slot);
                    window_.markFinished();
                    --live_tree_count_;
                    lag_high_water_ = std::max(lag_high_water_, window_.lag());
                    assertPoolInvariantsLocked();
                }
                delivery_cv_.notify_one();
                topUpPool();
            }

            // ── 前向回调与失败 ────────────────────────────────────────────────
            void onForwardBatchReady(const std::vector<std::size_t> &slots)
            {
                {
                    std::lock_guard<std::mutex> lock(pool_mutex_);
                    ready_.push(slots);
                }
                wakeWorkers(slots.size());
            }

            void wakeWorkers(std::size_t count)
            {
                std::lock_guard<std::mutex> lock(pool_mutex_);
                wakeSleepingWorkersLocked(count);
            }

            void wakeSleepingWorkersLocked(std::size_t count)
            {
                for (const auto &waiter : worker_waiters_)
                {
                    if (count == 0)
                    {
                        break;
                    }
                    if (!waiter->sleeping)
                    {
                        continue;
                    }
                    waiter->cv.notify_one();
                    --count;
                }
            }

            void fail(std::exception_ptr error)
            {
                {
                    std::lock_guard<std::mutex> lock(pool_mutex_);
                    if (!error_)
                    {
                        error_ = error;
                    }
                    stop_ = true;
                }
                delivery_cv_.notify_one();
                wakeWorkers(worker_count_);
                forward_.requestStop();
            }

            // ── 单树推进 ──────────────────────────────────────────────────────
            StepResult advanceGame(std::size_t slot, NetEvaluator *local)
            {
                stats_.sampleStep(ready_.size(), live_tree_count_, forward_.pendingSize());
                Slot &s = slots_[slot];
                const StepResult result = stepper_.advance(*s.tree, s.state, s.query, slot, local);
                if (result == StepResult::Done)
                {
                    finishGame(slot);
                }
                return result;
            }

            // ── worker 池与唤醒 ──────────────────────────────────────────────
            enum class WorkKind
            {
                Tree,
                Stop
            };

            struct WorkItem
            {
                WorkKind kind = WorkKind::Stop;
                std::size_t slot = 0;
            };

            // 锁内完成“等活 → 选树 / 选窄批 / 退出”。两种唤醒原因各自返回一种 WorkKind。
            WorkItem acquireWork(WorkerWaiter &waiter)
            {
                std::unique_lock<std::mutex> lock(pool_mutex_);
                const auto wait_start = std::chrono::steady_clock::now();
                while (ready_.empty() && !error_ && !stop_)
                {
                    waiter.sleeping = true;
                    waiter.cv.wait(lock);
                    waiter.sleeping = false;
                }
                stats_.sampleWait(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_start).count());

                // 尾段/长树阻塞：已交付过至少一场，且活跃树 ≤ worker 数。
                // 不要求 all_tasks_claimed：中途一局长树把交付/补场卡住时，
                // live 也会掉到 n 附近，这时就该让 worker 自消化前向，而不是等全表领完。
                // 这里每次 acquireWork 都重新求值（不 latch）：交付恢复、补场把 live 拉回 >n
                // 后，自动回到共享 pending / GPU 攒批。
                static const bool disable_tail = std::getenv("STS_DISABLE_TAIL") != nullptr;
                tail_mode_.store(!disable_tail && window_.delivered() > 0 &&
                                     live_tree_count_.load(std::memory_order_relaxed) <= worker_count_,
                                 std::memory_order_relaxed);

                if (stop_)
                {
                    return WorkItem{WorkKind::Stop};
                }
                stats_.observeReady(ready_.size());
                if (ready_.empty())
                {
                    return WorkItem{WorkKind::Stop};
                }

                const std::optional<std::size_t> slot = ready_.pop();
                if (!slot.has_value())
                {
                    throw std::logic_error("ready 队列在池锁内为空");
                }
                return WorkItem{WorkKind::Tree, *slot};
            }

            void workerLoop(std::size_t worker_id)
            {
                std::unique_ptr<NetEvaluator> cpu_evaluator;
                WorkerWaiter &waiter = *worker_waiters_[worker_id];
                try
                {
                    while (true)
                    {
                        const WorkItem work = acquireWork(waiter);
                        if (work.kind == WorkKind::Stop)
                        {
                            return;
                        }

                        const bool tail = tail_mode_.load(std::memory_order_relaxed);
                        if (tail)
                        {
                            forward_.ensureCpuEvaluator(cpu_evaluator);
                        }
                        NetEvaluator *local = tail ? cpu_evaluator.get() : nullptr;
                        while (!stop_.load(std::memory_order_relaxed) &&
                               advanceGame(work.slot, local) == StepResult::Ready)
                        {
                        }
                    }
                }
                catch (...)
                {
                    fail(std::current_exception());
                }
            }

            void startWorkers()
            {
                fleet_.threads.reserve(worker_count_);
                for (std::size_t i = 0; i < worker_count_; ++i)
                {
                    fleet_.threads.emplace_back([this, i]
                                                { workerLoop(i); });
                }
            }

            // ── 交付 ──────────────────────────────────────────────────────────
            void deliverResults()
            {
                try
                {
                    while (true)
                    {
                        GameOut game;
                        std::size_t index = 0;
                        {
                            std::unique_lock<std::mutex> lock(pool_mutex_);
                            delivery_cv_.wait(lock, [&]
                                              { return error_ ||
                                                       window_.delivered() >= window_.total() ||
                                                       !completed_.empty(); });
                            if (error_ || window_.delivered() >= window_.total())
                            {
                                return;
                            }

                            index = window_.delivered();
                            game = std::move(completed_.front());
                            completed_.pop_front();
                            window_.markDelivered();
                            assertPoolInvariantsLocked();
                        }
                        emit_(index, std::move(game));
                        // 交付窗口恢复后重试之前因环满而暂停的空闲槽。
                        topUpPool();
                    }
                }
                catch (...)
                {
                    fail(std::current_exception());
                }
            }

            // ── 统计与收尾 ────────────────────────────────────────────────────
            void printStats() const
            {
                stats_.print(forward_, tree_slot_count_, window_.capacity(), lag_high_water_);
            }

            void rethrowFailure() const
            {
                if (error_)
                {
                    std::rethrow_exception(error_);
                }
            }

            // ── 数据成员 ──────────────────────────────────────────────────────
            // 声明顺序 = 初始化顺序 = 析构逆序，不是排版偏好：
            //   · forward_ 必须先于 stepper_ / fleet_；
            //   · fleet_ 必须最后（析构时 join 线程并访问 forward_ / waiters）。
            const TaskFactory &make_task_;
            const SelfplayConfig &cfg_;
            const GameSink &emit_;
            NetEvaluator eval_;
            const std::size_t tree_slot_count_;

            // 槽由当前 worker 独占；槽的分配、ready/idle 和交付环由 pool_mutex_ 保护。
            std::vector<Slot> slots_;
            std::deque<GameOut> completed_; // 完成即入队，按完成序写
            TaskWindow window_;
            std::mutex pool_mutex_;
            std::condition_variable delivery_cv_;
            std::atomic<std::size_t> live_tree_count_{0};

            std::atomic<bool> tail_mode_{false}; // 尾段：活跃树 ≤ worker，worker 自消化前向
            ForwardService forward_;
            TreeStepper stepper_;
            std::exception_ptr error_;

            const std::size_t worker_count_;
            std::vector<std::size_t> idle_;
            ReadyQueue ready_;
            std::vector<std::unique_ptr<WorkerWaiter>> worker_waiters_;
            std::atomic<bool> stop_{false};

            RunStats stats_;
            std::size_t lag_high_water_ = 0;

            // 必须最后声明：先停并等待所有线程，再销毁请求、回调和共享状态。
            WorkerFleet fleet_;
        };

    }

    void selfplayPooled(std::size_t n_task, std::size_t pool, const TaskFactory &make_task,
                        const SelfplayConfig &cfg, const GameSink &emit)
    {
        if (n_task == 0)
        {
            return;
        }
        validateSelfplayConfig(cfg);
        PooledSelfplayRunner runner(n_task, pool, make_task, cfg, emit);
        runner.run();
    }

    void selfplay(const std::vector<BattleContext> &battles, const SelfplayConfig &cfg,
                  const GameSink &emit)
    {
        const std::size_t K = battles.size();

        const auto makeTask = [&](std::size_t j)
        { return battles[j]; };
        selfplayPooled(K, K, makeTask, cfg, emit);
    }

    std::vector<std::string> selfplayRun(int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed, const std::string &out_dir, int shard_rows, bool hard_only)
    {
        if (n_per < 1)
        {
            throw std::runtime_error("n_per 必须 ≥ 1");
        }
        // shard_rows 由 `ShardSink` 拦。
        std::vector<EncJob> jobs = encJobs(n_per, start_seed);
        if (hard_only)
        {
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                      [](const EncJob &j)
                                      { return !isHardStep(j.step); }),
                       jobs.end());
        }
        ShardSink sink(out_dir, shard_rows);

        printLaunchOrder(jobs);

        DeliveryProgress progress(jobs.size());
        // 领到任务号时才造局（buildBattle 是纯函数；不预造整张表）。
        const auto makeTask = [&](std::size_t j)
        { return taskForJob(jobs[j]); };

        const auto emitGame = [&](std::size_t, GameOut &&g)
        {
            sink.addGame(g);
            progress.tick();
        };

        selfplayPooled(jobs.size(), static_cast<std::size_t>(std::max(1, cfg.pool)), makeTask, cfg, emitGame);
        return sink.finish();
    }

}
