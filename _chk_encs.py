"""守卫：按怪组×分布地点采集（`collect_encs.py`）—— 列 / 地点 / 续跑 / **失败不静默**。

    python _chk_encs.py

五件事：

  1. 小规模真采（**3 项 × 2 场** @ 25 sims，2 个 worker ⇒ 走「父进程池 × 每项一个**真子进程**」
     那条路）：`CULTIST@4`（单怪弱怪）、`GREMLIN_GANG@12`（多怪，会长出分裂）、
     `CULTIST@45`（**同一组怪换个地点**）
  2. 列：`enc`/`enc_floor` 每场唯一且与请求一致；`bid` 连续；每项场数 == `n_per`
  3. **地点真的进了初始状态**：同一组怪、同一个 seed、只换层 ⇒ 首行观测不同
  4. 真训 1 epoch（端到端）
     [续跑]（重跑 ⇒ 打印「复用已有分片」且分片 mtime 不变）。
  6. **`eps > 0`**：`eps=1.0` 下每步都改执行随机合法动作，而它多半**不在搜索访问表里**
     （曾在此 `StopIteration`）；且随机性只走本场 seed 派生的流 ⇒ 重跑逐位相同。

⚠️ 采集走**进程内**调用：子集只能这么给（CLI 是「整张表」这一条路，没有 `--encs` 开关）。
   每项的**子进程**仍是真的 ⇒ 退出码那层逻辑照旧被测到。
⚠️ 训练走**真子进程**：要验的是端到端的存盘产物。
"""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
from collections import Counter
from typing import Any

import numpy as np
import slaythespire as sts

from collect_encs import collect_encs

HERE = os.path.dirname(os.path.abspath(__file__))

# 子集：单怪弱怪 @Act1 前半 / 多怪（会长出分裂）@Act1 后半 / **同组怪换个地点**（Act3 后半）
PAIRS = ((sts.MonsterEncounter.CULTIST, 4),
         (sts.MonsterEncounter.GREMLIN_GANG, 12),
         (sts.MonsterEncounter.CULTIST, 45))
N_PER, SIMS, WORKERS = 2, 25, 2


def run(args, expect_ok=True, env=None):
    """跑一条命令并收输出。

    `args`：argv 列表（第一个元素是脚本名，用 `cwd=HERE` 解析）；
    `expect_ok=True` 时退出码非 0 直接报错退出；`False` 用来测**失败路径**；
    `env` 不给就继承本进程环境（旋钮一律走 `STS_*`）。
    返回 `CompletedProcess`（守卫的断言在 `returncode` / `stdout` 上）。
    """
    p = subprocess.run([sys.executable, "-u", *args], cwd=HERE, capture_output=True,
                       text=True, encoding="utf-8", errors="replace",
                       env={**os.environ, **(env or {})})
    if expect_ok and p.returncode != 0:
        raise SystemExit(f"✗ {' '.join(args)} 退出码 {p.returncode}\n"
                         f"--- stdout ---\n{(p.stdout or '')[-1500:]}\n"
                         f"--- stderr ---\n{(p.stderr or '')[-1500:]}")
    return p


def check(name, cond, extra=""):
    """打一行结果并断言：`name` = 断言名（人看的）、`cond` = 条件、`extra` = 失败时附带的现场。"""
    print(f"  {'✓' if cond else '✗'} {name}" + (f"  {extra}" if extra and not cond else ""))
    if not cond:
        raise SystemExit(f"✗ 前提不成立：{name} {extra}")


def capture(fn, *args, **kw) -> tuple[str, Any]:
    """跑 `fn` 并把它打印的东西收回来（采集/续跑的判据都在打印里）。返回 `(输出, 返回值)`。"""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        out = fn(*args, **kw)
    return buf.getvalue(), out


