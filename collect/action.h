//
// 动作层（实现见 `action.cpp`）：枚举 / 编码 / 反解 / 展开。
//
// ⚠️ 段宽常量（`handStride` / `potionStride` / `potionBase` / `endTurnIdx` /
//    `unifiedActionCount`）**唯一一份**在 `bindings/slaythespire.h` 的 `sts::py` 里
//    （那里同时是快照布局的宿主）⇒ 本头不复制它们，实现体里按全限定名引用。
//
// ⚠️ 多选屏（`EXHAUST_MANY` / `GAMBLE`）在本层**没有任何特殊处理**：触发它们的遗物 / 药水 /
//    卡（`GAMBLING_CHIP` / `ELIXIR_POTION` / `GAMBLERS_BREW` / 无色 `PURITY`）由
//    `scenebuild.cpp` 的 ban 表挡在配方之外（造局面 `BANNED_RELICS` / `BANNED_POTIONS` /
//    `BANNED_CARDS`；整局每个非战斗动作后 `applyRunBans`）⇒ 防御是**"不让它进局面"**，
//    不是"进来后就地消化"（2026-10-20 删掉了那套运行时消化，见 `git log`）。
//
#ifndef STS_LIGHTSPEED_COLLECT_ACTION_H
#define STS_LIGHTSPEED_COLLECT_ACTION_H

#include <memory>
#include <utility>
#include <vector>

#include "collect.h"     // Frame / GameOut（采集侧通用结构）
#include "snapshot.h"    // maxMonsters（容量常量的唯一来源之一）

#include "sim/search/Action.h"

namespace sts::collect
{

    // 当前状态的**全部**合法动作，按枚举序（= 引擎的 `root.edges` 插入序）。
    std::vector<search::Action> enumerateActions(const BattleContext &bc);

        // 合法动作 + 它们各自的统一下标（**一次调用给全**）。
        //
        // 为什么要有这个出口：策略侧（裸网络贪心）只需要"在合法动作里取 logits 最大的那个"，
        // 而它要按下标去索引 logits ⇒ 需要"动作 ↔ 下标"的配对。逐动作调 `actionToIdx` 是
        // n 次跨语言往返，而且 Python 侧一旦自己算下标就多了一份会说谎的算术。
        // ⚠️ 返回值里的下标与 `actionToIdx` **同口径**（编不出来就抛）。
        std::vector<std::pair<search::Action, int>> legalActionsWithIdx(const BattleContext &bc);

    // 动作 → 统一动作下标。**编不出来就抛**（不返回 -1：`-1` 在 numpy 里是合法的末位下标，
    // 会静默污染标签）。
    int actionToIdx(const BattleContext &bc, const search::Action &a);

    // 一次展开：枚举 + 逐个 clone/execute/编码（MCTS 每步展开都走它）。两个列表同序；
    // 下标按 `actionToIdx`（编不出来就抛）。
    std::pair<std::vector<std::pair<int, search::Action>>,
              std::vector<std::unique_ptr<BattleContext>>>
    expandChildren(const BattleContext &bc);

    // 统一动作下标 → 动作（非法下标返回 default-constructed Action；调用方自己 `isValidAction`）。
    search::Action idxToAction(const BattleContext &bc, int idx);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_COLLECT_ACTION_H
