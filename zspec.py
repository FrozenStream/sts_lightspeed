"""值目标 `z` 的**各种口径** —— 全仓唯一的定义点。

为什么要单独一个模块：`z_revalue.py`（重放核对）与训练侧（`train.train`）都要用同一个
`z_from`，而**两边谁都不该 import 对方** —— 放任何一边都会成环。这里只依赖 numpy。

⚠️ **换口径 = 换任务**。默认 = `DEFAULT_SPEC`（`enginenorm` = 引擎 `evaluateEndState`
除以 `100 * maxHp`）。`engine` / `lossshape` / `hpw` 三个口径已**删**（前两个是 `hp` 的
手改变体、都被实验证否）。

⚠️ 口径**只依赖终局列**（`Terminal` / `Z_COLS`），而**缓存不存 `z`** —— z 一律现算
（`z_of` + `z_from`）。终局由缓存的 π 的 argmax 决定 ⇒ 换口径**既不必重采，
也不必重放**。

📌 `q = W/N` 的**尺度由值口径决定**，而 PUCT 的 `c_puct` 是针对那个尺度定的常数
⇒ 换口径必须一起换。现在由 `Z_SCALE` / `c_puct_for(spec)` 保证，不再靠人记（`azsearch`
的 `c_puct=None` 就走它）。
"""
from typing import NamedTuple

import glob
import json
import os

import numpy as np


class Terminal(NamedTuple):
    """**一场**战斗的终局记录。字段名 == 缓存列名（`Z_COLS` 由它派生）—— 只有一份定义。

    类型按字段本义：计数 / HP / 回合是 `int`，比例与引擎分是 `float`（缓存列 dtype 同源，
    见 `Z_DTYPES`）。**只有一种粒度**：一场一份；缓存里逐行存只是广播，不改变这一点。
    """
    t_win: int          # 1 = 玩家胜
    t_hp: int           # 玩家剩余 HP
    t_max_hp: int
    t_turn: int         # 结束时的回合数
    t_enemy: float      # 敌方剩余总血量占比
    t_escore: float     # 引擎终局分（`BattleScumSearcher2::evaluateEndState`）


Z_COLS = Terminal._fields

# 终局列里的胜负列在**行**中的下标（行布局 = `("seed",) + Z_COLS`，与 `eval_encs.ROW_COLS` 同源）。
I_T_WIN = ("seed",) + Z_COLS
I_T_WIN = I_T_WIN.index("t_win")


def baseline_win_rate(dir_: str = "data/baseline") -> tuple[str, float] | None:
    """教师基准胜率 → `(策略标签, 胜率)`；取目录下最新一份 json（没有则 `None`）。

    读的是门控同款 json（`items[*].per_spec[spec].rows`，行 = `("seed",) + Z_COLS`）
    ⇒ 胜率 = `t_win` 的均值。放在本模块，是因为那条行布局只有这里有定义。
    """
    fs = sorted(glob.glob(os.path.join(dir_, "*.json")), key=os.path.getmtime)
    if not fs:
        return None
    with open(fs[-1], encoding="utf-8") as fh:
        d = json.load(fh)
    if not isinstance(d, dict) or "items" not in d:      # 旧 walk schema
        return None
    spec = d["specs"][0]
    rows = [r for it in d["items"] for r in it["per_spec"][spec]["rows"]]
    return spec, sum(r[I_T_WIN] for r in rows) / max(len(rows), 1)

# 缓存列的 dtype：从字段类型派生（`int` 字段不存成 float64）—— 改字段不会漏改这里。
Z_DTYPES = {f: np.int64 if t is int else np.float64
            for f, t in Terminal.__annotations__.items()}

DEFAULT_SPEC = "hp"

