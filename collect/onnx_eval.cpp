//
// `onnx_eval.h` 的实现。⚠️ ORT 的头只在这个 TU 里出现（pimpl：别让它们漏进公共调用面）。
//
// 契约与旧的 TorchScript 实现（`torch_eval.*`，已删）逐条对应：
//   · `eval` 返回的指针只在**下一次 `eval` 之前**有效（复用内部缓冲）
//   · 头宽度：元数据读不到就等**首次前向**按实际输出钉住
//   · `allow_tf32` 默认 false（与 torch 后端的 matmul 默认一致）
//   · 三段诊断耗时 `t_h2d` / `t_fwd` / `t_d2h` 同口径
//   · 输入：把调用方那块 `(n, dim)` float32 host 内存**包成 OrtValue**（CPU 档零拷贝；CUDA 档
//     ORT 自己在 `Run` 里上车）。不建"设备上的复用输入缓冲"—— 那要自己按设备分配 + 管生命周期，
//     收益只在 CUDA 档而且很小（实测 H2D 只占全程 6.5%）。要做就做 IOBinding（见下面 ⚠️）。
//   · 输出：`Run` 返回的三个 OrtValue **留着**（`outs_`），裸指针指向它们的内存 ⇒ 下一次调用
//     之前一直有效（契约见 `net_eval.h`）。⚠️ 别把它们拷进 `std::vector<float>`：CPU 档下
//     那是白拷一遍（ORT 的输出本来就在 host 上），CUDA 档下 ORT 也已经把结果落回 host。
//   · 线程：`intra_op_num_threads = intra_threads`（生产 = 1，见头文件里那个 25× 的坑）。
//
// ⚠️ **没做 IOBinding**：所以 CUDA 档下 ORT 内部的 H2D/D2H 混在 `t_fwd` 里（拿不到三段分解）。
//    要做的话：给输入/输出各建一个设备上的 OrtValue（内存要用 `Ort::Allocator(sess, dev_mi)` 分配，
//    别拿设备 MemoryInfo 去包 host 内存 —— 那是未定义行为），然后 `RunWithBinding`。
//
#include "onnx_eval.h"

#include <chrono>
#include <unordered_map>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <vector>

#include <onnxruntime_cxx_api.h>

namespace sts::collect
{
    namespace
    {
        /// `cuda[:k]` → 卡号 + 是否要 CUDA EP。
        bool wantsCuda(const std::string &device, int &dev_id)
        {
            const std::string d = device;
            const bool cuda = d.rfind("cuda", 0) == 0;
            dev_id = 0;
            if (cuda && d.size() > 5 && d[4] == ':')
            {
                dev_id = std::stoi(d.substr(5));
            }
            return cuda;
        }

        /// 进程级 ORT 环境：Session 可以多开，Env 只该有一个（ORT 的推荐用法）。
        /// 每个 `OnnxEvaluator` 再建一个 Env 会多出独立的全局线程池/日志状态。
        Ort::Env &sharedOrtEnv()
        {
            static Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "sts"};
            return env;
        }

