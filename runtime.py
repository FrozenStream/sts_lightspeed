"""运行环境相关的**单一定义点** —— 迁移到别的机器时只需要看这一个文件。

四件事收在这里：
  1. 数据缓存目录（以前默认写 `%TEMP%`，导致缓存**不在工作区里**，拷项目会丢数据）
  2. 并行 worker 数（本机 8 物理核调的默认值不该带到大机器上）
  3. multiprocessing 启动方式（POSIX 能用 `fork`，Windows 只能 `spawn`，差价 ~6 秒/次）
  4. 产物命名与配方校验（`artifact_dir` / `check_recipe` / `cache_path` / `recipe_kv`）与
     **脚本 CLI**（`cli_for`）

**这里没有旋钮**：下面全是纯函数，参数由调用方显式给（`workers(v)` / `mp_context(mp_start)` /
`cache_path(name, cache_dir)` / `cli_for(fn)` / …）。配方（架构/优化/口径/线程/设备/打点）是
`train.train()` 的**形参**，由各脚本自己的 argparse 解析后一路传下去
（见 `train.RECIPE_KEYS`）；别再往这里加 `STS_*` 读取点。

⚠️ **全仓唯一还读环境变量的地方** = `train._BGLOG`（`STS_BGLOG`，作业日志路径，由
`sync.sh`/`chain.sh` 在启动时注入 —— 那不是配方，是作业环境）。
"""
import argparse
import inspect
import json
import multiprocessing as mp
import os
import shutil
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

# ── 产物**名字**：只有一个固定默认名，名字里**不带配方**（2026-10-15 定案）──────────────────
#
# 为什么把"名字 = 完整配方"反过来：那样每加一个旋钮就长十几字符，而单段上限 255（ext4/NTFS）
# ⇒ 实测生产配方**第 2 轮就 271 字符**（`start_seed<8位>` 与 `net<上一轮权重名首段>_wfp<指纹>`
# 一起进来），`os.makedirs` 在采集第一步抛 `ENAMETOOLONG` —— 报错点离原因很远，且每加一个
# 旋钮就复现一次。**现在**：名字 = `<kind>`（数据目录）/ `<数据目录名>.pt`（权重），
# 配方改由**产物内部**承载，唯一性靠"同名 ⇒ 必须同配方，否则炸"：
#
#   · 数据目录：`<目录>/_recipe.json` —— 复用前逐字比对（`check_recipe`），不一致当场报错
#   · 权重：存档里的 `recipe`（`ckpt.save(..., recipe=…)`；`config`/`z_spec`/`metrics` 本来就在）
#
# ⚠️ 代价（明确接受）：**从文件名看不出配方**，要看 `_recipe.json` / 存档 / 运行日志
#    （采集与训练都会把完整配方打一行）。⚠️ 也**不再有自定义名字**（`--name` 已删）：
#    要落到别的位置就用 `--cache`（采集）/ `out` 位置参数（训练）显式给路径。
RECIPE_FILE = "_recipe.json"


def artifact_dir(kind: str, cache_dir: str | None = None) -> str:
    """采集产物的**默认目录**：`<cache_dir>/<kind>`（`kind` 如 `spencs` / `encdata_teacher`）。

    名字里**不带任何配方**（见上面那段）；`--cache` 给了就用给的路径（不变）。
    """
    return os.path.join(cache_dir or DATA_DIR, kind)


