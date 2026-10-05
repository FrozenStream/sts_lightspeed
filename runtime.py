"""运行环境相关的**单一定义点** —— 迁移到别的机器时只需要看这一个文件。

三件事收在这里：
  1. 数据缓存目录（以前默认写 `%TEMP%`，导致缓存**不在工作区里**，拷项目会丢数据）
  2. 并行 worker 数（本机 8 物理核调的默认值不该带到大机器上）
  3. multiprocessing 启动方式（POSIX 能用 `fork`，Windows 只能 `spawn`，差价 ~6 秒/次）

**这里没有旋钮**：下面全是纯函数，参数由调用方显式给（`workers(v)` / `mp_context(mp_start)` /
`cache_path(name, cache_dir)` / …）。配方（架构/优化/口径/线程/设备/打点）是
`train.train()` 的**形参**，由各脚本自己的 argparse 解析后一路传下去
（见 `train.RECIPE_KEYS`）；别再往这里加 `STS_*` 读取点。

⚠️ **全仓唯一还读环境变量的地方** = `train._BGLOG`（`STS_BGLOG`，作业日志路径，由
`sync.sh`/`chain.sh` 在启动时注入 —— 那不是配方，是作业环境）。
"""
import inspect
import multiprocessing as mp
import os
import re
import sys

# ---- 控制台编码：本地 Windows 默认是 GBK，而这套脚本要打 ✅/❌ 这类符号 ----
# ⚠️ 症状是**静默的**：直接跑在终端里正常（VS Code 终端是 UTF-8），但一旦被管道
#    （`| tail`、`> log`）Python 就用 locale 编码 + `errors="strict"` → `UnicodeEncodeError`
#    当场崩。已经踩过两次（一次在 snippet，一次在 `_gen_eval.py tiers | tail`）。
#    在 import 时统一切成 UTF-8；失败也不该拖垮脚本，所以 errors="replace"。
for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))


# 缓存默认目录（**不是旋钮**：只是下面各函数的形参默认值）。
# ⚠️ 以前默认写 `%TEMP%` / `/tmp`，所以缓存**不在工作区里** —— 拷项目到别的机器会静默丢数据。
DATA_DIR = os.path.join(HERE, "data")


def cache_path(name, cache_dir=None):
    """缓存文件全路径（父目录建好）。`cache_dir=None` = 仓库内 `data/`。"""
    d = cache_dir or DATA_DIR
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def cache_name(name, kind, n, sims=None, **extra):
    """数据文件名的**唯一**拼装点：`[<自定义名字>_]<kind>_n<N>_s<SIMS>_<k><v>...npz`。

    `name` = 人给的名字（采集器 `--name`，如 `imit`），进名字最前面；不给就只剩 `<kind>_…`。
    ⚠️ 为什么要收口（2026-09-25）：`data/` 里同时躺着 `walkdata_n800_s4000.npz`（无版本）、
    `walkdata_v3_n800_s4000.npz`、`walkdata_v4_n10000_s4000.npz`、`_walkv5_n500.npz`
    （前导下划线、版本在中间）—— **光看名字分不出哪个是现役**，而它们相差一个数量级算力。
    ⚠️ `extra` **不排序**（2026-10-02）：顺序 = 调用方给的顺序 = 配方签名顺序，名字读起来就是配方。
    """
    parts = ([name] if name else []) + [kind, f"n{n}"]
    if sims is not None:
        parts.append(f"s{sims}")
    parts += [f"{k}{_fmt_val(extra[k])}" for k in extra]
    return "_".join(parts) + ".npz"


