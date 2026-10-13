//
// `torch_eval.h` 的实现。⚠️ 本 TU 会链上 libtorch ⇒ 进程里**必须先 `import torch`**（Windows 上
// DLL 依赖在加载期解析；Python 的 torch 会把它的 `lib/` 加进 DLL 搜索路径）。
//
#include "torch_eval.h"

#include <ATen/Parallel.h>   // at::set_num_threads（`torch::set_num_threads` 在 torch 2.12 上不存在）
#include <torch/csrc/jit/runtime/graph_executor.h>   // torch::jit::setGraphExecutorOptimize

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace sts::py {

TorchEvaluator::TorchEvaluator(const std::string &path, const std::string &device,
                               int intra_threads)
{
    // 关掉 profiling executor：它按**输入形状**特化执行计划，而本项目批宽每轮都变
    // （每棵树每轮叶数 1..batch）⇒ 每遇新形状就重做一遍，CUDA 上单次 30 s 级
    // （实测 37.0 / 36.5 / 32.9 s）。代价：稳态单次 1.079 → 1.243 ms（+15%）。
    // ⚠️ 进程级开关，必须在**模块首次前向之前**落（执行计划惰性建立）。
    torch::jit::setGraphExecutorOptimize(false);
    // ⚠️ 再落线程数：默认按 `nproc` 起（本机报 128），而 cgroup 只有 16 核 ⇒ 超订，
    //    小批前向实测因此慢 25×。与 Python 侧 `inference_net(intra_threads=1)` 同源。
    if (intra_threads > 0) { at::set_num_threads(intra_threads); }
    try
    {
        // ⚠️ 不走 `torch::jit::load(路径)`：Windows 上那条路报 `errno 42 Illegal byte sequence`
        // （libtorch 自己处理路径时踩的坑）。自己用 ifstream（**二进制**）读进来，再走流式 load。
        std::ifstream in(path, std::ios::binary);
        if (!in) { throw std::runtime_error("打不开 TorchScript 存档：" + path); }
        module = torch::jit::load(in);
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error("载入 TorchScript 存档失败：" + path + " —— " + e.what());
    }
    module.eval();
    if (!device.empty() && device != "cpu")
    {
        device_ = torch::Device(device);
        // ⚠️ 图里的 buffer 常量不会跟着搬 —— 存档必须是在**同一设备**上 trace 的。
        module.to(device_);
    }
}

std::vector<torch::Tensor> TorchEvaluator::eval(const float *obs, std::int64_t n, std::int64_t dim)
{
    using clock = std::chrono::steady_clock;
    torch::NoGradGuard no_grad;   // 推理路径：关 autograd
    const auto m0 = clock::now();
    // 复用一块设备上的输入缓冲：每次调用只做一次 `copy_`，不新建张量。
    if (!xbuf_.defined() || xbuf_.size(0) < n || xbuf_.size(1) != dim)
    {
        xbuf_ = torch::empty({n, dim},
                             torch::TensorOptions().dtype(torch::kFloat32).device(device_));
    }
    auto xin = xbuf_.narrow(0, 0, n);
    xin.copy_(torch::from_blob(const_cast<float *>(obs), {n, dim}, torch::kFloat32));
    const auto m1 = clock::now();
    auto out = module.forward({xin}).toTuple()->elements();
    const auto m2 = clock::now();
    if (out.size() != 3) { throw std::runtime_error("TorchScript 存档输出不是 3 项"); }
    std::vector<torch::Tensor> ts;
    ts.reserve(3);
    for (auto &e : out) { ts.push_back(e.toTensor().to(torch::kCPU).contiguous()); }
    const auto m3 = clock::now();
    auto add = [](std::atomic<double> &a, clock::time_point b, clock::time_point c) {
        a.store(a.load(std::memory_order_relaxed) + std::chrono::duration<double>(c - b).count(),
                std::memory_order_relaxed);
    };
    add(t_h2d, m0, m1);
    add(t_fwd, m1, m2);
    add(t_d2h, m2, m3);
    if (p_act_dim == 0)
    {
        p_act_dim = static_cast<int>(ts[0].size(1));
        p_sel_dim = static_cast<int>(ts[1].size(1));
    }
    else if (static_cast<int>(ts[0].size(1)) != p_act_dim)
    {
        throw std::runtime_error("存档的动作头宽度变了（换错存档？）");
    }
    return {ts[0], ts[1], ts[2]};
}

} // namespace sts::py