        /// Windows 的 `Ort::Session` 要 `wchar_t` 路径（`ORTCHAR_T`）。
        std::basic_string<ORTCHAR_T> toOrtPath(const std::string &p)
        {
#ifdef _WIN32
            return std::filesystem::path(p).wstring();
#else
            return p;
#endif
        }
    } // namespace

    struct OnnxEvaluator::Impl
    {
        Ort::Session session{nullptr};
        std::vector<std::string> in_names_s, out_names_s;
        std::vector<const char *> in_names, out_names;
        Ort::MemoryInfo cpu_mi{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
        std::int64_t obs_dim = 0;        // 模型声明的输入宽度（与 `dim` 对账）
        std::vector<Ort::Value> outs;    // 上一次的输出（裸指针指向它 ⇒ 活到下一次调用）
        std::vector<Ort::Value> in_hold; // 输入值（`Run` 期间必须活着）
    };

    OnnxEvaluator::OnnxEvaluator(const std::string &path, const std::string &device, int intra_threads,
                                 bool allow_tf32)
        : impl_(std::make_unique<Impl>())
    {
        Impl &m = *impl_;
        // ⚠️ 必须先建 Env：CUDA EP 注册会取默认 logger，而 logger 是 Env 构造时注册的。
        Ort::Env &env = sharedOrtEnv();
        Ort::SessionOptions so;
        // ⚠️ 与 torch 侧同一条教训：默认按 `nproc` 起线程池，而 cgroup 常远小于 `nproc`
        //    ⇒ 小批前向下线程池同步开销压倒计算（实测 25×）。生产值 = 1。
        so.SetIntraOpNumThreads(std::max(1, intra_threads));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL); // 单服务线程 ⇒ 让 ORT 别自己并行分区
        so.SetLogSeverityLevel(3);
        int dev_id = 0;
        const bool cuda = wantsCuda(device, dev_id);
        if (cuda)
        {
            // ⚠️ 只能走 **`CUDAProviderOptions`（V2 通道）**，不能用通用
            //    `AppendExecutionProvider("CUDA", {..})` —— 那个接口在 1.31 里只认"插件式"EP，
            //    传 "CUDA" 会报 `Unknown provider name 'CUDA'`（实测）。而 TF32 只能从这个
            //    字符串选项集里关（`use_tf32`，ORT 的 CUDA EP 默认是 **1** ⇒ 与 torch(FP32) 差 ~5e-3）。
            // ⚠️ 拿不到 CUDA EP 就**当场失败**：静默落到 CPU 会让"一轮采集慢 3 倍"变成一个查不出
            //    原因的谜（本仓已经踩过一次"以为在 GPU 上跑"）。要 CPU 就显式传 device="cpu"。
            try
            {
                Ort::CUDAProviderOptions cuda_opts;
                cuda_opts.Update({{"device_id", std::to_string(dev_id)},
                                  {"use_tf32", allow_tf32 ? "1" : "0"}});
                // ⚠️ `Ort::CUDAProviderOptions` 是 `Base<OrtCUDAProviderOptionsV2>`（持有指针的包装）
                //    ⇒ 解引用拿底下的 C 对象（直接传包装体会报"不完整类型转换"）。
                so.AppendExecutionProvider_CUDA_V2(*cuda_opts);
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error(
                    std::string("要 CUDA 执行提供者但注册失败（换 GPU 包 / 装 cuDNN？）：") + e.what());
            }
        }
        // ⚠️ 不走 `std::ifstream` 自己读（torch 时代那条路是绕 Windows 的路径 bug）；ORT 的
        //    `Session(env, path, so)` 自己处理路径，Windows 上要 `wchar_t`。
        try
        {
            m.session = Ort::Session(env, toOrtPath(path).c_str(), so);
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error("载入 ONNX 存档失败：" + path + " —— " + e.what());
        }

        Ort::AllocatorWithDefaultOptions alloc;
        for (std::size_t i = 0; i < m.session.GetInputCount(); ++i)
        {
            m.in_names_s.emplace_back(m.session.GetInputNameAllocated(i, alloc).get());
        }
        for (std::size_t i = 0; i < m.session.GetOutputCount(); ++i)
        {
            m.out_names_s.emplace_back(m.session.GetOutputNameAllocated(i, alloc).get());
        }
        for (auto &s : m.in_names_s) { m.in_names.push_back(s.c_str()); }
        for (auto &s : m.out_names_s) { m.out_names.push_back(s.c_str()); }
        if (m.out_names.size() != 3)
        {
            throw std::runtime_error("ONNX 存档的输出不是 3 项（`onnxnet.py` 导的是 policy/select/value）");
        }
        // 头宽度**优先从模型元数据读**；读不到（导出图里那一维是动态的 —— 实测 `factor` 策略头就会
        // 出现 `-1`）就等**首次前向后按实际输出**钉住（与 torch 侧同契约）。批维是动态的 ⇒ 只取最后一维。
        const auto pa_info = m.session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto ps_info = m.session.GetOutputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape();
        if (pa_info.size() >= 2 && pa_info.back() > 0) { p_act_dim = static_cast<int>(pa_info.back()); }
        if (ps_info.size() >= 2 && ps_info.back() > 0) { p_sel_dim = static_cast<int>(ps_info.back()); }
        const auto in_info = m.session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (in_info.size() == 2 && in_info.back() > 0) { m.obs_dim = in_info.back(); }
        std::fprintf(stderr, "  ONNX 会话就绪：%s（头 %d/%d%s，设备 %s，intra %d）\n", path.c_str(),
                     p_act_dim, p_sel_dim, (p_act_dim > 0 && p_sel_dim > 0) ? "" : " 待首次前向",
                     cuda ? "cuda" : "cpu", std::max(1, intra_threads));
    }

    OnnxEvaluator::~OnnxEvaluator() = default;

    OnnxOut OnnxEvaluator::eval(const float *obs, std::int64_t n, std::int64_t dim)
    {
        using clock = std::chrono::steady_clock;
        Impl &m = *impl_;
        if (m.obs_dim > 0 && dim != m.obs_dim)
        {
            throw std::runtime_error("观测宽度与存档不符（存档 " + std::to_string(m.obs_dim) +
                                     " vs 调用 " + std::to_string(dim) + "）—— 布局改过？");
        }
        const auto m0 = clock::now();
        const std::array<std::int64_t, 2> shape{n, dim};
        m.in_hold.clear();
        // ⚠️ `CreateTensor` **不拷**：它包住这块内存（CPU 档零拷贝）。调用方负责让 `obs` 活到
        //    本函数返回（`Run` 是同步的 ⇒ 返回即安全）。
        m.in_hold.push_back(Ort::Value::CreateTensor<float>(
            m.cpu_mi, const_cast<float *>(obs), static_cast<std::size_t>(n * dim), shape.data(),
            shape.size()));
        const auto m1 = clock::now();
        m.outs = m.session.Run(Ort::RunOptions{nullptr}, m.in_names.data(), m.in_hold.data(),
                               m.in_hold.size(), m.out_names.data(), m.out_names.size());
        const auto m2 = clock::now();
        if (m.outs.size() != 3) { throw std::runtime_error("ORT 返回的输出不是 3 项"); }
        OnnxOut o;
        o.pa = m.outs[0].GetTensorData<float>();
        o.ps = m.outs[1].GetTensorData<float>();
        o.val = m.outs[2].GetTensorData<float>();
        // 头宽度：元数据读不到（那两维是动态的）就在**这里**钉住 —— 契约与 torch 侧同
        // （"首次前向后定义"），`NNEval::start()` 会读这两个值。
        const auto pa_shape = m.outs[0].GetTensorTypeAndShapeInfo().GetShape();
        const auto ps_shape = m.outs[1].GetTensorTypeAndShapeInfo().GetShape();
        if (p_act_dim <= 0)
        {
            p_act_dim = static_cast<int>(pa_shape.back());
        }
        else if (static_cast<int>(pa_shape.back()) != p_act_dim)
        {
            throw std::runtime_error("存档的动作头宽度变了（换错存档？）");
        }
        if (p_sel_dim <= 0)
        {
            p_sel_dim = static_cast<int>(ps_shape.back());
        }
        else if (static_cast<int>(ps_shape.back()) != p_sel_dim)
        {
            throw std::runtime_error("存档的选牌头宽度变了（换错存档？）");
        }
        const auto m3 = clock::now();
        auto add = [](std::atomic<double> &a, clock::time_point b, clock::time_point c)
        {
            a.store(a.load(std::memory_order_relaxed) + std::chrono::duration<double>(c - b).count(),
                    std::memory_order_relaxed);
        };
        add(t_h2d, m0, m1);
        add(t_fwd, m1, m2);
        add(t_d2h, m2, m3);
        return o;
    }

} // namespace sts::py
