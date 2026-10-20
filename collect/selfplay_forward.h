#ifndef STS_LIGHTSPEED_SELFPLAY_FORWARD_H
#define STS_LIGHTSPEED_SELFPLAY_FORWARD_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net_eval.h"
#include "selfplay.h"
#include "shardio.h" // kPolicyDim / kSelectDim（策略头宽度，netprobe 已钉死）

namespace sts::collect::detail
{
    using sts::py::NetEvaluator;

    // 前向队列使用独立锁；跨到树调度的回调一律在解锁后运行。
    class ForwardService
    {
    public:
        // 一次前向的往返：`slot`/`obs` 由提交方填，`pa`/`ps`/`val` 由服务填回。
        struct Query
        {
            std::array<float, Snap::snapshotLen> obs;
            std::array<float, kPolicyDim> pa;
            std::array<float, kSelectDim> ps;
            float val = 0.0f;
            std::size_t slot = 0;
        };

        struct Settings
        {
            std::function<void(const std::vector<std::size_t> &)> on_batch_ready;
            std::function<void(std::exception_ptr)> on_fail;
        };

        // `cpu_model_path` = 尾段（worker 就地前向）用的 CPU 存档，由调用方解析好
        // （`SelfplayConfig::model_path_cpu` 为空时传主存档）。
        ForwardService(NetEvaluator &eval, std::string cpu_model_path);
        ~ForwardService();

        // 仅在服务线程启动前配置，运行期间不修改。
        void configure(Settings settings);

        void start();

        void requestStop();
        void shutdown();

        ForwardService(const ForwardService &) = delete;
        ForwardService &operator=(const ForwardService &) = delete;

        void push(Query &q);

        std::size_t pendingSize() const;

        std::string stats() const;

        // tail_mode 用：worker 本地 CPU 会话（懒建，已建好则直接返回）。
        void ensureCpuEvaluator(std::unique_ptr<NetEvaluator> &ev);

    private:
        void notifyPending();

        // 返回 false 表示停服；否则 `batch` 为当时队列里的全部请求。
        bool takePendingBatch(std::vector<Query *> &batch);

        void evaluateBatch(const std::vector<Query *> &batch, std::vector<float> &buf);

        void notifyBatchDone(const std::vector<Query *> &batch, std::vector<std::size_t> &slots);

        void serveLoop();

        void countBatch(std::size_t rows);

        void serve();

        NetEvaluator &main_eval_;
        Settings settings_;
        const std::string cpu_model_path_;
        int p_act_dim_ = 0, p_sel_dim_ = 0;
        std::thread server_thread_;
        std::deque<Query *> pending_;

        std::atomic<std::size_t> pending_count_{0};

        std::atomic<std::size_t> batch_count_{0}, row_count_{0}, max_batch_rows_{0};
        std::atomic<double> time_gather_s_{0.0}, time_eval_s_{0.0}, time_scatter_s_{0.0};
        bool stop_ = false;
        mutable std::mutex m_;
        std::condition_variable cv_;
    };
} // namespace sts::collect::detail

#endif // STS_LIGHTSPEED_SELFPLAY_FORWARD_H