# 各口径下 `z` 的**尺度**（标准差）。PUCT 的 `c_puct` 是针对 Q 的尺度定的常数，换口径
# **必须**一起换 —— 同样的 `c_puct` 在不同口径下的等效探索强度不同（踩过一次）。
# 数字在 63,000 场（`data/encdata_n1000_s5000_eps0.1_start_seed0_fpafc96aec.npz`）上实测；
# 换口径 / 换数据分布后要重测。注意「落差」（赢局 mean − 败局 mean）与 std 不同：
# 实测 enginenorm 落差 0.915（旧注释里的 0.477 是 40 场的旧值）。
Z_SCALE = {"enginenorm": 0.420, "hp": 0.618, "hp_turn": 0.618}

# 各口径下 `z` 的**区间中点**（实测 min/max 的中点）。
# 用途：MCTS 里**未访问节点**的 Q 初值。PUCT 的约定是「未访问 = 中性」，而中性到底是
# 哪个数由值口径定 —— 围棋 z∈[-1,1] 时是 0，而 `enginenorm` 的 z 实测 -0.001~1.573、
# 赢局典型 0.916 ⇒ 取 0 等于把未访问节点判成“最差”，已访问的兄弟白拿 ~0.9 的优势
# ⇒ **探索被压制**。换口径必须一起换这个初值。
Z_MID = {"enginenorm": 0.786, "hp": 0.0, "hp_turn": 0.0}

C_PUCT_BASE = 1.5       # `c_puct` 的基准值
C_PUCT_REF = 0.618      # 基准值是在这个 Q 尺度下定的（`hp` 口径）


def c_puct_for(spec: str) -> float:
    """把 `C_PUCT_BASE` 折算到 `spec` 的 Q 尺度：使「等效探索强度」不随值口径漂。

    `u = q + c·P·√ΣN/(1+N)` 里两项必须同尺度才可加。直接对 q 乘常数是无效变换
    （整体缩放不改变 argmax），要改的是 `c`。
    """
    if spec not in Z_SCALE:
        raise KeyError(f"未知口径 {spec!r}（可选：{sorted(Z_SCALE)}）")
    return C_PUCT_BASE * Z_SCALE[spec] / C_PUCT_REF


def q_init_for(spec: str) -> float:
    """`spec` 下未访问节点的 Q 初值（该口径的区间中点）。"""
    if spec not in Z_MID:
        raise KeyError(f"未知口径 {spec!r}（可选：{sorted(Z_MID)}）")
    return Z_MID[spec]


def z_from(t: Terminal, spec: str) -> float:
    """**一场**战斗的值目标。只读终局列。"""
    win = t.t_win > 0
    ratio = t.t_hp / max(1.0, t.t_max_hp)
    if spec == "enginenorm":
        return t.t_escore / (100.0 * max(1.0, t.t_max_hp))
    if spec == "hp":
        return ratio if win else -1.0
    if spec == "hp_turn":
        return (ratio - 0.000125 * t.t_turn) if win else -1.0
    raise KeyError(f"unknown spec {spec!r}")


def z_of(d, spec: str) -> np.ndarray:
    """整份缓存的值目标：**逐场算一次**，再按 `bid` 广播回逐行。

    前提（`collect.assemble` 保证，不成立就当场报错，不当作默认）：`bid` 升序且每场
    连续、终局列在同一场内逐行相同 —— 否则「取首行 + 广播」会静默算错。
    """
    bid = np.asarray(d["bid"])
    if bid.size and (np.diff(bid) < 0).any():
        raise ValueError("bid 不是升序 ⇒ 逐行广播不成立（不是 assemble 拼出来的？）")
    first = np.unique(bid, return_index=True)[1]          # 每场首行（bid 升序 ⇒ 已有序）
    counts = np.bincount(bid)
    for c in Z_COLS:
        if c not in d:
            raise KeyError(f"缓存缺终局列 {c!r} ⇒ 不是当前 schema（需要重采）")
        v = np.asarray(d[c], dtype=np.float64)
        if not np.array_equal(v, np.repeat(v[first], counts)):
            raise ValueError(f"{c} 在同一场内不逐行相同 ⇒ 终局列不是按场复制的")
    zs = [z_from(Terminal._make(d[c][i] for c in Z_COLS), spec) for i in first]
    return np.repeat(zs, counts)
