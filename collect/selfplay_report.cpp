#include "selfplay_report.h"

#include <cstdio>

#include "encs.h" // monsterEncounterStrings
#include "selfplay_forward.h"
#include "selfplay_tree.h"

namespace sts::collect::detail
{
    namespace
    {
        constexpr int kLongGameSteps = 100;

        void updateHighWater(std::atomic<std::size_t> &high_water, std::size_t value)
        {
            std::size_t previous = high_water.load(std::memory_order_relaxed);
            while (value > previous &&
                   !high_water.compare_exchange_weak(previous, value, std::memory_order_relaxed))
            {
            }
        }
    } // namespace

    void RunStats::sampleStep(std::size_t ready, std::size_t live, std::size_t pending)
    {
        step_samples.fetch_add(1, std::memory_order_relaxed);
        sum_ready.fetch_add(ready, std::memory_order_relaxed);
        sum_live.fetch_add(live, std::memory_order_relaxed);
        sum_pending.fetch_add(pending, std::memory_order_relaxed);
    }

    void RunStats::sampleWait(double seconds)
    {
        wait_count.fetch_add(1, std::memory_order_relaxed);
        wait_time.fetch_add(seconds, std::memory_order_relaxed);
    }

    void RunStats::observeReady(std::size_t rows)
    {
        updateHighWater(ready_high_water, rows);
    }

    void RunStats::observeLive(std::size_t live)
    {
        updateHighWater(live_high_water, live);
    }

    void RunStats::print(const ForwardService &forward, std::size_t tree_slots,
                         std::size_t delivery_capacity, std::size_t lag_high_water) const
    {
        std::fprintf(stderr, "  前向服务：%s | worker 等活 %zu 次 %.2fs（队列最深 %zu）\n",
                     forward.stats().c_str(), wait_count.load(), wait_time.load(),
                     ready_high_water.load());
        const std::size_t samples = step_samples.load();
        if (samples == 0)
        {
            return;
        }

        const double n = static_cast<double>(samples);
        std::fprintf(stderr,
                     "  树分布（每次 step 采样）：均 在飞 %.1f / 可推 %.1f / 待服 %.1f"
                     "｜池宽 %zu 峰值在飞 %zu｜交付落后峰值 %zu（上限 %zu）\n",
                     static_cast<double>(sum_live.load()) / n -
                         static_cast<double>(sum_ready.load()) / n,
                     static_cast<double>(sum_ready.load()) / n,
                     static_cast<double>(sum_pending.load()) / n, tree_slots,
                     live_high_water.load(), lag_high_water, delivery_capacity);
    }

    void reportLongGame(const Tree &tree)
    {
        if (tree.steps < kLongGameSteps)
        {
            return;
        }
        std::fprintf(stderr, "  [长局] %s seed=%llu 步=%d 回合=%d %s\n",
                     monsterEncounterStrings[static_cast<int>(tree.real.encounter)],
                     static_cast<unsigned long long>(tree.real.seed), tree.steps,
                     tree.out.term.turn,
                     tree.real.outcome == Outcome::PLAYER_VICTORY ? "胜" : "未胜");
    }
} // namespace sts::collect::detail
