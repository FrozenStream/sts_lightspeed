// 推理引擎的**独立探针**：只做"载入存档 → 跑几批 → 报形状/读数/耗时（可选 dump 原始输出）"。
//
// 为什么单独一个目标（与 `selfplay-test` 分开）：
//   · 这一层（图 / 设备 / 线程数 / 输出宽度）出问题时，用它能**不经过搜索热循环**就定位
//     （把网络绑在搜索上验，等于让"热循环正在被改"挡住引擎侧的验证，也真的发生过）。
//   · `--dump` 写原始 float32 ⇒ 换 ORT 版本 / 换执行提供者时可以逐批对账
//     （同版本期望逐位；跨版本容差级，见 `.agent/notes.md` §3）。
//
// 用法：
//   net-test <存档.onnx> [n=64] [reps=20] [device=cpu] [--tf32] [--dump out.bin]
//   · `--dump` 写原始 float32（顺序：pa[n*96] | ps[n*128] | val[n*1]，小端）—— 跨版本 diff 用。
//   · `--tf32` 放开 TF32（ORT 的开关；默认关 ⇒ 真 FP32、与 torch 对账时可比）。
// ⚠️ 输入是**合成**的合法 obs（`(i*31 + j*17) % 3 * 0.5` ⇒ 取整后 ∈ {0,1}）：既让嵌入看见"非空槽"，
//    又完全确定（跨版本/跨设备吃的是同一批数）。
//
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "net_eval.h"
#include "snapshot.h" // Snap::snapshotLen（观测宽度）

#include "test_util.h"

namespace
{
    constexpr int kObsLen = sts::collect::Snap::snapshotLen;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "用法：net-test <存档.onnx> [n=64] [reps=20] [device=cpu] [--dump f.bin]\n");
        return 2;
    }
    const std::string model = argv[1];
    std::int64_t n = 64;
    int reps = 20;
    std::string device = "cpu";
    std::string dump;
    bool tf32 = false;
    int pos = 0;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
        {
            dump = argv[++i];
        }
        else if (std::strcmp(argv[i], "--tf32") == 0) { tf32 = true; }
        else if (pos == 0) { n = std::atoll(argv[i]); ++pos; }
        else if (pos == 1) { reps = std::atoi(argv[i]); ++pos; }
        else { device = argv[i]; }
    }
    if (n < 1) { testutil::fatal("n 至少 1"); }

    std::vector<float> obs(static_cast<std::size_t>(n) * kObsLen);
    for (std::int64_t i = 0; i < n; ++i)
    {
        for (int j = 0; j < kObsLen; ++j)
        {
            obs[static_cast<std::size_t>(i) * kObsLen + j] =
                static_cast<float>(((i * 31 + j * 17) % 3)) * 0.5f;
        }
    }

    sts::py::NetEvaluator ev(model, device, /*intra_threads=*/1, /*allow_tf32=*/tf32);
    // 契约：**首次前向必须在主线程跑过**（头宽度/上下文/执行计划那时才建立）—— 真实调用方
    // （`NNEval::start`）也要求这件事。
    ev.eval(obs.data(), n, kObsLen);
    if (ev.p_act_dim <= 0 || ev.p_sel_dim <= 0) { testutil::fatal("首次前向后头宽度仍未知"); }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    for (int r = 0; r < reps; ++r)
    {
        ev.eval(obs.data(), n, kObsLen);
    }
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count() / reps;
    const auto out = ev.eval(obs.data(), n, kObsLen);

    // 读数：三段的 sum / min / max + 第 0 行的 argmax（跨后端应当**逐项一致**，float 级差异除外）
    auto stats = [](const float *p, std::size_t cnt, const char *name)
    {
        double sum = 0.0;
        float mn = p[0], mx = p[0];
        for (std::size_t i = 0; i < cnt; ++i)
        {
            sum += p[i];
            mn = std::min(mn, p[i]);
            mx = std::max(mx, p[i]);
        }
        std::fprintf(stderr, "  %s: sum=%.6f min=%.6f max=%.6f\n", name, sum, mn, mx);
    };
    const auto da = static_cast<std::size_t>(ev.p_act_dim);
    const auto ds = static_cast<std::size_t>(ev.p_sel_dim);
    std::fprintf(stderr, "net-test：%s  n=%lld reps=%d device=%s tf32=%d 头 %d/%d  均 %.3f ms/次\n",
                 model.c_str(), static_cast<long long>(n), reps, device.c_str(), tf32 ? 1 : 0,
                 ev.p_act_dim, ev.p_sel_dim, ms);
    stats(out.pa, static_cast<std::size_t>(n) * da, "pa");
    stats(out.ps, static_cast<std::size_t>(n) * ds, "ps");
    stats(out.val, static_cast<std::size_t>(n), "val");
    std::int64_t best = 0;
    for (std::int64_t k = 1; k < da; ++k)
    {
        if (out.pa[k] > out.pa[best]) { best = k; }
    }
    std::fprintf(stderr, "  行 0 的 argmax(pa) = %lld（pa[0]=%.6f）\n", static_cast<long long>(best),
                 static_cast<double>(out.pa[0]));
    std::fprintf(stderr, "前向分段（本层计时）：H2D %.3fs 前向 %.3fs D2H %.3fs\n", ev.t_h2d.load(),
                 ev.t_fwd.load(), ev.t_d2h.load());

    if (!dump.empty())
    {
        FILE *f = std::fopen(dump.c_str(), "wb");
        if (!f) { testutil::fatal("写不了 " + dump); }
        std::fwrite(out.pa, sizeof(float), static_cast<std::size_t>(n) * da, f);
        std::fwrite(out.ps, sizeof(float), static_cast<std::size_t>(n) * ds, f);
        std::fwrite(out.val, sizeof(float), static_cast<std::size_t>(n), f);
        std::fclose(f);
        std::fprintf(stderr, "  已 dump → %s\n", dump.c_str());
    }
    return 0;
}
