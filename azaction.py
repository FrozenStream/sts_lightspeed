"""动作空间 + 编码约定 + 终局读数 + 策略函数。

统一动作索引的权威在 C++ `bindings/bindings-util.cpp` 的 `actionToIdx`；这里是它唯一的 Python 映射。
**编不出来由 C++ 直接抛**，Python 侧不做兜底 —— 下标不合法就是当场崩。

⚠️ 本模块被 `aznet` 全量重导出 ⇒ 外部一律 `from aznet import POLICY_DIM, ...`。
"""
import random

import numpy as np
import torch

import slaythespire as sts
from azlayout import LAYOUT
from zspec import DEFAULT_SPEC, Terminal, z_from

# ---------------------------------------------------------------- 动作空间
_ACT = sts.action_layout()                  # 动作索引空间的结构（唯一来源 = C++ `slaythespire.h`）
POLICY_DIM = _ACT["count"]                  # 策略头宽度 = 手牌段 + 药水段 + END_TURN
END_TURN_IDX = _ACT["end_turn"]
HAND_STRIDE = _ACT["hand_stride"]           # 单张牌的槽宽 = [无目标, 怪槽0,1,2,3,4]
POTION_STRIDE = _ACT["potion_stride"]       # 单瓶药的槽宽 = [无目标, 怪槽0,1,2,3,4, 丢弃]
POTION_BASE = _ACT["potion_base"]           # 药水段起点 = 手牌段宽度

SELECT_DIM = LAYOUT["card_select_slots"]


def is_card_select_state(b: sts.BattleContext) -> bool:
    return b.input_state == sts.InputState.CARD_SELECT


def action_dim(b: sts.BattleContext) -> int:
    """当前状态下该用哪个头的维度。"""
    return SELECT_DIM if is_card_select_state(b) else POLICY_DIM


def legal_actions_and_idx(b: sts.BattleContext) -> list[tuple[sts.Action, int]]:
    """[(`动作`, 它在对应 logits 里的下标)]。"""
    return [(a, b.action_to_idx(a)) for a in b.enumerate_actions()]


def obs_tensor(b: sts.BattleContext) -> torch.Tensor:
    return torch.as_tensor(b.get_snapshot_np(), dtype=torch.float32).unsqueeze(0)


# ---------------------------------------------------------------- 终局读数
def terminal_state(b: sts.BattleContext) -> Terminal:
    """读**这一场**的终局记录（字段 = `zspec.Z_COLS`；唯一一份计算）。

    **一场一份**：正常打完就是真终局；`MAX_STEPS` 收场时读的是当时的状态（不是真终局）。
    `t_enemy` = 敌方剩余总血量占比；`t_escore` = 引擎终局分（`enginenorm` 的分子）。
    """
    cur = [m.cur_hp for m in b.monsters]
    mx = [m.max_hp for m in b.monsters]
    return Terminal(t_win=int(b.outcome == sts.Outcome.PLAYER_VICTORY),
                    t_hp=int(b.player_hp),
                    t_max_hp=int(b.player_max_hp),
                    t_turn=int(b.turn),
                    t_enemy=sum(cur) / max(1, sum(mx)),
                    t_escore=float(sts.evaluate_end_state(b)))


def terminal_value(b: sts.BattleContext, spec: str = DEFAULT_SPEC) -> float:
    """终局叶的价值 = `zspec.z_from` 在该口径下的 z。

    ⚠️ 口径必须与**网络头的训练口径**（`train.Z_SPEC`）相同：树里非终局叶由头给值，
    两个尺度混进同一个 `W/N` ⇒ PUCT 静默错权（`hp` 口径的败 = −1.0，`enginenorm` ≈ 0~0.2）。
    """
    return z_from(terminal_state(b), spec)


# ---------------------------------------------------------------- 策略
def random_action(b: sts.BattleContext) -> sts.Action | None:
    """合法动作里均匀抽一个。"""
    acts = b.enumerate_actions()
    return random.choice(acts) if acts else None


Got = tuple[np.ndarray, np.ndarray, bool, sts.Action, float | None]


def teacher_action(b: sts.BattleContext, sims: int, rng: random.Random, tau: float = 0.0, eps: float = 0.0,
                   exploration_parameter: float | None = None) -> Got:
    """跑一次 C++ UCT 搜索，返回本步标签。

    标签 = `(pi, mask, is_sel, action, root_win_rate)`，宽度 = `action_dim(b)`：
    `pi` 访问分布（未访问的下标为 0）/ `mask` 合法动作 / `action` 要执行的动作（调用方直接
    执行它）/ `root_win_rate` 根胜率。

    ⚠️ `sims ≥ 1` 是契约（`sims = 0` 搜索一次都不跑、根节点没有任何边 ⇒ 下文当场崩）。
    ⚠️ 只有**未终局**的战斗能跑搜索：终局状态下 `enumerate_actions()` 会在 C++ 里直接 abort。

    `tau` 采样温度（0 = 取 `argmax(pi)`）；`eps` 以该概率改**执行**一个随机合法动作（`pi`
    仍是访问分布；0 = 确定可复现）—— 该动作**可能没被搜索访问过**（`pi` 处为 0）。
    `exploration_parameter` 转给 C++ 的 UCB 常数（`None` = 引擎默认 3√2，调小 = 更偏利用）。
    """
    is_sel = is_card_select_state(b)
    dim = action_dim(b)
    stats = (b.search_stats(sims) if exploration_parameter is None
             else b.search_stats(sims, exploration_parameter))

    legal = list(b.enumerate_actions())
    mask = np.zeros(dim, dtype=bool)
    for a in legal:
        mask[b.action_to_idx(a)] = True

    # π 仍**只**来自搜索的访问分布；未访问的下标保持 0。
    # `visited` 只有**被搜索访问过**的动作 —— π 抽签落在它里面，但 `eps` 的随机动作不保证在（不得用它反查）。
    pi = np.zeros(dim, dtype=np.float32)
    visited: dict[int, sts.Action] = {}
    for a, n in stats.policy:
        if n > 0:
            i = b.action_to_idx(a)
            pi[i] += n
            visited[i] = a
    pi = pi / pi.sum()
    # 随机性一律走**注入的流**：采集按本场 seed 派生（与进程、调度无关 ⇒ 串行/并行逐位相同）。
    if eps > 0 and rng.random() < eps:                                       # 改执行一个随机合法动作（π 不变）
        action = legal[rng.randrange(len(legal))]
    elif tau <= 1e-8:
        action = visited[int(np.argmax(pi))]
    else:
        w = pi ** (1.0 / tau)
        action = visited[rng.choices(range(dim), weights=[float(x) for x in w])[0]]
    return pi, mask, is_sel, action, float(stats.root_win_rate)


def net_action(net: torch.nn.Module, b: sts.BattleContext) -> sts.Action | None:
    """裸网络贪心（只在合法动作里 argmax）。调用方负责 `net.eval()`。"""
    cand = legal_actions_and_idx(b)
    if not cand: raise ValueError("no legal actions")
    with torch.no_grad():
        p_act, p_sel, _ = net(obs_tensor(b))
    is_sel = is_card_select_state(b)
    logits = (p_sel if is_sel else p_act).squeeze(0)
    mask = torch.zeros(action_dim(b), dtype=torch.bool)
    for _, i in cand:
        mask[i] = True
    k = int(torch.argmax(logits.masked_fill(~mask, -1e9)))
    return next(a for a, i in cand if i == k)