def check_recipe(dir_path: str, recipe: dict, *, kind: str) -> None:
    """目录 ↔ 配方的**一致性闸**（数据产物复用的唯一判据）。

    四种情形，各自响亮：

    | 目录状态 | 行为 |
    |---|---|
    | 不存在 / 没有分片 | 建目录 + 写 `<dir>/_recipe.json`（**采集前就写**：半路崩了也留下证据） |
    | 有 `_recipe.json` 且逐字相同 | 放行（调用方随后按"已有分片"走复用） |
    | 有 `_recipe.json` 但不同 | **报错**，并列出**哪几项不同**（对症才能改） |
    | 有分片却没有 `_recipe.json` | **报错**（旧命名时代的目录 / 手造的目录 ⇒ 换 `--cache` 或删掉） |

    `recipe` 必须是 JSON 口径的基本类型（`recipe_kv()` 出来的就是）⇒ 比对 = 逐字相等。
    """
    path = os.path.join(dir_path, RECIPE_FILE)
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            old = json.load(f)
        if old.get("recipe") == recipe and old.get("kind") == kind:
            return
        bad = sorted(k for k in set(old.get("recipe") or {}) | set(recipe)
                     if (old.get("recipe") or {}).get(k) != recipe.get(k)
                     or (k == "kind" and old.get("kind") != kind))
        diff = {k: ((old.get("recipe") or {}).get(k), recipe.get(k)) for k in bad}
        raise RuntimeError(
            f"{dir_path} 里那份产物的配方与本次**不一致**（{RECIPE_FILE}）：\n"
            f"  不同项 {diff}\n"
            f"  ⇒ 同名目录不许装两份配方：换个 `--cache`，或删掉这个目录重采")
    shards = [n for n in os.listdir(dir_path) if n.endswith(".bin")] if os.path.isdir(dir_path) else []
    if shards:
        raise RuntimeError(
            f"{dir_path} 里有 {len(shards)} 个分片却没有 {RECIPE_FILE} ⇒ "
            f"**配方无从校验**（旧命名时代的产物 / 手造的目录）：换个 `--cache`，或删掉它重采")
    os.makedirs(dir_path, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"kind": kind, "recipe": recipe}, f, ensure_ascii=False, indent=1, sort_keys=True)


def reuse_shards(kind: str, shard_dir: str, recipe: dict) -> list[str] | None:
    """采集产物的**复用/新建唯一入口**：清空壳 → 配方闸（`check_recipe`）→ 命中就返回已有分片。

    返回 `None` = 需要真采（此刻目录与 `_recipe.json` 都已就位）。命中与“本次配方”各打一行 ——
    调用方（`chain.sh` 那类）靠“产物目录 …”那一行拿数据位置，不能静默。
    """
    # 函数内导入：`collect_io` 拉 numpy，而本模块被所有脚本 import ⇒ 别把 numpy 挂在 import 期。
    from collect_io import list_shards

    if os.path.isdir(shard_dir) and not list_shards(shard_dir):
        shutil.rmtree(shard_dir, ignore_errors=True)  # 上次中断留下的空壳（有目录、没分片）
    check_recipe(shard_dir, recipe, kind=kind)
    got = [str(p) for p in list_shards(shard_dir)]
    if got:
        print(f"产物目录 {shard_dir} 复用已有 {len(got)} 个分片", flush=True)
        return got
    print(f"配方 {json.dumps(recipe, ensure_ascii=False, sort_keys=True)}", flush=True)
    return None


def cache_path(name, cache_dir=None):
    """产物全路径（父目录建好）。`cache_dir=None` = 仓库内 `data/`。

    ⚠️ 本函数是**全部产物路径的唯一拼装点**（数据目录与权重都走它）⇒ 路径口径只在这里写一遍。
    """
    d = cache_dir or DATA_DIR
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def recipe_kv(fn, kw, drop=()) -> dict:
    """按 `fn` 的签名挑出**全部**参数（含默认值、签名顺序）—— 产物的**配方**就是它。

    ⚠️ 它现在进的是 `_recipe.json` 与存档的 `recipe`（不再是文件名）。
    **不做手工白名单**，只按 `drop` 除外；`kw` 里缺一个签名参数就当场 `KeyError` ——
    新增一个会改数据的旋钮而忘了进配方 = 两种产物同名，那种错静默且代价高。
    """
    return {k: kw[k] for k, p in inspect.signature(fn).parameters.items() if k not in drop}


