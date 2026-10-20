"""网络存档的**唯一**入口：`net_path` → C++ 能载的 `.onnx`（采集 / 门控 / 测试共用）。

C++ 侧的推理引擎只有一个：**ONNX Runtime**（`bindings/net_eval.h` → `onnx_eval.*`，CMake 里写死
`bindings/onnx_eval.cpp`）。libtorch/TorchScript 那一支（`torch_eval.*` + `tsnet.py`）已于 2026-10-15
删除 —— 于是这里不再有"按后端分派"这回事，只**转发**到 `onnxnet`。

⚠️ 为什么留这一层而不让调用方直接 `from onnxnet import ensure_net`：
  · 它是"网络怎么进 C++"的**单一收口**（将来若再加一个引擎，改这里一处）；
  · 顺手挡**旧扩展**：链 libtorch 的老 `.pyd`/`.so` 会报 `NET_BACKEND == "torch"` —— 那时拿
    `.onnx` 去喂它只会得到一句难懂的"图载入失败"，所以这里**当场拒绝**并说清怎么办。
"""
import slaythespire as sts

ONNX = "onnx"


def backend() -> str:
    """C++ 侧报的推理后端（**只可能是 `onnx`**；别的值 = 装了个旧扩展）。"""
    be = str(getattr(sts, "NET_BACKEND", "?"))
    if be != ONNX:
        raise RuntimeError(
            f"当前的 `slaythespire` 扩展报的推理后端是 {be!r}（期望 {ONNX!r}）—— "
            "它还是链 libtorch 的那份旧构建。请重编扩展（C++ 侧现在只认 ONNX Runtime，"
            "见 `CMakeLists.txt` 顶部那段），并确认 `import slaythespire` 拿到的是新产物。")
    return be


def model_path(net_path: str, device: str = "cpu") -> str:
    """`net_path` 在 `device` 上的存档路径（**唯一**拼名点的转发）。"""
    from onnxnet import onnx_path
    return onnx_path(net_path, device)


def ensure_net(net_path: str, device: str = "cpu") -> str:
    """没有就导一份（原子落盘）并返回路径 —— 采集/门控就用这个。"""
    backend()
    from onnxnet import ensure_onnx
    return ensure_onnx(net_path, device)