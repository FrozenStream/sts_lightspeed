#include "selfplay_forward.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "netprobe.h" // probeForward（会话的建立校验）
#include "snapshot.h" // Snap::snapshotLen

namespace sts::collect::detail
{
    ForwardService::ForwardService(NetEvaluator &eval, std::string cpu_model_path)
        : main_eval_(eval), cpu_model_path_(std::move(cpu_model_path))
    {
    }

    void ForwardService::configure(Settings settings)
    {
        settings_ = std::move(settings);
    }

    void ForwardService::start()
    {
        // 前置：会话已 `probeForwardChecked`（`selfplay.cpp` 的 `run()`）⇒ 宽度直接取，不再查一遍。
        p_act_dim_ = main_eval_.p_act_dim;
        p_sel_dim_ = main_eval_.p_sel_dim;
        server_thread_ = std::thread([this]
                                     { serve(); });
    }

    ForwardService::~ForwardService()
    {
        shutdown();
    }

    void ForwardService::requestStop()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_one();
    }

    void ForwardService::shutdown()
    {
        requestStop();
        if (server_thread_.joinable())
        {
            server_thread_.join();
        }
    }

    void ForwardService::push(Query &q)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_.push_back(&q);
            ++pending_count_;
        }
        notifyPending();
    }

    void ForwardService::notifyPending()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (stop_ || pending_.empty())
            {
                return;
            }
        }
        cv_.notify_one();
    }

    bool ForwardService::takePendingBatch(std::vector<Query *> &batch)
    {
        batch.clear();
        std::unique_lock<std::mutex> lk(m_);

        while (!stop_ && pending_.empty())
        {
            cv_.wait(lk);
        }
        if (stop_)
        {
            return false;
        }

        // 来一批发一批：批宽由“此刻到达的请求数”决定，不攒批。
        batch.assign(pending_.begin(), pending_.end());
        pending_.clear();
        pending_count_ -= batch.size();
        return true;
    }

    void ForwardService::evaluateBatch(const std::vector<Query *> &batch, std::vector<float> &buf)
    {
        countBatch(batch.size());
        buf.resize(batch.size() * static_cast<std::size_t>(Snap::snapshotLen));
        const auto d0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < batch.size(); ++i)
        {
            std::copy(batch[i]->obs.begin(), batch[i]->obs.end(),
                      buf.begin() + static_cast<std::ptrdiff_t>(i * Snap::snapshotLen));
        }
        const auto d1 = std::chrono::steady_clock::now();
        const auto fwd = main_eval_.eval(buf.data(), static_cast<std::int64_t>(batch.size()), Snap::snapshotLen);
        const auto d2 = std::chrono::steady_clock::now();
        const float *pa = fwd.pa;
        const float *ps = fwd.ps;
        const float *val = fwd.val;
        const auto da = static_cast<std::size_t>(p_act_dim_);
        const auto ds = static_cast<std::size_t>(p_sel_dim_);
        for (std::size_t i = 0; i < batch.size(); ++i)
        {
            Query &q = *batch[i];
            std::copy_n(pa + i * da, kPolicyDim, q.pa.begin());
            std::copy_n(ps + i * ds, kSelectDim, q.ps.begin());
            q.val = val[i];
        }
        const auto d3 = std::chrono::steady_clock::now();
        time_gather_s_.fetch_add(std::chrono::duration<double>(d1 - d0).count());
        time_eval_s_.fetch_add(std::chrono::duration<double>(d2 - d1).count());
        time_scatter_s_.fetch_add(std::chrono::duration<double>(d3 - d2).count());
    }

    void ForwardService::notifyBatchDone(const std::vector<Query *> &batch, std::vector<std::size_t> &slots)
    {
        if (settings_.on_batch_ready)
        {
            slots.clear();
            slots.reserve(batch.size());
            for (const Query *q : batch)
            {
                slots.push_back(q->slot);
            }
            settings_.on_batch_ready(slots);
        }
    }

    void ForwardService::serveLoop()
    {
        std::vector<Query *> batch;
        std::vector<std::size_t> slots;
        std::vector<float> buf;
        while (takePendingBatch(batch))
        {
            evaluateBatch(batch, buf);
            notifyBatchDone(batch, slots);
        }
    }

    void ForwardService::serve()
    {
        try
        {
            serveLoop();
        }
        catch (...)
        {
            if (settings_.on_fail)
            {
                settings_.on_fail(std::current_exception());
            }
        }
    }

    void ForwardService::countBatch(std::size_t rows)
    {
        batch_count_.fetch_add(1, std::memory_order_relaxed);
        row_count_.fetch_add(rows, std::memory_order_relaxed);
        std::size_t prev = max_batch_rows_.load(std::memory_order_relaxed);
        while (rows > prev && !max_batch_rows_.compare_exchange_weak(prev, rows, std::memory_order_relaxed))
        {
        }
    }

    std::size_t ForwardService::pendingSize() const
    {
        return pending_count_.load(std::memory_order_relaxed);
    }

    std::string ForwardService::stats() const
    {
        char b[400];
        const std::size_t nb = batch_count_.load(), nr = row_count_.load();
        std::snprintf(b, sizeof(b),
                      "批 %zu / 行 %zu（均 %.1f，最大 %zu）本层 gather %.2fs 调用 %.2fs scatter %.2fs"
                      " | ORT 内部 H2D %.2fs 前向 %.2fs D2H %.2fs",
                      nb, nr, nb ? static_cast<double>(nr) / static_cast<double>(nb) : 0.0, max_batch_rows_.load(),
                      time_gather_s_.load(), time_eval_s_.load(), time_scatter_s_.load(), main_eval_.t_h2d.load(),
                      main_eval_.t_fwd.load(), main_eval_.t_d2h.load());
        return b;
    }

    // CPU 尾段每次只有 1 行：让单个会话开多核，每个算子的线程池同步会压倒计算
    // （实测小批 `intra=nproc` 慢 25×）。这里每个 worker 懒建自己的 `intra=1` 会话，
    // 并行度来自“多个会话同时各算 1 行”，而不是一个会话内部拆算子。
    // ⚠️ `Ort::Env` 仍是进程级共享的（见 `onnx_eval.cpp` 的 `sharedOrtEnv`），每 worker 一份的只是 Session。

    // CPU 尾段每次只有 1 行：让单个会话开多核，每个算子的线程池同步会压倒计算
    // （实测小批 `intra=nproc` 慢 25×）。这里每个 worker 懒建自己的 `intra=1` 会话，
    // 并行度来自“多个会话同时各算 1 行”，而不是一个会话内部拆算子。
    // ⚠️ `Ort::Env` 仍是进程级共享的（见 `onnx_eval.cpp` 的 `sharedOrtEnv`），每 worker 一份的只是 Session。
    void ForwardService::ensureCpuEvaluator(std::unique_ptr<NetEvaluator> &ev)
    {
        if (ev)
        {
            return;
        }
        ev = std::make_unique<NetEvaluator>(cpu_model_path_, "cpu", 1);
        probeForward(*ev);
        if (ev->p_act_dim != p_act_dim_ || ev->p_sel_dim != p_sel_dim_)
        {
            throw std::runtime_error("CPU 会话与主会话头宽度不一致（act " +
                                     std::to_string(ev->p_act_dim) + "/" + std::to_string(p_act_dim_) +
                                     "、sel " + std::to_string(ev->p_sel_dim) + "/" +
                                     std::to_string(p_sel_dim_) + "），请检查 `model_path_cpu`");
        }
    }

} // namespace sts::collect::detail
