"""权重存档的**唯一读写入口** —— 单文件自描述（`fmt=1`，2026-09-28 起）。

存档 = 一个 `torch.save` 的 dict：

    {"fmt": 1, "weights": <state_dict>, "config": {...}, <其余元数据>}

`weights` 是 `net.state_dict()`；`config` 是**建网配置**（= `train.CONFIG` 的快照；整局网那份
已于 2026-10-06 随整局线退役）——读侧 `AZNet(**net_kwargs(cfg))` 就能重建出这份权重所属的网。
其余键是可选元数据：战斗网放 `space`（动作空间宽）/`z_spec`（值口径）/`metrics`（训练指标）。

⚠️ **为什么必须是同一个文件**：`state_dict` 是纯张量、**不带**任何元数据（PyTorch 的既定设计），
所以"这份权重是按什么配方建的"只能存在别处。以前存在同名 `.json` 副档里 ⇒ ① 拷走 `.pt` 就丢配置
（没人拦）② 两份会漂（改了一份没改另一份）③ 路径约定还分两套（战斗侧 `x.pt.json`、整局侧 `x.json`）。
配置与权重同生同死，才谈得上"拿到的权重知道自己是怎么建的"。

⚠️ 旧的两文件存档（`.pt` + 同名 `.json`）**仍能读**，但 `read()` 会打一行 ⚠️：读侧要容忍历史
文件，不要容忍到"以为现在还是两文件制"。要迁移就用 `ckpt.migrate()` 重存（权重不变）。

⚠️ 缺配置时 `config={}`（**不猜默认值**）—— 是报错、按键名推断、还是退回当前脚本的配置，
由调用方决定（本项目反复踩"静默兜底"的坑，所以这里不替调用方兜）。
"""
import hashlib
import json
import os
from typing import Any

import numpy as np
import torch

FMT = 1                 # 当前格式号。加字段不必动它；改**含义**才动。
LEGACY_FMT = 0          # `.pt` + 同名 `.json` 副档（两文件，2026-09-28 之前）
_SIDE_SUFFIX = ".json"  # 旧副档后缀（只读路径用；**不再写**）


def weights_fp(weights: Any) -> str:
    """权重的**内容指纹**（`sha1[:8]`）：键名 + dtype + 形状 + 字节，按键名排序。

    名字（`--name`）是**人给的标签**，同一个标签训两遍内容完全不同 —— 指纹才是身份。
    进存档（`save()` 自动算）、并从那里进自对弈数据的名字（`net<tag>_wfp<8>`）。

    ⚠️ 只比较**权重**；建网配置与值口径在 `config` / `z_spec` 里，各自单独看。
    """
    h = hashlib.sha1()
    for k in sorted(weights):
        t = weights[k]
        h.update(k.encode())
        h.update(str(tuple(t.shape)).encode())
        h.update(str(t.dtype).encode())
        h.update(np.ascontiguousarray(t.detach().cpu().numpy()).tobytes())
    return h.hexdigest()[:8]


def save(path: str, weights: Any, config: dict, **meta: Any) -> str:
    """写单文件存档，返回路径。

    参数：
      `path`    存档路径（**父目录要先存在** —— 这里不建目录）
      `weights` `net.state_dict()`（任何“名字 → 张量”的 dict）
      `config`  **建网配置** ⇒ 读侧 `AZNet(**net_kwargs(config))` / `RunNet(**config)` 直接用
      `**meta`  其余元数据：战斗网 `space`/`z_spec`/`metrics`，整局网 `train`/`run_dim`/…
                （要能被 `torch.save` pickle；用基本类型的最佳 —— `ckpt.py` 与
                `faults.jsonl` 都按 JSON 口径看它）

    **原子替换**：先写同目录的 `<path>.<pid>.tmp` 再 `os.replace` ⇒ 并发的读侧只会看到
    完整旧档或完整新档，不会读到写了一半的文件（采集侧热重载最新权重就靠这条）。
    """
    payload = dict(fmt=FMT, weights=weights, config=dict(config), fp=weights_fp(weights), **meta)
    tmp = f"{path}.{os.getpid()}.tmp"
    try:
        torch.save(payload, tmp)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    return path


