"""值目标 `z` 的**各种口径** —— 全仓唯一的定义点。

⚠️ **换口径 = 换任务**。默认 = `DEFAULT_SPEC`（`hp` = 胜则剩余血量比例、败则 −1）。
`engine` / `lossshape` / `hpw` 三个口径已**删**（前两个是 `hp` 的手改变体、都被实验证否）。

⚠️ 口径**只依赖终局列**（`Terminal` / `Z_COLS`）⇒ 换口径**既不必重采、也不必重放**：
`collect.CollectData.z(spec)` 从数据的终局列现算，这里只定义"怎么算"。

📌 `q = W/N` 的**尺度由值口径决定**，而 PUCT 的 `c_puct` 是针对那个尺度定的常数
⇒ 换口径必须一起换。现在由 `Z_SCALE` / `c_puct_for(spec)` 保证，不再靠人记（`aznet`
的 `c_puct=None` 就走它）。
"""
from typing import NamedTuple

import numpy as np


class Terminal(NamedTuple):
    """**一场**战斗的终局记录：字段名 == 落盘列名、顺序 == 列序（双方唯一的契约，C++ 也指这里）。

    类型按字段本义：计数 / HP / 回合是 `int`，比例与引擎分是 `float`。**只有一种粒度**：
    一场一份；落盘里逐行存只是广播（`CollectData.seg` 正是靠"同一场逐行相同"认出场的）。
    """
    t_win: int          # 1 = 玩家胜
    t_hp: int           # 玩家剩余 HP
    t_max_hp: int
    t_turn: int         # 结束时的回合数
    t_enemy: float      # 敌方剩余总血量占比
    t_escore: float     # 引擎终局分（`BattleScumSearcher2::evaluateEndState`）


Z_COLS = Terminal._fields

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

# 根节点 Dirichlet 噪声的**总浓度**（不是每动作的 α：α = 总浓度 / 合法动作数）。
# 10.83 = AGZ 论文的 0.03 × 围棋的 ~361 个合法动作 —— 也就是说**它跟着动作数走**，
# 换引擎（本引擎每帧合法动作 mean 4.7）必须重标定，别照抄。
# ⚠️ 它原来是 `azsearch.py` 的模块常量；那个文件 2026-10-11 删掉（搜索只留 C++ 一份），
#    搬到这里与 `c_puct` / `q_init` 同住 —— 三者都是"搜索旋钮 × 值口径"的常数。
DIRICHLET_TOTAL = 10.83


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


def z_of(cols: dict[str, np.ndarray], spec: str) -> np.ndarray:
    """值目标 `z`：`cols` = 终局列（`Z_COLS` 的名字 → 数组）。只读终局列。

    元素级实现 ⇒ 传长度 1 的数组就是"一场"，传整列就是整份数据。`-1.0` 表示"没赢"。
    """
    win = np.asarray(cols["t_win"]) > 0
    max_hp = np.maximum(1.0, np.asarray(cols["t_max_hp"]))
    ratio = np.asarray(cols["t_hp"]) / max_hp
    if spec == "enginenorm":
        return np.asarray(cols["t_escore"]) / (100.0 * max_hp)
    if spec == "hp":
        return np.where(win, ratio, -1.0)
    if spec == "hp_turn":
        return np.where(win, ratio - 0.000125 * np.asarray(cols["t_turn"]), -1.0)
    raise KeyError(f"unknown spec {spec!r}")
