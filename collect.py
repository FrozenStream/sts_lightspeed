"""采集侧公共层：通用执行器 + 缓存两道闸。

采集脚本（`collect_encs` / `collect_selfplay`）是**触发器**，各自提供：

  · `task`       任务标识，决定指纹与可混训性；
  · `jobs(...)`  任务流，产出 `(seed, enc, floor)`；
  · 标签与执行策略（默认 = 引擎内的 C++ 教师，自己出招）。

本模块按任务流执行：造环境、逐步记录、并行、组装、验缓存、落盘。它不依赖 `train`。
环境由 `make_stage_battle(seed, enc, floor)` 唯一确定，触发器不参与造环境。

种子纪律：采集段起点 `COLLECT_SEED0`，评估段起点 `EVAL_SEED0`（本文件定义，
`eval_walk.py` 的种子闸读它），两段不得重叠。
"""
import os
import random
from enum import StrEnum
from typing import Any, Callable, Iterable, NamedTuple

import numpy as np
import slaythespire as sts
from tqdm import tqdm

from scene import CHARACTER, act_of, ascension_of, shape_floor
import runtime
from zspec import Terminal, Z_COLS, Z_DTYPES
from aznet import (Got, LAYOUT, MONSTER_FIELDS, MONSTER_OFF, MONSTER_SLOTS, OBS_DIM,
                   POLICY_DIM, SELECT_DIM, teacher_action, terminal_state)

COLLECT_SEED0 = 0
# 评估 / held-out 段起点。**1e8 只是「远离采集段」，不是有语义的边界**；
# `eval_walk.py` 拒绝低于它的 `seed0`（被训练见过的种子不能当评估），
# 采集脚本用 `--seed0` 往高段排。
EVAL_SEED0 = 100_000_000
MAX_STEPS = 1000

# 标签：`label(b) -> (pi, mask, is_sel, action, root_win_rate)`，
# 返回的动作就是本步要执行的动作（记什么就出什么）。
# `pi` / `mask` 的宽度随 `is_sel` 走（`POLICY_DIM` / `SELECT_DIM`）；`root_win_rate` 可为 None。
Label = Callable[[sts.BattleContext], Got]

# 一场的任务：`(场景种子, 遭遇, 该遭遇的出现层)`。
Job = tuple[int, sts.MonsterEncounter, int]
# 一场的逐决策点记录：`(obs, pi, mask, is_sel)`。
Traj = list[tuple[np.ndarray, np.ndarray, np.ndarray, bool]]

# 缓存里两个分类列的**取值域**（唯一定义点）。用 `StrEnum` ⇒ 成员本身就是 `str`：
# `d[Src.TEACHER]` 与 `d["teacher"]` 等价（hash/eq 走 str）、f-string 直接拼、
# `np.savez(**{…})` 落盘仍是裸字符串 ⇒ 换枚举**不改缓存字节**、不作废数据。


class Task(StrEnum):
    ENCS = "encs"        # 按怪组 × 分布地点（`collect_encs` / `collect_selfplay`）


class Src(StrEnum):
    TEACHER = "teacher"  # 教师 UCT 搜索
    MCTS = "mcts"        # 网络自己的 MCTS（自对弈）


class BattleRecord(NamedTuple):
    seed: int
    traj: Traj
    term: Terminal           # 终局读数（`Z_COLS` 那 6 项）
    enc: int                 # 遭遇 id
    floor: int               # 该遭遇的出现层


def make_stage_battle(seed: int, enc: sts.MonsterEncounter, floor: int) -> tuple[sts.GameContext, sts.BattleContext, sts.MonsterEncounter]:
    """按层造一局，返回 `(gc, b, enc)`；`floor` 决定发哪一段的牌组/升级/遗物/药水/hp。

    参数：
      `seed`  场景种子，同时是 `shape_floor` 的 rng 种子（同组不同 seed 才有差异）
              与**进阶等级**的抽签种子（`scene.ascension_of`：A10+ 才带 `ASCENDERS_BANE`）
      `enc`   `sts.MonsterEncounter` 成员
      `floor` 该遭遇的出现层，`act_of` 据此设 `gc.act`

    遭遇只能从 `b.init(gc, enc)` 传入：`gc.encounter` 是只读属性。
    """
    gc = sts.GameContext(CHARACTER, seed, ascension_of(seed))
    gc.floor_num = int(floor)
    gc.act = act_of(floor)
    shape_floor(gc, seed)
    b = sts.BattleContext()
    b.init(gc, enc)
    return gc, b, enc


