//
// ONNX Runtime 推理器：把 `onnxnet.py` 导出的 `.onnx` 载进 C++，让搜索侧直接调网络。**唯一引擎**。
//
// 为什么不链 libtorch/TorchScript：代价是**全链路**的 —— `.pyd` 静态链 `c10.dll`/`torch_cpu.dll`
// ⇒ 进程里必须先 `import torch`（Windows 加载器在 `PyInit_*` 之前就解析完导入表）、torch 自带一份
// pybind11 要和子模块抢、构建要 `TORCH_CMAKE_PREFIX`、Executable 还要 `Python.h`/libpython。
// ORT 这边只有**头 + 一个导入库**，torch 退回训练/导出那侧（旧实现 `torch_eval.*` 已于 2026-10-15 删）。
// ⚠️ 速度不是动机：实测（同权重、intra=1）batch≥64 两边打平、batch=1 ORT 快 3.8×。
// ⚠️ 数值**不可能逐位相同**（不同实现的归约顺序不同）：实测 max|Δ| ≈ 3e-7、两个头的 argmax
//    逐行一致 ⇒ 回归判据是"动作一致 + 容差"，见 `onnxnet.verify()`。
//
// 设备：`device` 空/"cpu" = CPU；`cuda[:k]` = CUDA 执行提供者（`k` = 卡号）。⚠️ 要真的用上 GPU，
// 链接时那份库必须是 **GPU 包**（`onnxruntime-*-gpu_cuda13`），且运行时能找到
// `onnxruntime_providers_cuda` + 一个匹配的 cuDNN —— 否则 `AppendExecutionProvider_CUDA` 会
// 落到 CPU 上（本文件的构造里对此**显式失败**，不静默降级；静默降级 = 一轮采集慢 3 倍还查不出原因）。
//
// ⚠️ 实现细节藏在 `Impl`（pimpl）：ORT 的头很重，而且**不要**漏进 `net_eval.h` 那条公共调用面。
//
#ifndef STS_LIGHTSPEED_ONNX_EVAL_H
#define STS_LIGHTSPEED_ONNX_EVAL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace sts::py {

/// `eval` 的返回：三段 host 指针（契约见 `net_eval.h`）。
struct OnnxOut
{
    const float *pa = nullptr;  // (n, 96)
    const float *ps = nullptr;  // (n, 128)
    const float *val = nullptr; // (n, 1)
};

class OnnxEvaluator
{
public:
    // ⚠️ 非 CPU 时 `device` 里的卡号会传给 CUDA EP；`intra_threads` 落成 ORT 的
    //    `intra_op_num_threads`（小批前向必须 = 1：默认按 `nproc` 起线程池，而 cgroup 常常
    //    远小于 `nproc` ⇒ 每算子的线程池同步开销压倒计算本身，实测那个坑让前向慢 25×）。
    // ⚠️ `allow_tf32` **默认 false**（与 torch 后端的 matmul 默认一致）：ORT 的 CUDA EP
    //    **默认开** TF32 ⇒ 与 torch(FP32) 差 ~5e-3（实测 4.29e-3 vs 关掉 3.8e-6），
    //    那会让"换后端"变成"换数值"。要拿 TF32 换速度（tensor core）就把这个打开、并接受
    //    与历史数据的不可比。
    explicit OnnxEvaluator(const std::string &path, const std::string &device = "",
                           int intra_threads = 1, bool allow_tf32 = false);
    ~OnnxEvaluator();
    OnnxEvaluator(const OnnxEvaluator &) = delete;
    OnnxEvaluator &operator=(const OnnxEvaluator &) = delete;

// `(n, dim)` float32 行主序 → `{pa, ps, val}`。`obs` 只被**看**（ORT 直接包住这块内存，
    // CPU 档下连拷贝都没有）⇒ 调用方要保证它在整次前向里活着。
    OnnxOut eval(const float *obs, std::int64_t n, std::int64_t dim);

    int p_act_dim = 0, p_sel_dim = 0; // 首次前向后钉住（防存档与 `azlayout` 漂了）
    // 诊断（契约与 torch 后端同）：`t_h2d` = 我们做的输入上车；`t_fwd` = `Run`（⚠️ CUDA 档下
    // **ORT 内部的 H2D/D2H 也在这一段里** —— 我们没做 IOBinding，所以那两段拿不到）；
    // `t_d2h` = 取形状/指针（≈0）。
    std::atomic<double> t_h2d{0.0}, t_fwd{0.0}, t_d2h{0.0};

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sts::py

#endif // STS_LIGHTSPEED_ONNX_EVAL_H
