#ifndef STS_LIGHTSPEED_SELFPLAY_POOL_H
#define STS_LIGHTSPEED_SELFPLAY_POOL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "collect.h" // GameOut

namespace sts::collect::detail
{
    // 三个计数器的唯一入口：started / finished / delivered。
    // 所有方法都在池锁内调用；`canStart()` 同时编码“任务没领完”和“交付窗口未满”。
    class TaskWindow
    {
      public:
        struct Claim
        {
            std::size_t task = 0;
            bool last = false; // 这一领是否让 started 达到 total
        };

        TaskWindow(std::size_t total, std::size_t capacity) : total_(total), capacity_(capacity) {}

        bool canStart() const
        {
            return started_ < total_ && started_ - delivered_ < capacity_;
        }

        std::optional<Claim> claim()
        {
            if (!canStart())
            {
                return std::nullopt;
            }
            const std::size_t task = started_++;
            return Claim{task, started_ == total_};
        }

        void markFinished() { ++finished_; }
        void markDelivered() { ++delivered_; }

        std::size_t total() const { return total_; }
        std::size_t capacity() const { return capacity_; }
        std::size_t started() const { return started_; }
        std::size_t finished() const { return finished_; }
        std::size_t delivered() const { return delivered_; }
        std::size_t lag() const { return finished_ - delivered_; }

      private:
        std::size_t total_ = 0;
        std::size_t capacity_ = 0;
        std::size_t started_ = 0;
        std::size_t finished_ = 0;
        std::size_t delivered_ = 0;
    };

    // 完成任务进入 `PooledSelfplayRunner::completed_`，按完成序直接写；不再有按任务号等待的交付环。

    // ready 队列 + 原子长度镜像。push/pop/empty/reverseInPlace 在池锁内调用；
    // size() 可在锁外读（ForwardService 的 wait_more 回调）。
    class ReadyQueue
    {
      public:
        void push(std::size_t slot)
        {
            slots_.push_back(slot);
            size_.store(slots_.size(), std::memory_order_relaxed);
        }

        void push(const std::vector<std::size_t> &slots)
        {
            slots_.insert(slots_.end(), slots.begin(), slots.end());
            size_.store(slots_.size(), std::memory_order_relaxed);
        }

        std::optional<std::size_t> pop()
        {
            if (slots_.empty())
            {
                return std::nullopt;
            }
            const std::size_t slot = slots_.back();
            slots_.pop_back();
            size_.store(slots_.size(), std::memory_order_relaxed);
            return slot;
        }

        bool empty() const { return slots_.empty(); }
        std::size_t size() const { return size_.load(std::memory_order_relaxed); }

        // 只在 worker 启动前调用：首轮按任务号升序出队。
        void reverseInPlace() { std::reverse(slots_.begin(), slots_.end()); }

      private:
        std::vector<std::size_t> slots_;
        std::atomic<std::size_t> size_{0};
    };
} // namespace sts::collect::detail

#endif // STS_LIGHTSPEED_SELFPLAY_POOL_H
