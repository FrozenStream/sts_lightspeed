//
// 推理后端的**唯一调用面**：`sts::py::NetEvaluator`（= `onnx_eval.h` 的 `OnnxEvaluator`）。
//
// ⚠️ **只有一个后端**（2026-10-15）：libtorch/TorchScript 那一支（`torch_eval.*`）已删 —— C++ 侧
//    不再链 torch，进程里**不必**先 `import torch`（Windows 的加载器在 `PyInit_*` 之前就解析完
//    导入表，所以链 torch 的模块必须先让宿主把 `torch/lib` 加进 DLL 搜索路径）。
//
// 契约：
//   · `NetEvaluator(path, device, intra_threads)`：`device` 空/"cpu" = CPU，"cuda[:k]" = 卡上；
//     `intra_threads` 落成"算子内部线程数"（小批前向必须 = 1，见实现里的注释）。
//   · `eval(obs, n, dim)` → `{pa, ps, val}` 三个**host 指针**（`n×96` / `n×128` / `n×1`）。
//     ⚠️ 指针只在**下一次 `eval` 之前**有效（实现内部复用缓冲）⇒ 立刻读走，别存。
//   · `p_act_dim` / `p_sel_dim`：头宽度，首次前向后钉住（防"存档与 `azlayout` 漂了"）。
//   · `t_h2d` / `t_fwd` / `t_d2h`：三段累计耗时（诊断用；服务线程写、收尾读）。
//
// ⚠️ **首次前向必须在主线程跑过**再起服务线程：头宽度、CUDA 上下文、执行计划都是那时才建立的
//    （见 `collect/selfplay.cpp` 的 `NNEval`）。
//
#ifndef STS_LIGHTSPEED_NET_EVAL_H
#define STS_LIGHTSPEED_NET_EVAL_H

#include <atomic>
#include <cstdint>
#include <string>

#include "onnx_eval.h"

namespace sts::py
{
    using NetEvaluator = OnnxEvaluator;
}

#endif // STS_LIGHTSPEED_NET_EVAL_H
