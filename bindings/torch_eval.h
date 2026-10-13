//
// TorchScript 推理器：把 `aznet.AZNet` 的 trace 存档载进 C++，让搜索侧直接调网络。
//
// ⚠️ 存档必须由 Python 侧**现 trace**（`torch.jit.trace(AZNet, (8, OBS_DIM))`）：网络定义只有
// Python 一份，权重换了重 trace 一次 —— 不在训练环里加新产物，也不让 C++ 侧复制定义。
// 观测布局**不动**：仍在宿主 TU（`Snap`），本文件只吃 `(n, 观测宽度)` 的 float32。
//
// 没编 Torch（`STS_WITH_TORCH` 未定义）时退化成"构造即抛"的桩：C++ 树照旧可用（网络由
// Python 批量前向，见 `az_search_new(..., ts_path=None)`）。
//
#ifndef STS_LIGHTSPEED_TORCH_EVAL_H
#define STS_LIGHTSPEED_TORCH_EVAL_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <torch/script.h>

namespace sts::py {

struct TorchEvaluator
{
    torch::jit::Module module;
    torch::Device device_{torch::kCPU};
    int p_act_dim = 0, p_sel_dim = 0;   // 首次前向后钉住（防存档与 `azlayout` 漂了）

    // 诊断：三段累计秒数（服务线程写、主线程结束后读）。
    std::atomic<double> t_h2d{0.0}, t_fwd{0.0}, t_d2h{0.0};

    // 设备上的输入缓冲（按需增长）：避免每次调用都新建张量 + 上车。
    torch::Tensor xbuf_;

    // `device` 空 或 `"cpu"` = CPU（原本的行为）。
    // ⚠️ 非 CPU 时**存档必须在那个设备上 trace**：`trace` 会把 buffer 当常量烤进图，
    //    CPU 上 trace 完再 `.to("cuda")` 搬不动那些常量 ⇒ 一调用就报"两个设备"。
    //
    // ⚠️ `intra_threads` **必须**落到 `torch::set_num_threads`：torch 默认按 `nproc` 起线程，
    //    而本机 cgroup 往往远小于 `nproc`（实测 128 vs 16）⇒ 默认就是 4~8 倍超订，
    //    小批（一行一次前向）下每算子的线程池同步开销远超计算本身：
    //    实测批 8 前向 74 ms → 5 ms，**快 25×**。与 Python 侧 `inference_net(intra_threads=1)` 同源。
    explicit TorchEvaluator(const std::string &path, const std::string &device = "",
                            int intra_threads = 1);

    // `(n, dim)` float32 行主序 → `(p_act, p_sel, val)`。⚠️ `obs` 只被**看**（from_blob），
    // 调用方要保证它在整次前向里活着。输入上车、**输出回 CPU**（调用方按 CPU 指针切片读）。
    std::vector<torch::Tensor> eval(const float *obs, std::int64_t n, std::int64_t dim);
};

} // namespace sts::py

#endif //STS_LIGHTSPEED_TORCH_EVAL_H
