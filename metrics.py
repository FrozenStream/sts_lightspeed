"""训练读数落盘：纯文本 JSONL，**不依赖 tensorboard**。

一次训练一个目录 `data/runs/<运行名>/seed<k>/`：

| 文件 | 内容 |
|---|---|
| `points.jsonl` | 逐点标量：`{"step": <int>, "tag": "<组/名>", "value": <float>}` |
| `recipe.json`  | 配方（架构 / z 口径 / 线程数 / 设备 / …）—— 绘图时给运行贴标签用 |

故障另落 `<根>/faults.jsonl`（一行一条，与运行无关，可直接 `grep`）。`tag` 里的 `/` 就是
分组（`train/loss`、`val/ce`、`fault/<标签>`），横轴一律是 `step`（全局 batch 号或 epoch）。

绘图（长表，`step` 为横轴）：

    import pandas as pd
    df = pd.read_json("data/runs/<运行名>/seed0/points.jsonl", lines=True)
    df.pivot_table(index="step", columns="tag", values="value").plot()

⚠️ 落盘是**旁路**：写不进去只警告一次，绝不影响训练；也**绝不在故障路径上再抛异常**
—— 那会盖掉真正的那条（本项目反复踩）。`writer()` 返回 `None` 表示本次不落盘，
调用点不必判空，后续函数全是 no-op。
⚠️ 关掉：调用方传 `on=False`（`train(record=False)` / `train.py … --record 0`）。
⚠️ 换根目录：调用方传 `out_dir=<路径>`（默认 `data/runs`）。
"""
import json
import os
import time
from typing import Any

import runtime

# 默认根目录（`data/runs`）——**不是旋钮**，只是「没给 out_dir」时的归属地。
DEFAULT_DIR = os.path.join(runtime.DATA_DIR, "runs")

# 同一进程里**只有一条训练**（多种子是各起子进程）⇒ 记住最近一个 Run，
# `fault()` 在异常路径上就不必再传一遍（那里最容易漏参数、也最容易再抛一次）。
_CURRENT: Any = None
_DIR: str = ""                 # 最近一次 `writer()` 的根目录（`faults.jsonl` 落到同一处）
_RUNS: dict[str, Any] = {}
_WARNED: set[str] = set()


class Run:
    """一个运行的落盘句柄：`points.jsonl` 常开，逐点 append + flush。"""

    def __init__(self, path: str):
        self.path = path
        os.makedirs(path, exist_ok=True)
        self.fh = open(os.path.join(path, "points.jsonl"), "a", encoding="utf-8")
        self.n = 0

    def point(self, step: int, tag: str, value: float) -> None:
        self.fh.write(json.dumps({"step": int(step), "tag": tag, "value": float(value)},
                                 ensure_ascii=False) + "\n")
        self.n += 1

    def flush(self) -> None:
        self.fh.flush()

    def close(self) -> None:
        try:
            self.fh.close()
        except OSError:
            pass


def writer(tag: str, sub: str | None = None, on: bool = True, out_dir: str | None = None) -> Any:
    """拿一个 `Run`；同一个 `tag/sub` 复用，**关掉或建不出来 ⇒ None**。

    参数：
      `tag`  运行名 —— 用权重名/数据名（`runtime.ckpt_name` 那套）⇒ 目录与存档对得上
      `sub`  子目录，用 `seed<k>` ⇒ 多种子并排可比；`None` = 不分子目录
      `on`   调用方自己的开关（`train(record=…)`）；`False` ⇒ 不落盘
      `out_dir`  根目录（`None` = `data/runs`）

    返回 `None` 表示不可用（关掉 / 目录建不出来）—— 调用点不必判空。
    """
    global _CURRENT, _DIR
    if not on:
        return None
    key = f"{tag}/{sub}" if sub else str(tag)
    d0 = out_dir or DEFAULT_DIR
    if key not in _RUNS:
        try:
            _RUNS[key] = Run(os.path.join(d0, key))
        except OSError as e:
            _warn_once("open", f"读数目录建不出来（{e}）—— 本次不落盘")
            _RUNS[key] = None
        else:
            print(f"[记录] {os.path.join(d0, key)}/points.jsonl", flush=True)
    _CURRENT = _RUNS[key]
    _DIR = d0
    return _CURRENT