def run_battle(seed: int, enc: sts.MonsterEncounter, floor: int, label: Label) -> BattleRecord:
    """跑一场并逐决策点记录，返回 `BattleRecord`。

    `label(b)` 给出该步的策略目标，记完就执行 `got[3]`；循环到 `is_battle_over()` 或
    `MAX_STEPS` 为止（撞 `MAX_STEPS` 时终局列读的是当时的状态，不是真终局）。
    """
    _, b, _ = make_stage_battle(seed, enc, floor)
    traj, steps = [], 0
    while not b.is_battle_over() and steps < MAX_STEPS:
        got = label(b)
        traj.append((b.get_snapshot_np().copy(), got[0], got[1], got[2]))
        got[3].execute(b)
        steps += 1
    return BattleRecord(seed, traj, terminal_state(b), int(enc), int(floor))


# ---------------------------------------------------------------- 教师（默认策略）

def make_teacher(sims: int, eps: float = 0.0) -> Callable[[Job], BattleRecord]:
    """返回「用这套参数跑一场」的函数；`job` = `(seed, enc, floor)`。"""
    def battle(job: Job) -> BattleRecord:
        seed, enc, floor = job
        rng = random.Random(seed * 1000003 + 11)
        return run_battle(seed, enc, floor,
                          lambda b: teacher_action(b, sims, rng, eps=eps))
    return battle


# ---------------------------------------------------------------- 批量执行

def collect_jobs(jobs: list[Job], worker: Callable[[Job], BattleRecord], desc: str = "采集",
                 initializer: Callable[..., None] | None = None, initargs: tuple = (),
                 verbose: bool = True, workers=None,
                 mp_start=None) -> tuple[list[BattleRecord], int]:
    """串行或并行跑完 `jobs`，返回 `(records, wins)`；`wins` = 胜场数（`t_win`）。

    `runtime.workers(workers) > 1` 且任务不只一场时走进程池。**`workers` / `mp_start` 由调用方
    显式给**（`None` = 按 cgroup 配额 / 自动选启动方式）。并行要求 `worker` 与 `initializer`
    都能在子进程里按名字找到（模块级函数；参数用 `initializer` 装）。
    结果按 `(seed, floor, enc)` 排序 ⇒ 串行与并行逐位相同。
    """
    w = runtime.workers(workers)
    if w and w > 1 and len(jobs) > 1:
        with runtime.mp_context(mp_start).Pool(min(w, len(jobs)), initializer=initializer,
                                            initargs=initargs) as pool:
            out = _drain(pool.imap_unordered(worker, jobs, chunksize=1), len(jobs),
                         desc, verbose)
    else:
        if initializer is not None:
            initializer(*initargs)          # 串行路径也要装（并行时由池装）
        out = _drain((worker(j) for j in jobs), len(jobs), desc, verbose)
    out.sort(key=lambda r: (r.seed, r.floor, r.enc))
    return out, sum(int(r.term.t_win > 0) for r in out)


def _drain(it: Iterable[BattleRecord], total: int, desc: str,
           verbose: bool) -> list[BattleRecord]:
    """收进度条（完成序），返回结果列表。"""
    out, wins = [], 0
    with tqdm(total=total, desc=desc, unit="场", disable=not verbose,
              dynamic_ncols=True, mininterval=0.4) as pg:
        for r in it:
            out.append(r)
            wins += int(r.term.t_win > 0)
            pg.set_postfix_str(f"胜率 {wins / len(out):.1%}", refresh=False)
            pg.update()
    return out


# ---------------------------------------------------------------- 组装