def cli_for(fn, description=""):
    """按 `fn` 的**签名**注册 `--flag` —— 签名是 flag 的**唯一来源**：

    | flag 的哪一段 | 来自哪里 |
    |---|---|
    | `--<名字>` | 形参名（`_` → `-`） |
    | 类型 | 注解（无注解、或注解是不可调用的联合时退回默认值的类型） |
    | 默认值 / 是否必填 | 形参默认值（没有默认值 ⇒ `required=True`） |

    用法（`main()` 里就这一句）：

        kw = vars(cli_for(collect_part, "collect one item").parse_args())
        collect_part(**kw)          # 名字对不上就让它自己 TypeError

    ⇒ **没有第二张表**：加一个形参 = 自动多一个 flag（名字/类型/默认值一起长出来）。

    ⚠️ **不做逐 flag 的 `help=`**：那种说明要么另开一张 `{参数名: 说明}` 的表（改名后静默变空
    说明），要么贴在形参上（`Annotated[T, "…"]`）把签名撑成一堵墙 —— 而它要说的东西 docstring
    里本来就有。`--help` 印的是：usage + `description` + flag 列表 + **本模块的 docstring**
    （`epilog`，`Raw…` ⇒ 原样打印、不重排）⇒ **旋钮的说明写在模块 docstring 里**。
    ⚠️ 注解要是**真的类型对象**（别用 `from __future__ import annotations`）：`type=` 得能调。
    ⚠️ `bool` 走 `BooleanOptionalAction` ⇒ 生成 `--x` / `--no-x` 一对。**别**写 `type=bool`：
    `bool("0")` 是 `True`，`--flag 0` 会静默变成打开。
    """
    p = argparse.ArgumentParser(
        description=description, epilog=fn.__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    for k, prm in inspect.signature(fn).parameters.items():
        if prm.kind in (prm.VAR_POSITIONAL, prm.VAR_KEYWORD):
            continue                      # `*args` / `**kw` 没有可命名的 flag
        flag = "--" + k.replace("_", "-")
        if prm.default is inspect.Parameter.empty:
            # 必填 ⇒ 没有默认值可以借类型，注解必须真的可调用。
            # ⚠️ 宽松兜底（退回 `str`）不行：`sims="8"` 会一路漏到函数体里才在 `sims < 2` 上炸，
            #   而 `inspect.Parameter.empty` **是**一个可调用的类 ⇒ 不清查就会得到
            #   `invalid _empty value` 这种看不懂的错。
            if prm.annotation is inspect.Parameter.empty or not callable(prm.annotation):
                raise TypeError(
                    f"`cli_for({fn.__name__})`：必填形参 {k!r} 要写一个真的类型注解"
                    f"（当前 {prm.annotation!r}）—— 否则 `{flag}` 收不到对的类型")
            p.add_argument(flag, required=True, type=prm.annotation)
        elif isinstance(prm.default, bool):
            p.add_argument(flag, action=argparse.BooleanOptionalAction, default=prm.default)
        else:
            # `type=`：注解优先（`x: float = 0` 的默认值是 `int`，按 `type(default)` 写会拒收
            # `0.5`）；无注解、或注解是不可调用的联合（`str | None`）⇒ 退回默认值的类型。
            ann = prm.annotation
            p.add_argument(flag, default=prm.default,
                           type=ann if (ann is not inspect.Parameter.empty and callable(ann))
                           else (str if prm.default is None else type(prm.default)))
    return p


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


# torch 线程数 = **入口脚本的形参**（`--train-threads`），这里不持有。
# 采集侧的前向在 C++ 的 ONNX Runtime 里，intra-op 固定 1（每个 worker 一个会话），没有对应旋钮。
#
# ⚠️ 为什么必须固定：线程数会改变 GEMM 的浮点归约顺序，而这个搜索对微小扰动**混沌敏感**
# （见 .agent/notes.md：把 `3*sqrt(2)` 换成差 1 ULP 的字面常量，教师胜率动了 7.5pp）。
# 训练与 rollout 各自固定，且**父进程串行与 worker 必须用同一个值**，
# 否则「并行 == 串行」这个复现前提不成立。默认 **1**（线程数是配方的一部分）。
