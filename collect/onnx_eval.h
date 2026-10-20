#ifndef STS_LIGHTSPEED_ONNX_EVAL_H
#define STS_LIGHTSPEED_ONNX_EVAL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace sts::collect {

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