def assemble(trajs: list[BattleRecord], task: Task, src: Src,
             extra: dict[str, Any] | None = None) -> dict[str, Any]:
    """逐场轨迹 → 训练数组。`trajs` 每项 = `run_battle` 的记录。

    `task` 写进 `task` 列、`src` 记标签来源（都取 `Task` / `Src` 成员，见上），
    两列都不进指纹；`extra` 为附加标量列。

    返回键：`obs` / `pi_act` / `m_act` / `pi_sel` / `m_sel` / `bid` / `seed` / `enc` /
    `enc_floor` / `Z_COLS` 全部终局列，加 `space` / `obs_dim` / `task` / `src`。
    键名按空间命名（`act` = POLICY_DIM，`sel` = SELECT_DIM），不带维数。
    层与幕不另存列：层在 `enc_floor`，幕由 `scene.act_of(层)` 派生。
    **不写 `z`**：值目标由 `zspec.z_from` 从终局列现算（口径是训练侧的选择，不是数据）。
    """
    obs_l, pi_act_l, m_act_l, pi_sel_l, m_sel_l, bid_l = ([] for _ in range(6))
    seed_l: list[int] = []
    enc_l: list[int] = []
    flr_l: list[int] = []
    term_l: list[Terminal] = []
    for i, r in enumerate(trajs):
        for obs, pi, mask, is_sel in r.traj:
            obs_l.append(obs)
            pi_act_l.append(np.zeros(POLICY_DIM, np.float32) if is_sel else pi)
            m_act_l.append(np.zeros(POLICY_DIM, bool) if is_sel else mask)
            pi_sel_l.append(pi if is_sel else np.zeros(SELECT_DIM, np.float32))
            m_sel_l.append(mask if is_sel else np.zeros(SELECT_DIM, bool))
            # 终局列**逐行**存（同一场的每一帧都带同一份 `Terminal`）—— 下面按列转置。
            term_l.append(r.term)
            bid_l.append(i)
            seed_l.append(r.seed)
            enc_l.append(r.enc)
            flr_l.append(r.floor)
    out: dict[str, Any] = dict(
        obs=np.asarray(obs_l, dtype=np.float32),
        pi_act=np.asarray(pi_act_l, dtype=np.float32),
        m_act=np.asarray(m_act_l),
        pi_sel=np.asarray(pi_sel_l, dtype=np.float32),
        m_sel=np.asarray(m_sel_l),
        bid=np.asarray(bid_l),
        # seed 必存：bid 只是批次内下标，两份数据共用种子段时同一场会落到 val 两侧。
        seed=np.asarray(seed_l),
        enc=np.asarray(enc_l, dtype=np.int32),
        enc_floor=np.asarray(flr_l, dtype=np.int32),
        # 动作空间标记：挡“缓存 81 维、代码 96 维”这种形状看不出的语义错配。
        space=np.array(POLICY_DIM),
        # 观测宽度标记：`space` 只管动作空间，布局改动它看不出来。
        obs_dim=np.array(OBS_DIM),
        task=np.array(task), src=np.array(src))
    # 终局列：`Terminal` 逐行存 ⇒ 按字段转置（列序 == `Z_COLS` == 字段序），dtype 同源。
    out.update({k: np.asarray([getattr(t, k) for t in term_l], dtype=Z_DTYPES[k])
                for k in Z_COLS})
    if extra:
        out.update(extra)
    return out


# ---------------------------------------------------------------- 复用与混训

def load_cache(cache: str | None, src: Src) -> dict[str, Any] | None:
    """缓存已存在则过闸后直接返回，否则 `None`。

    `src` 必须与被复用缓存的标签来源一致：否则会把上一轮的自对弈产物当成教师采集续跑。
    """
    if not (cache and os.path.exists(cache)):
        return None
    d = np.load(cache)
    check_cache_obs(d, f"复用 {cache}")
    got = d.get("src")
    if got is None or str(got) != str(src):
        raise RuntimeError(f"{cache} 的 src={got} ≠ {src}")
    print(f"从缓存加载 {cache}（{len(d['obs'])} 样本）", flush=True)
    return {k: d[k] for k in d.files}


def coverage_report(data: dict[str, Any]) -> None:
    """打印多怪覆盖度：样本级、场级、场级怪物数分布。"""
    mon = data["obs"][:, MONSTER_OFF:MONSTER_OFF + MONSTER_FIELDS * MONSTER_SLOTS:MONSTER_FIELDS]
    n_mon = (mon > 0).sum(1)
    first = np.unique(data["bid"], return_index=True)[1]      # 每场首行足够代表该场
    nb = n_mon[first]
    hist = {int(k): int(v) for k, v in zip(*np.unique(nb, return_counts=True))}
    print(f"  多怪覆盖：样本级 {float((n_mon > 1).mean()):.1%} / "
          f"场级 {float((nb > 1).mean()):.1%}", flush=True)
    print(f"  场级怪物数分布：{dict(sorted(hist.items()))}", flush=True)


# ---------------------------------------------------------------- 缓存闸

LAYOUT_PROBE = 3        # 布局对账的抽查场数


def block_of(i: int) -> str:
    """快照下标 → 所在块的键名（`*_off` 把布局切成连续块）。"""
    offs = sorted((v, k) for k, v in LAYOUT.items() if k.endswith("_off"))
    for j, (o, k) in enumerate(offs):
        end = offs[j + 1][0] if j + 1 < len(offs) else LAYOUT["len"]
        if o <= i < end:
            return k
    return f"越界({i})"


