"""动作空间：策略头宽度（`POLICY_DIM`）与选牌屏宽度（`SELECT_DIM`）。

统一动作索引的权威在 C++ `collect/action.cpp` 的 `actionToIdx`（`sts.legal_actions_with_idx`
一次给全）—— Python 侧不复制那份算术。
⚠️ 本模块被 `aznet` 全量重导出 ⇒ 外部一律 `from aznet import POLICY_DIM, ...`。
"""
import slaythespire as sts
from azlayout import LAYOUT

POLICY_DIM = sts.action_layout()["count"]   # 策略头宽度 = 手牌段 + 药水段 + END_TURN
SELECT_DIM = LAYOUT["card_select_slots"]    # 选牌屏头宽度
