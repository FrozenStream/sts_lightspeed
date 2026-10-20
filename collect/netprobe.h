// 前向会话的**建立校验**（唯一实现）：空观测跑一次 + 两个策略头宽度钉到动作空间。
//
// 三处调用：自对弈的主会话（`selfplay.cpp`）、尾段分流用的 CPU 会话（`selfplay_forward.cpp`）、
// 评估的裸网络（`eval.cpp`）。宽度只在这里比一次 —— 下游直接取 `p_act_dim` / `p_sel_dim`。
// ⚠️ 首次前向必须在**主线程**跑过：头宽 / 执行计划 / CUDA 上下文都是那时才建立的。
#ifndef STS_LIGHTSPEED_NETPROBE_H
#define STS_LIGHTSPEED_NETPROBE_H

#include <cstddef>
#include <stdexcept>
#include <vector>

#include "net_eval.h" // NetEvaluator（bindings，唯一调用面）
#include "shardio.h"  // kPolicyDim / kSelectDim（动作空间宽度）
#include "snapshot.h" // Snap::snapshotLen

namespace sts::collect
{
    using sts::py::NetEvaluator;

    // 空观测跑一次。**不查宽度** —— 只有"两个会话互相比宽度"时才需要它。
    inline void probeForward(NetEvaluator &ev)
    {
        const std::vector<float> probe(static_cast<std::size_t>(Snap::snapshotLen), 0.0f);
        ev.eval(probe.data(), 1, Snap::snapshotLen);
    }

    // 空观测跑一次 + 头宽必须等于动作空间（存档与 `azlayout` 漂了当场抛）。
    inline void probeForwardChecked(NetEvaluator &ev)
    {
        probeForward(ev);
        if (ev.p_act_dim != kPolicyDim || ev.p_sel_dim != kSelectDim)
        {
            throw std::runtime_error("网络策略头宽度与动作空间不一致");
        }
    }

} // namespace sts::collect

#endif // STS_LIGHTSPEED_NETPROBE_H
