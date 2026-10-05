"""训练线程数守卫 —— 「同线程数跨进程**逐位相同**」，并把「换线程数 = 换轨迹」量出来。

用法：
    python _chk_threads.py                       # 合成小数据（自包含，~20 s）
    python _chk_threads.py data/_ab28.npz --epochs 64    # 真实缓存

判据（**会断言**，失败 ⇒ 退出码 1）：
    · 同一个线程数跑两个**独立进程** ⇒ 权重逐位相同（`sha`）。
      它挡住的是：未播种 RNG、`train/eval` 模式泄漏、归约顺序随调度变。

观测（**不断言**，只打印）：
    · 两个线程数之间的 `val_ce` 差与 `max|Δw|`。换线程数就换轨迹是**当前 torch 的行为**，
      不是契约 —— 将来 torch 若把两者做成一致，这条不该报红。

⚠️ 子进程模式（`--arm` / `--sd-out`）是内部接口，别手敲。
⚠️ 合成数据只为跑通训练环路（`obs` 全随机 ⇒ 学不到东西，`val_ce` 高是正常的）——
   这里比的是**两个进程的输出是否逐位相同**，不是模型好不好。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
import torch

import train
import slaythespire as sts
from aznet import OBS_DIM, POLICY_DIM
from zspec import Terminal
from collect import BattleRecord, Src, Task, assemble

# 子进程把结果打印在这行前缀后面（父进程只认它，其余输出照旧落 shell）。
ARM_MARK = "@@ARM@@"


def sha_of(sd) -> str:
    """逐位哈希：键名 + 形状 + 原始字节。"""
    h = hashlib.sha256()
    for k in sorted(sd):
        t = sd[k].detach().to("cpu").contiguous()
        h.update(k.encode())
        h.update(str(tuple(t.shape)).encode())
        h.update(t.numpy().tobytes())
    return h.hexdigest()[:16]


def make_synth(n_battles=48, per_battle=8, seed=0):
    """合成一份能过 `train()` 全部闸的缓存。

    走 `collect.assemble`（缓存 schema 的**唯一生产者**）—— 手搬键集会在 schema 变动后
    静默漂移，而这正是 `train()` 的闸要挡的那类错。
    终局列只要**内部自洽**：值目标由 `zspec.z_of` 现算，口径与造法无关；
    具体数字不重要 —— 下游只比**跨线程是否逐位相同**。
    """
    g = np.random.default_rng(seed)
    enc, floor = sts.MonsterEncounter.CULTIST, 4
    trajs = []
    for b in range(n_battles):
        win = bool(g.random() < 0.5)
        mhp = 80
        hp = int(g.integers(1, 81)) if win else 0
        traj = []
        for _ in range(per_battle):
            pi = np.zeros(POLICY_DIM, np.float32)
            pi[g.integers(0, POLICY_DIM)] = 1.0
            # 全部按「非 select 帧」造：那些行的 CE 贡献恒 0（`targets` 对全零行不动）
            traj.append((g.random(OBS_DIM, dtype=np.float32), pi,
                         np.ones(POLICY_DIM, bool), False))
        term = Terminal(t_win=int(win), t_hp=hp, t_max_hp=mhp, t_turn=0, t_enemy=0.0,
                        t_escore=100.0 * (35.0 + hp))
        trajs.append(BattleRecord(b, traj, term, int(enc), floor))
    return assemble(trajs, Task.ENCS, Src.TEACHER)


def run_arm(cache, epochs, threads, sd_out):
    """让一个**独立进程**以 `threads=<threads>` 训练一次，返回它打印的 metrics。"""
    cmd = [sys.executable, os.path.abspath(__file__),
           "--arm", str(threads), "--epochs", str(epochs), "--sd-out", sd_out]
    if cache:
        cmd.append(cache)
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=os.path.dirname(os.path.abspath(__file__)))
    for ln in (p.stdout or "").splitlines():
        if ln.startswith(ARM_MARK):
            return json.loads(ln[len(ARM_MARK):])
    raise RuntimeError(f"{threads} 线程的子进程没给出结果（exit={p.returncode}）：\n"
                       f"--- stdout ---\n{(p.stdout or '')[-1500:]}\n"
                       f"--- stderr ---\n{(p.stderr or '')[-1500:]}")


def arm(cache, epochs, threads, sd_out):
    """子进程模式：训练一次并打印结果（父进程靠 `ARM_MARK` 行读取）。"""
    data = ({k: np.load(cache)[k] for k in np.load(cache).files}
            if cache else make_synth())
    # `tb=False`：守卫是**对照实验**，不该往 `data/runs/` 里塞训练日志 —— 否则每个 arm
    # 一条曲线，还把「哪次是正式跑」搅模糊。线程数是**形参**（不再走 env）。
    net, m = train.train(data, epochs=epochs, threads=threads, tb=False, verbose=False)
    # ⚠️ `train()` 内部会 `set_num_threads(threads)` ⇒ 断言必须放在它**之后**，
    #    否则量的是别的东西。
    assert torch.get_num_threads() == threads, torch.get_num_threads()
    sd = {k: v.clone() for k, v in net.state_dict().items()}
    if sd_out:
        torch.save(sd, sd_out)
    print(ARM_MARK + json.dumps({"threads": threads, "sha": sha_of(sd),
                                 "val_ce": repr(float(m["val_ce"])),
                                 "train_ce": repr(float(m["train_ce"]))}), flush=True)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="训练线程数守卫", epilog=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cache", nargs="?", default=None,
                   help="缓存路径；不给就用合成小数据")
    p.add_argument("--epochs", type=int, default=4, help="训练轮数（真实缓存上取 64）")
    p.add_argument("--threads", type=int, nargs="*", default=[1, 8], help="要比的线程数")
    p.add_argument("--arm", type=int, default=None, help="内部：以该线程数训练一次")
    p.add_argument("--sd-out", default=None, help="内部：落盘权重供父进程比对")
    a = p.parse_args()
    if a.arm is not None:
        return arm(a.cache, a.epochs, a.arm, a.sd_out)

    ok = fail = 0
    recs = {}
    with tempfile.TemporaryDirectory() as td:
        for th in a.threads:
            runs = []
            for i in (1, 2):                      # 同线程数、两个独立进程
                sd_out = os.path.join(td, f"sd_t{th}_{i}.pt")
                runs.append((run_arm(a.cache, a.epochs, th, sd_out), sd_out))
            same = runs[0][0]["sha"] == runs[1][0]["sha"]
            ok, fail = (ok + 1, fail) if same else (ok, fail + 1)
            print(f"  {'✓' if same else '✗'} {th:2d} 线程：两个进程的权重 sha = "
                  f"{runs[0][0]['sha']} / {runs[1][0]['sha']}"
                  f"（val_ce {runs[0][0]['val_ce']}）", flush=True)
            recs[th] = runs[0]

        for x, y in zip(a.threads, a.threads[1:]):
            sx = torch.load(recs[x][1], map_location="cpu")
            sy = torch.load(recs[y][1], map_location="cpu")
            dw = max(float((sx[k].double() - sy[k].double()).abs().max()) for k in sx)
            print(f"  观测：{x} vs {y} 线程 —— val_ce {recs[x][0]['val_ce']} vs "
                  f"{recs[y][0]['val_ce']}、max|Δw| = {dw:.4g}"
                  f"（换线程数就换轨迹；不断言）", flush=True)

    print(f"\n{ok} 通过 / {fail} 失败", flush=True)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