def check_cache_obs(d: Any, where: str) -> None:
    """缓存必须**由当前二进制采集**：宽度相符 + 抽查几场**逐位重放对账**。

    只看 `obs_dim` 挡不住「列语义漂移」：快照是定长块，而 `snapshot_layout()` 里只有**块宽、
    偏移与取值域** —— 交换块内两列（如把 `intentMoveId` 提到 `curHp` 前面）时，宽度、键、
    `space`、**任何布局指纹**全都不动。所以必须真拿二进制重算一遍：

    缓存每场的**第 0 行 = 该场初始状态**，用缓存自带的 `seed`/`enc`/`enc_floor` 重建场景
    （`make_stage_battle` 是造环境的唯一入口）后与当前二进制的快照**逐位相同**才算通过。
    缺那三列 = 无法验证 ⇒ 直接拒（没有兜底）。
    """
    got = d.get("obs_dim")
    if got is None or int(got) != OBS_DIM:
        raise RuntimeError(f"{where}: obs_dim={got} 当前={OBS_DIM}")
    obs, bid = np.asarray(d["obs"]), np.asarray(d["bid"])
    if obs.shape[1] != OBS_DIM:
        raise RuntimeError(f"{where}: obs 宽 {obs.shape[1]} 当前={OBS_DIM}")
    miss = [k for k in ("seed", "enc", "enc_floor") if k not in d]
    if miss:
        raise RuntimeError(f"{where}: 缺 {miss} ⇒ 无法重放场景验证布局（旧格式，需重新采集）")
    nb = int(bid.max()) + 1
    first = np.full(nb, -1, dtype=np.int64)
    for i, b in enumerate(bid):
        if first[b] < 0:
            first[b] = i
    if (first < 0).any():
        raise RuntimeError(f"{where}: {int((first < 0).sum())} 场在缓存里一行都没有")
    rng = np.random.default_rng(0)                     # 抽查固定种子 ⇒ 失败可复现
    for b in rng.choice(nb, size=min(LAYOUT_PROBE, nb), replace=False):
        j = int(first[b])
        enc, floor = sts.MonsterEncounter(int(d["enc"][j])), int(d["enc_floor"][j])
        snap = make_stage_battle(int(d["seed"][j]), enc, floor)[1].get_snapshot_np()
        diff = np.flatnonzero(snap.astype(np.float32) != obs[j].astype(np.float32))
        if diff.size:
            blocks = sorted({block_of(int(i)) for i in diff})
            raise RuntimeError(
                f"{where}: 场 {int(b)}（seed {int(d['seed'][j])} {enc.name}@{floor}）与当前"
                f"二进制不一致 —— 不同 {diff.size} 项，落在 {blocks} ⇒ 这份缓存不是当前"
                f"二进制采的")


def check_action_space(data: Any, where: str = "") -> None:
    """缓存必须带 `space` 标记且等于 `POLICY_DIM`。

    81→96 只改了语义（`hand*5+t` → `hand*6+t`），形状对不上会当场崩，语义对不上不会。
    """
    got = data.get("space")
    if got is None:
        raise RuntimeError(f"{where} 缓存没有动作空间标记（81 维旧数据），"
                           f"当前 POLICY_DIM={POLICY_DIM}；用 `_remap_space.py` 重排")
    if int(got) != POLICY_DIM:
        raise RuntimeError(f"{where} 动作空间不一致：缓存={int(got)} 当前={POLICY_DIM}；"
                           f"用 `_remap_space.py` 重排，或重新采集")


# ---------------------------------------------------------------- 混训

# 0 维标量列：不能 concatenate，拼接时跳过，取被掺入侧。
_SCALARS = ("space", "obs_dim", "task", "src")


def mix_caches(a: dict[str, Any], b: dict[str, Any],
               where: str = "") -> dict[str, Any]:
    """把两份同任务缓存拼成一份：`a` 在前，`b` 的 `bid` 整体错开。

    `bid` 必须错开：两边都从同一个种子段起点编号，直接拼会让 `battle_split` 把两个不同
    来源的场当成同一场（val 实际占比翻倍）。
    键集合必须相同：`b` 有而 `a` 没有的键说明是另一套格式，报错而不是硬拼。
    """
    for k in ("space", "obs_dim", "task"):
        if k in a and k in b and str(a[k]) != str(b[k]):
            raise RuntimeError(f"{where}{k} 不一致：A={a[k]} vs B={b[k]}，不是同一任务")
    miss = set(b) - set(a) - set(_SCALARS)
    if miss:
        raise RuntimeError(f"{where}B 多出 A 没有的键 {sorted(miss)}：两边不是同一套 schema")
    n_a = len(np.unique(a["bid"]))
    off = int(np.asarray(a["bid"]).max()) + 1
    out: dict[str, Any] = {}
    for k in b:
        if k in _SCALARS:
            continue
        out[k] = (np.concatenate([a[k], b[k] + off]) if k == "bid"
                  else np.concatenate([a[k], b[k]]))
    for k in _SCALARS:
        if k in b:
            out[k] = b[k]
    got = len(np.unique(out["bid"]))
    want = n_a + len(np.unique(b["bid"]))
    assert got == want, f"bid 错开失败：得到 {got} 场，应为 {want} 场"
    print(f"  {where}拼接：A {len(a['obs'])} 样本/{n_a} 场 + B {len(b['obs'])} 样本/"
          f"{want - n_a} 场 → {len(out['obs'])} 样本/{got} 场（B 占 "
          f"{len(b['obs']) / len(out['obs']):.1%}）", flush=True)
    return out
