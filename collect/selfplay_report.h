#ifndef STS_LIGHTSPEED_SELFPLAY_REPORT_H
#define STS_LIGHTSPEED_SELFPLAY_REPORT_H

#include <atomic>
#include <cstddef>

namespace sts::collect::detail
{
    class ForwardService;
    struct Tree;

    // 跨线程读的运行统计。采样点与打印分离：热循环只碰这几个原子量，格式化集中在 print()。
    struct RunStats
    {
        void sampleStep(std::size_t ready, std::size_t live, std::size_t pending);
        void sampleWait(double seconds);
        void observeReady(std::size_t rows);
        void observeLive(std::size_t live);

        void print(const ForwardService &forward, std::size_t tree_slots,
                   std::size_t delivery_capacity, std::size_t lag_high_water) const;

        std::atomic<std::size_t> wait_count{0};
        std::atomic<std::size_t> ready_high_water{0};
        std::atomic<double> wait_time{0.0};
        std::atomic<std::size_t> step_samples{0};
        std::atomic<std::size_t> sum_ready{0};
        std::atomic<std::size_t> sum_live{0};
        std::atomic<unsigned long long> sum_pending{0};
        std::atomic<std::size_t> live_high_water{0};
    };

    // 单场步数超过阈值的诊断行（长局是正常样本，不是废数据）。
    void reportLongGame(const Tree &tree);
} // namespace sts::collect::detail

#endif // STS_LIGHTSPEED_SELFPLAY_REPORT_H