def dataset_tag(npz_path) -> str:
    """数据名进**别的**文件名时只留自定义部分：`_n<数字>` 之前的一切（`[<名字>_]<kind>`）。

    ⚠️ 抄全名会撞长度上限：实测自对弈那支（数据名 156 字符）嵌进权重名后到 **320 字符**，
    超 255 ⇒ 训练跑完、存盘那一步才 `ENAMETOOLONG`。名字本身就是一份完整配方，不必再抄一遍。
    没带名字时退化成 `<kind>`（旧名如 `encdata_sp_s500_sample` 无 `_n<数字>` ⇒ 原样返回）。
    """
    stem = os.path.splitext(os.path.basename(npz_path))[0]
    m = re.match(r"^(.*?)_n\d+", stem)
    return m.group(1) if m else stem


def net_tag(ckpt_path) -> str:
    """权重名进**别的**文件名时只留自定义部分：第一段（`train.py --name` 给的名字，如 `sp1`）。

    没给 `--name` 时第一段就是它自己的数据段，照样能指认（只是长一点）。
    ⚠️ 自定义名字因此是**唯一身份**：一个名字只能对应一个网，重名会静默指向错的那份。
    """
    return os.path.basename(ckpt_path).split("-")[0].removesuffix(".pt")


def recipe_kv(fn, kw, drop=()):
    """按 `fn` 的签名挑出**全部**参数（含默认值、签名顺序），供文件名拼装。

    口径同 `train.ckpt_params`：名字本身就是一份完整配方，不看文件、不查日志就能复现
    ⇒ **不做手工白名单**，只按 `drop` 除外。`kw` 里缺一个签名参数就当场 `KeyError`
    —— 新增配方参数而忘了进名字 = 两种数据同名，那种错静默且代价高。
    """
    return {k: kw[k] for k, p in inspect.signature(fn).parameters.items() if k not in drop}


def _fmt_val(v) -> str:
    """参数值 → 名字里的一段。布尔写 `1`/`0`（`True` 里的大小写当文件名不好看）；
    浮点用 `%g`（`0.001` / `0.2` / `1`）；其余 `str`。`None` 写 `none` 而不是 `None`。"""
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, float):
        return f"{v:g}"
    return "none" if v is None else str(v)


# `short_name` 显示时**丢掉**的段：结构 / 优化数值。它们几乎每次实验都是同一组默认值，
# 全显示会让评测表的列宽到几百字符。`z_spec` / `soft_target` / `soft_temp` **必须留着** ——
# 那是各条臂之间真正不同的东西：丢了它们，软目标与硬目标在表里会同名。
DISPLAY_DROP = ("hidden", "depth", "arch", "sel_arch", "pair", "dropout", "lr", "wd",
                "batch", "loss_chunk", "threads", "device")


def short_name(path) -> str:
    """完整权重名 → **显示用短名**：`<自定义名字>-<数据集名>-e<轮数>` + 非超参段。

    完整名（约两百字符、含全部超参）永远在文件上；显示只求「一行放得下且互不重名」。
    没有轮数段（`epochs=None`）时原样返回。
    """
    stem = os.path.splitext(os.path.basename(path))[0]
    parts = stem.split("-")
    cut = next((i for i, p in enumerate(parts) if re.fullmatch(r"e\d+", p)), None)
    if cut is None:
        return stem
    keep = [p for p in parts[cut + 1:] if not p.startswith(DISPLAY_DROP)]
    return "-".join(parts[:cut + 1] + keep)


def ckpt_name(npz_path, epochs=None, name=None, cache_dir=None, **extra):
    """权重文件名 = `<自定义名字>-<数据引用>-<各项参数>.pt`（含目录）。

    段间用**连字符**：数据引用自己就用下划线（`imit_encdata`），两个分隔符不打架。
    `name` 是调用方给的自定义名字（`train.py --name`，如 `imit` / `sp1`）；不给就只剩数据引用。
    轮数 `e<N>` 排在参数最前，其余**按调用方给的顺序**逐个写上（`hidden128-lr0.001`）。

    调用方只管把参数给全，不区分默认值 —— 名字本身就是一份完整配方，不看文件就能复现。
    ⚠️ 数据引用只留**自定义部分**（`dataset_tag`）：抄数据的全名会在自对弈那支超 255 字符。
    ⚠️ 以前权重叫 `az_boss.pt` / `n1_s0.pt` —— 拿到权重**看不出**它是哪份数据、怎么训的，
    对不上就没法复现。名字里带数据引用 ⇒ 可反查；`--out` 只当逃生口。
    """
    parts = [p for p in (name, dataset_tag(npz_path)) if p]
    if epochs is not None:
        parts.append(f"e{epochs}")
    parts += [f"{k}{_fmt_val(v)}" for k, v in extra.items()]
    return cache_path("-".join(parts) + ".pt", cache_dir)


