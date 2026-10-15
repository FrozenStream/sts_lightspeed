"""权重 → TorchScript 存档（**唯一**拼名点 + trace）—— 采集与门控共用。

⚠️ 存档是**按设备**各存一份：图里的 buffer 常量不会跟着 `module.to()` 搬（见 `bindings/torch_eval.cpp`
的注释）⇒ CUDA 的存档必须在 CUDA 上 trace。同一份权重在两种设备上 = **两份文件、两套数值**。
⚠️ trace 前必须落 `_jit_set_profiling_executor(False)`：C++ 的 `TorchEvaluator` 构造里也落这一句，
两条路径必须是同一个执行器，否则数值对不上。
"""
import os

import torch  # noqa: F401  ⚠️ 必须在 `slaythespire` 之前（扩展静态链 libtorch）

import ckpt


def _tag(device: str) -> str:
    """设备 → 文件名后缀：`""` / `cpu` → `cpu`；`cuda` / `cuda:1` → `cuda` / `cuda1`。"""
    return (device or "cpu").replace(":", "")


def ts_path(net_path: str, device: str = "cpu") -> str:
    """`net_path` 在 `device` 上的 TorchScript 存档路径（**唯一**拼名点）。

    `data/selfplay_ts/` 是**派生目录**（不在 `data/` 的缓存命名体系里）：名字由**内容指纹 + 设备**
    决定 ⇒ 换权重/换设备自动换文件、重跑同一份权重直接命中。它是只读的（`torch.jit.load` 不写）
    ⇒ 全部 worker 共用一份，不必加锁。
    """
    h = str(ckpt.read(net_path)["fp"])
    stem = os.path.splitext(os.path.basename(net_path))[0].split("-")[0]
    return os.path.join("data", "selfplay_ts", f"{stem}-{h}-{_tag(device)}.ts")


def ensure_ts(net_path: str, device: str = "cpu") -> str:
    """没有就 trace 一份并**原子**落盘（先写 `.tmp` 再 `replace`），返回路径。

    网络定义只有 Python 一份（`aznet.AZNet`），C++ 侧只吃 trace 出来的图 ⇒ 这里不重复定义。
    """
    path = ts_path(net_path, device)
    if os.path.exists(path):
        return path
    from aznet import AZNet, OBS_DIM, net_kwargs

    dev = device or "cpu"
    cfg = ckpt.config_of(net_path)
    if not cfg:
        raise RuntimeError(f"{net_path} 存档里没有建网配置（用 `python ckpt.py {net_path}` 看）")
    net = AZNet(**net_kwargs(cfg)).to(dev)
    net.load_state_dict(ckpt.weights_of(net_path))
    net.eval()
    torch._C._jit_set_profiling_executor(False)
    with torch.no_grad():
        traced = torch.jit.trace(net, torch.zeros(8, OBS_DIM, device=dev))
    traced.eval()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.{os.getpid()}.tmp"
    torch.jit.save(traced, tmp)
    os.replace(tmp, path)
    return path