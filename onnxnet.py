"""权重 → ONNX 存档（**唯一**拼名点 + 导出 + 验收）—— 采集与门控共用。

为什么是 ONNX 而不是 TorchScript：C++ 侧只为跑网络就得链 libtorch ⇒ 连带一串代价（`.pyd` 静态链
`c10.dll`/`torch_cpu.dll` ⇒ 进程里**必须先 `import torch`**、torch 自带一份 pybind11 与子模块打架、
构建要 `TORCH_CMAKE_PREFIX`）。换成 ONNX Runtime 后 C++ 侧只认 `onnxruntime`（头 + 库），torch 退回
**训练/导出**这一侧；旧的 TorchScript 导出器（`tsnet.py`）与 C++ 那侧（`torch_eval.*`）已于 2026-10-15 删除。

⚠️ 存档**按设备**各一份：CUDA 的执行提供者只认在 GPU 上跑的图（CPU 版跑得动但别指望性能），
所以命名带 `-cpu` / `-cuda`；同一份权重两种设备 = 两份文件。（ONNX 本身设备无关，这里的后缀是
为了"**缓存键**要含设备"——否则 cuda 跑回 CPU 图会很慢且难查。）
⚠️ 数值口径：ONNX Runtime 的算子不是 torch 的实现 ⇒ 与 torch **不可能逐位相同**（实测 max|Δ| ≈ 3e-7、
两个头的 argmax 逐行一致）。要改判据就改 `verify()` 里那两条。
"""
import os

import numpy as np
import torch  # noqa: F401  ⚠️ 导出侧要它（`slaythespire` 扩展**不再**依赖它）

import ckpt

# 导出用的 opset。⚠️ 换它 = 换图 ⇒ 别无声改（同一份权重导两次会得到不同编号的算子）。
OPSET = 17


def _tag(device: str) -> str:
    """设备 → 文件名后缀：`""` / `cpu` → `cpu`；`cuda` / `cuda:1` → `cuda` / `cuda1`。"""
    return (device or "cpu").replace(":", "")


def onnx_path(net_path: str, device: str = "cpu") -> str:
    """`net_path` 在 `device` 上的 ONNX 存档路径（**唯一**拼名点）。

    `data/onnx/` 是**派生目录**（不在 `data/` 的缓存命名体系里）：名字由**内容指纹 + 设备**决定
    ⇒ 换权重/换设备自动换文件、重跑同一份权重直接命中。只读 ⇒ 所有 worker/进程共用一份。
    """
    h = str(ckpt.read(net_path)["fp"])
    stem = os.path.splitext(os.path.basename(net_path))[0].split("-")[0]
    return os.path.join("data", "onnx", f"{stem}-{h}-{_tag(device)}.onnx")


def build_net(net_path: str, device: str = "cpu"):
    """按存档里的建网配置造网 + 灌权重（`AZNet` 定义只有 Python 一份）。"""
    from aznet import AZNet, net_kwargs

    cfg = ckpt.config_of(net_path)
    if not cfg:
        raise RuntimeError(f"{net_path} 存档里没有建网配置（用 `python ckpt.py {net_path}` 看）")
    net = AZNet(**net_kwargs(cfg)).to(device or "cpu")
    net.load_state_dict(ckpt.weights_of(net_path))
    net.eval()
    return net


def export(net_path: str, device: str = "cpu", out: str | None = None) -> str:
    """导出（**原子**落盘：先写 `.tmp` 再 `replace`），返回路径。已有就跳过。"""
    from aznet import OBS_DIM

    path = out or onnx_path(net_path, device)
    if os.path.exists(path):
        return path
    net = build_net(net_path, device)
    x = torch.zeros(8, OBS_DIM, device=device or "cpu")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.{os.getpid()}.tmp"
    # ⚠️ 走 **legacy tracer**（`dynamo=False`）：新的 `torch.export` 路线要装 `onnxscript`，而这条图
    #    没有数据相关的分支（追踪出来的图与 `torch.jit.trace` 同构）⇒ 没必要多一个依赖。
    #    批维是**动态**的（池宽/批宽每轮都变）⇒ 必须显式声明 `dynamic_axes`。
    torch.onnx.export(
        net, (x,), tmp,
        input_names=["obs"], output_names=["policy", "select", "value"],
        dynamic_axes={"obs": {0: "n"}, "policy": {0: "n"}, "select": {0: "n"}, "value": {0: "n"}},
        opset_version=OPSET, do_constant_folding=True, dynamo=False,
    )
    os.replace(tmp, path)
    return path


def ensure_onnx(net_path: str, device: str = "cpu") -> str:
    """没有就导一份并原子落盘，返回路径（`collect_selfplay.py` / `eval_gate.py` 的入口）。"""
    return export(net_path, device)


# ---------------------------------------------------------------- 验收