def _cpu_quota():
    """cgroup 的 CPU 配额（核数）；拿不到（非容器/非 Linux）返回 None。

    ⚠️ `os.cpu_count()` **无视 cgroup**：AutoDL 容器里它报宿主机的 128 核，而配额是 12
    ⇒ 按它开 worker 会超订 5 倍。以前靠 `sync.sh` 硬塞 `STS_WORKERS=16` 绕开，现在
    那个 env 没了 ⇒ 必须自己读配额。
    """
    try:                                    # cgroup v2
        with open("/sys/fs/cgroup/cpu.max", encoding="ascii") as f:
            quota, period = f.read().split()
        if quota != "max":
            return max(1, int(int(quota) / int(period)))
    except (OSError, ValueError):
        pass
    try:                                    # cgroup v1
        with open("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", encoding="ascii") as f:
            quota = int(f.read().strip())
        with open("/sys/fs/cgroup/cpu/cpu.cfs_period_us", encoding="ascii") as f:
            period = int(f.read().strip())
        if quota > 0:
            return max(1, int(quota / period))
    except (OSError, ValueError):
        pass
    return None


def workers(v=None):
    """并发上限：`None`/空 = 本机默认（cgroup 配额优先，上限 64）；`'serial'`/`0`/`'off'` = 关并行（`None`）；
    否则按数值。

    `v` 就是入口脚本 `--workers` 解析出来的值（**形参，不是全局状态**；argparse 给的是字符串）。
    ⚠️ 本机是 8 物理核 / 16 逻辑核，实测 ~5x 就饱和（GIL 型负载吃不到超线程）
    ⇒ 不要拿逻辑核数当 worker 数，要更多请显式给 `--workers`。
    """
    s = str(v).strip().lower() if v not in (None, "") else ""
    if s in ("serial", "0", "off"):
        return None
    if s:
        return int(s)
    return max(1, min(64, _cpu_quota() or os.cpu_count() or 4))


def mp_context(mp_start=None):
    """跨平台的 multiprocessing context。`mp_start` = 入口脚本的 `--mp-start`。

    POSIX 上优先 `fork`：worker 直接继承父进程已经 import 好的 torch，启动几乎免费。
    Windows 只能 `spawn`（每个 worker 重跑 `import torch`：实测 3.9~9.7 s）。
    ⚠️ fork + OpenMP 理论上可能死锁（父进程先用过多线程 torch 再 fork）⇒ 出问题就传 `spawn`。
    """
    want = str(mp_start or "").strip().lower()
    avail = mp.get_all_start_methods()
    if want:
        return mp.get_context(want)
    for m in ("fork", "forkserver", "spawn"):
        if m in avail:
            return mp.get_context(m)
    return mp.get_context()


# torch 线程数 = **入口脚本的形参**（`--train-threads` / `--rollout-threads`），这里不持有。
#
# ⚠️ 为什么必须固定：线程数会改变 GEMM 的浮点归约顺序，而这个搜索对微小扰动**混沌敏感**
# （见 .agent/notes.md：把 `3*sqrt(2)` 换成差 1 ULP 的字面常量，教师胜率动了 7.5pp）。
# 训练与 rollout 各自固定，且**父进程串行与 worker 必须用同一个 rollout 值**，
# 否则「并行 == 串行」这个复现前提不成立。两个默认都是 **1**（线程数是配方的一部分）。