def read(path: str, map_location: Any = "cpu", weights_only: bool = False) -> dict:
    """读存档，返回**归一化**的 dict：`fmt` / `weights` / `config` / 其余元数据。

    `map_location` / `weights_only` 原样透传 `torch.load`（本格式的元数据都是基本类型 ⇒
    需要时可以用 `weights_only=True` 收紧）。

    ⚠️ 旧的两文件存档也走这里：`fmt=LEGACY_FMT`、`config` 取旧副档的 `reg`（历史键名），
    副档里其余键（`space`/`z_spec`/`metrics`）当元数据摊平 —— 于是调用方**不必**知道
    自己拿到的是哪种格式。
    """
    d = torch.load(path, map_location=map_location, weights_only=weights_only)
    if _is_new(d):
        out = dict(d)
        out["config"] = dict(out.get("config") or {})
        # `fp` 是加指纹之前的老档就没有 ⇒ 现算一个（读侧不必分两种存档）。
        if out.get("fp") is None and isinstance(out.get("weights"), dict):
            out["fp"] = weights_fp(out["weights"])
        return out
    # ---- 旧存档：裸 state_dict + 同名副档 ----
    cfg, meta = {}, {}
    side = legacy_sidecar(path)
    if side:
        try:
            with open(side, encoding="utf-8") as f:
                j = json.load(f)
        except (OSError, ValueError) as e:
            print(f"⚠️ 旧副档 {side} 读不了（{type(e).__name__}: {e}）—— 当它没有配置",
                  flush=True)
        else:
            cfg, meta = _legacy_split(j)
            print(f"⚠️ {path} 是**旧的两文件存档**（+ {os.path.basename(side)}）—— "
                  f"建议 `ckpt.migrate()` 重存为单文件", flush=True)
    return dict(meta, fmt=LEGACY_FMT, weights=d, config=cfg)


def config_of(path: str) -> dict | None:
    """只取建网配置（`path` = 存档路径）。**取不到就 None** —— 调用方自己决定报错还是
    换路（见模块 docstring）。

    ⚠️ 一次调用 = **读一遍整份存档**（含全部权重）：别在循环里逐条调，那里用 `read()`
    一次拿全。
    """
    try:
        return read(path)["config"] or None
    except (OSError, RuntimeError, ValueError, KeyError, EOFError) as e:
        print(f"⚠️ 存档 {path} 读不了（{type(e).__name__}: {e}）—— 当它没有配置", flush=True)
        return None


def weights_of(path: str, map_location: Any = "cpu", weights_only: bool = False) -> Any:
    """只取 `state_dict`（新格式在这层解包；旧格式它本身就是）。

    `map_location` / `weights_only` 同 `read()`。要**同时**拿配置/元数据就用 `read()`
    —— 那是一次 IO 拿全，不要 `config_of()` + `weights_of()` 读两遍。
    """
    return read(path, map_location=map_location, weights_only=weights_only)["weights"]


def legacy_sidecar(path: str) -> str | None:
    """旧副档的两种命名都试（`path` = 权重路径；战斗侧 `x.pt.json`、整局侧 `x.json`）；
    都没有返回 `None`。"""
    for p in (path + _SIDE_SUFFIX, os.path.splitext(path)[0] + _SIDE_SUFFIX):
        if os.path.exists(p):
            return p
    return None


def _legacy_split(j: Any) -> tuple[dict, dict]:
    """拆旧副档：战斗侧 = `{"reg": {...}, "space":…}`，整局侧 = **整个 dict 就是配置**。"""
    for key in ("config", "reg"):
        if isinstance(j.get(key), dict):
            return dict(j[key]), {k: v for k, v in j.items() if k != key}
    return dict(j), {}


def migrate(path: str) -> str | None:
    """把**旧**的两文件存档（`path`）重存成单文件（权重**逐位不变**），并删掉旧副档。

    返回存档路径；文件不存在或已经是新格式 ⇒ `None`（幂等，可以重复调）。
    """
    if not os.path.exists(path):
        return None
    ck = read(path)
    if ck["fmt"] == FMT:
        return None
    side = legacy_sidecar(path)
    meta = {k: v for k, v in ck.items() if k not in ("fmt", "weights", "config")}
    save(path, ck["weights"], ck["config"], **meta)
    if side:
        os.remove(side)
    return path


def _is_new(d: Any) -> bool:
    """新格式判别：`d`（`torch.load` 出来的对象）是 dict，且 `fmt` 是 int、`weights` 是 dict
    —— 裸 `state_dict`（纯张量）不会被误判成新格式。"""
    return (isinstance(d, dict) and isinstance(d.get("fmt"), int)
            and isinstance(d.get("weights"), dict))


if __name__ == "__main__":
    # 用法：python ckpt.py <存档> [...] —— 打印格式与配置（迁移前先看一眼）
    import sys
    for p in sys.argv[1:]:
        try:
            ck = read(p)
        except (OSError, RuntimeError, ValueError, EOFError) as e:
            print(f"✗ {p}: {type(e).__name__}: {e}")
            continue
        tag = "单文件" if ck["fmt"] == FMT else "⚠️ 旧的两文件"
        print(f"{p}  [{tag}]  config={ck['config']}  weights={len(ck['weights'])} 键  "
              f"元数据={sorted(set(ck) - {'fmt', 'weights', 'config'})}")