def sample_obs(n: int, seed: int = 0) -> "torch.Tensor":
    """合成一批**合法** obs（真实 obs 要起引擎才有；这里够用来验算子一致性）。

    ⚠️ 别用 `randn`：id 列会被 `obs[..., idx].long()` 当嵌入下标，负值/超界直接 IndexError；
    one-hot 列也有上界。⇒ 小数特征随机（`rand*0.5` ⇒ 取整后恒 0，对 id / one-hot 都合法），
    再把三类**类别列**显式塞成小整数，让"非空槽"那条路也被走到。
    """
    from azlayout import CARD_ID_IDX, MONSTER_ID_IDX, MONSTER_INTENT_MOVE_IDX
    from aznet import OBS_DIM

    g = torch.Generator().manual_seed(seed)
    x = torch.rand(n, OBS_DIM, generator=g) * 0.5
    for idx, lo, hi in ((CARD_ID_IDX, 1, 40), (MONSTER_ID_IDX, 1, 5),
                        (MONSTER_INTENT_MOVE_IDX, 1, 3)):
        x[:, torch.as_tensor(idx, dtype=torch.long)] = torch.randint(
            lo, hi, (n, len(idx)), generator=g).float()
    return x


def verify(net_path: str, device: str = "cpu", batches=(1, 8, 64, 256, 512),
           atol: float = 1e-4) -> bool:
    """torch vs ONNX Runtime 的逐批对账（**动作层面必须完全一致**）。返回是否通过。

    判据两条：① 两个头的 argmax **逐行**相同（主判据：搜索只经 softmax/argmax 消费 logits）；
    ② `max|Δ|` < `atol`（兜底：跨实现/跨 torch 版本的 float 级差异差一个量级很正常 —— 本地
    torch 2.13 差 ~1e-6、远端 2.12 差 ~1e-5，所以线划在 1e-4）。
    ⚠️ "逐位"是不可能达成的（不同实现的归约顺序不同）⇒ 别拿它当回归判据。
    """
    import onnxruntime as ort
    from aznet import OBS_DIM

    net = build_net(net_path, device)
    sess = ort.InferenceSession(ensure_onnx(net_path, device),
                                _sdopts(intra=1, device=device), providers=_providers(device))
    want_cuda = (device or "cpu").lower().startswith("cuda")
    got = sess.get_providers()
    if want_cuda and "CUDAExecutionProvider" not in got:
        # ⚠️ ORT 的 providers 列表是**按优先级回退**的：注册失败时它会**静默**用 CPU 跑完这轮对账
        #    ⇒ 那会把"GPU 没接上"伪装成"数值一致、只是慢"。要 GPU 就得当场报出来。
        raise RuntimeError(f"要 CUDA 执行提供者但会话只拿到 {got} —— 换 GPU 包 / 装 cuDNN 9？")
    print(f"执行提供者 = {got}")
    torch.set_num_threads(1)
    ok = True
    print(f"{'n':>6} {'max|Δp|':>9} {'max|Δs|':>9} {'max|Δv|':>9} {'argmax 同':>10} {'判据':>6}")
    for n in batches:
        x = sample_obs(n)
        with torch.no_grad():
            tp, ts, tv = net(x.to(device or "cpu"))
        op, os_, ov = sess.run(None, {"obs": x.numpy()})
        dp = float(np.abs(op - tp.cpu().numpy()).max())
        ds = float(np.abs(os_ - ts.cpu().numpy()).max())
        dv = float(np.abs(ov - tv.cpu().numpy()).max())
        same = float((op.argmax(1) == tp.cpu().numpy().argmax(1)).mean())
        good = same == 1.0 and max(dp, ds, dv) < atol
        ok = ok and good
        print(f"{n:>6} {dp:>9.2e} {ds:>9.2e} {dv:>9.2e} {same:>10.2f} {'✓' if good else '✗':>6}")
    return ok


def _sdopts(intra: int, device: str):
    """与 C++ 侧 `OnnxEvaluator` 同口径的会话选项（对账时两边得是同一种配置）。"""
    import onnxruntime as ort

    so = ort.SessionOptions()
    so.intra_op_num_threads = max(1, int(intra))  # 小批前向下线程池同步开销压倒计算（同 torch 那条）
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    so.log_severity_level = 3
    return so


def _providers(device: str):
    """设备串 → EP 列表：`cuda[:k]` → CUDA（`device_id=k`），否则 CPU。

    ⚠️ `use_tf32="0"` **必须显式给**：ORT 的 CUDA EP 默认开 TF32（实测与 torch(FP32) 差 4.29e-3），
    而 C++ 侧 `OnnxEvaluator` 的默认也是关（`allow_tf32=false`）⇒ 两边必须同口径，否则"对账通过"
    只是两边都开了 TF32 而已。
    """
    dev = (device or "cpu").lower()
    if not dev.startswith("cuda"):
        return ["CPUExecutionProvider"]
    dev_id = int(dev.split(":")[1]) if ":" in dev else 0
    return [("CUDAExecutionProvider", {"device_id": str(dev_id), "use_tf32": "0"}),
            "CPUExecutionProvider"]


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        sys.exit("用法：python onnxnet.py <net.pt> [device]   # 导出并做 torch ↔ ORT 对账")
    dev = sys.argv[2] if len(sys.argv) > 2 else "cpu"
    p = ensure_onnx(sys.argv[1], dev)
    size = os.path.getsize(p) / 1e6
    print(f"ONNX 存档：{p}（{size:.2f} MB）")
    sys.exit(0 if verify(sys.argv[1], dev) else 1)