def scalars(w: Any, step: int, **kv: float) -> None:
    """记一组标量。`w=None` ⇒ **什么都不做**；`step` = 横轴；`kv` = `tag → 值`。"""
    if w is None:
        return
    try:
        for k, v in kv.items():
            w.point(step, k, float(v))
        w.flush()
    except Exception as e:                          # noqa: BLE001  —— 见模块 docstring
        _drop(w, f"标量写入失败（{type(e).__name__}: {e}）")


def recipe(w: Any, d: dict) -> None:
    """配方落 `recipe.json`（人看 / 绘图贴标签两用）；**不落点**。"""
    if w is None:
        return
    try:
        with open(os.path.join(w.path, "recipe.json"), "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False, indent=1, sort_keys=True)
    except Exception as e:                          # noqa: BLE001  —— 见模块 docstring
        _drop(w, f"配方写入失败（{type(e).__name__}: {e}）")


def fault(tag: str, message: str, step: int = 0, writer_: Any = None,
          out_dir: str | None = None) -> None:
    """记一条故障：`<根>/faults.jsonl`（一行一条）+ `fault/<tag>` 标量点（=1）。

    参数：
      `tag`      故障**类别**（成为标量名 `fault/<tag>`、也是 jsonl 里的键）——
                 放"哪种病"（`loss_nonfinite`），别塞变量内容，否则每一条都是新曲线
      `message`  给人看的整句：要带**现场**（epoch/batch 进度、lr、上一个有限 loss…）
      `step`     出故障时的横轴位置（batch 或 epoch 序号）
      `writer_`  指定 Run；默认用本进程最近一个（`writer()` 记的 `_CURRENT`）
      `out_dir`  根目录；默认用本进程最近一次 `writer()` 的目录，再退到 `data/runs`

    ⚠️ **自己绝不抛** —— 故障路径上再炸一个异常会盖掉真正的那条（本项目反复踩）。
    """
    print(f"❌ [{tag}] {message}", flush=True)
    try:
        with open(os.path.join(out_dir or _DIR or DEFAULT_DIR, "faults.jsonl"), "a",
                  encoding="utf-8") as f:
            f.write(json.dumps({"ts": time.strftime("%Y-%m-%d %H:%M:%S"), "tag": tag,
                                "step": int(step), "message": message},
                               ensure_ascii=False) + "\n")
    except OSError as e:
        _warn_once("fault-log", f"故障记录写不进去（{e}）")
    w = writer_ if writer_ is not None else _CURRENT
    scalars(w, step, **{f"fault/{tag}": 1.0})


def close(w: Any = None) -> None:
    """收尾 flush + 关文件。`w=None` ⇒ 关**全部**；否则只关这一个。"""
    global _CURRENT
    for key, wr in list(_RUNS.items()):
        if wr is not None and (w is None or wr is w):
            wr.close()
        if w is None or wr is w:
            _RUNS.pop(key, None)
    if w is None:
        _CURRENT = None


def _drop(w: Any, msg: str) -> None:
    """把坏掉的 Run 从表里丢掉（只警告一次）—— 落盘坏了不能拖垮训练。"""
    _warn_once(type(w).__name__, f"{msg} —— 本次已停用")
    for key, wr in list(_RUNS.items()):
        if wr is w:
            _RUNS.pop(key, None)


def _warn_once(key: str, msg: str) -> None:
    """去重告警：`key` 相同只打一次（避免逐 batch 刷屏）。"""
    if key not in _WARNED:
        _WARNED.add(key)
        print(f"⚠️ [记录] {msg}", flush=True)