def main() -> int:
    # 进程内采集也要走「池 × 每项一个子进程」那条路（`--workers 2` 的等价写法）
    os.environ["STS_WORKERS"] = str(WORKERS)
    with tempfile.TemporaryDirectory() as td:
        cache = os.path.join(td, "encs.npz")
        print(f"[采集] {len(PAIRS)} 项 × {N_PER} 场 @ {SIMS} sims（workers={WORKERS}）")
        log, d = capture(collect_encs, N_PER, SIMS, encs=PAIRS, cache=cache)

        print("[缓存列]")
        d = {k: np.load(cache)[k] for k in np.load(cache).files}
        nb = np.bincount(d["bid"])
        first = np.unique(d["bid"], return_index=True)[1]
        per_enc, per_flr = d["enc"][first], d["enc_floor"][first]
        want = {(int(e), int(f)) for e, f in PAIRS}
        check("场次数 == 项数 × 每项场数", len(first) == len(PAIRS) * N_PER, len(first))
        check("每场只对应一个 (enc, enc_floor)",
              bool(np.all(d["enc"] == per_enc[d["bid"]]))
              and bool(np.all(d["enc_floor"] == per_flr[d["bid"]])), )
        got = {(int(e), int(f)) for e, f in zip(per_enc.tolist(), per_flr.tolist())}
        check("(遭遇, 层) 集合 == 请求的项集", got == want, (sorted(got), sorted(want)))
        cnt = Counter(zip(per_enc.tolist(), per_flr.tolist()))
        check("每项场数 == n_per", all(v == N_PER for v in cnt.values()), dict(cnt))
        check("每场至少一行", bool(np.all(nb > 0)))
        check("`bid` 连续 0..n-1",
              bool(np.array_equal(np.unique(d["bid"]), np.arange(len(first)))))

        print("[按地点发初始状态]")
        c = int(sts.MonsterEncounter.CULTIST)
        i4 = int(np.flatnonzero((per_enc == c) & (per_flr == 4))[0])
        i45 = int(np.flatnonzero((per_enc == c) & (per_flr == 45))[0])
        # 两项的第 0 场都从 `start_seed` 起 ⇒ **同一个 seed**，两边的差别只有层号
        check("首行观测不同（层真的改了初始状态）",
              not np.array_equal(d["obs"][first[i4]], d["obs"][first[i45]]))

        print("[端到端训练]")
        out = os.path.join(td, "net.pt")
        run(["train.py", "train", cache, "1", out, "--tb", "0"])
        check("训 1 epoch + 存盘成功", os.path.exists(out))

        print("[续跑]")
        parts = [os.path.join(os.path.splitext(cache)[0] + "_parts", f"{int(e)}@{int(f)}.npz")
                 for e, f in PAIRS]
        mt = [os.path.getmtime(p) for p in parts]
        log2, _ = capture(collect_encs, N_PER, SIMS, encs=PAIRS, cache=cache)
        check("⭐ 重跑复用已有分片（不重采）", "复用已有分片" in log2, log2[-400:])
        check("⭐ 分片没被重写（mtime 不变）",
              [os.path.getmtime(p) for p in parts] == mt)
        # 部分复用：删 1 片重跑 ⇒ 合并的必须是「有分片的**全集**」，不能只回本次实采的
        os.remove(parts[0])
        capture(collect_encs, N_PER, SIMS, encs=PAIRS, cache=cache)
        d2 = dict(np.load(cache))
        check("⭐ 部分复用不丢项（返回有分片的全集）",
              len(np.unique(d2["bid"])) == len(PAIRS) * N_PER,
              len(np.unique(d2["bid"])))

        print("[eps > 0]")
        # eps=1.0 ⇒ 每步都改执行**随机合法动作**，而它多半没被搜索访问过
        eps_a, eps_b = os.path.join(td, "eps_a.npz"), os.path.join(td, "eps_b.npz")
        capture(collect_encs, N_PER, SIMS, encs=PAIRS, cache=eps_a, eps=1.0)
        capture(collect_encs, N_PER, SIMS, encs=PAIRS, cache=eps_b, eps=1.0)
        da, db = dict(np.load(eps_a)), dict(np.load(eps_b))
        check("⭐ eps=1.0 全项都采得出来（随机动作不保证在访问表里）",
              len(np.unique(da["bid"])) == len(PAIRS) * N_PER, len(np.unique(da["bid"])))
        check("⭐ eps>0 同 seed 重跑逐位相同（随机流不看进程/调度）",
              set(da) == set(db) and all(np.array_equal(da[k], db[k]) for k in da))

    print("\n全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
