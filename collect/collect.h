//
// 采集侧的**通用结构**（自对弈与教师共用）—— 数据结构 + 两个共用工具。
//
// 放这里的判据：**两个采集器都碰它**。`Frame` / `Term` 是两者的结果行（自对弈记录一棵树
// 每步的根观测/访问分布/执行动作，教师记录 UCT 的同一批列），列名与 `collect/shardio.h`
// 的落盘列、Python `zspec.Terminal` **同名同序**。`GameOut`（结果行 + 终局）也在这里。
//
// `readTerm` / `snapshotObs` 也在这儿（定义在 `selfplay.cpp`）：两个采集器都要用，
// **谁都不该为了用它去 include 对方**（教师曾因此 include `selfplay.h`）。
//
// ⚠️ 观测宽度用 `Snap::snapshotLen`（同目录 `snapshot.h`，本头就在 `sts::collect` 里）。
//    （历史上有个 `sts::py::kSnapshotLen` 别名 —— 2026-10-11 目录名 = 命名空间名之后已经没有它了。）
// ⚠️ `std::array` **必须写全第二个模板参数**：它是 `template<class T, size_t N>` 的聚合、
//    没有推导指引 ⇒ `std::array<float>` 编不过。
//
#ifndef STS_LIGHTSPEED_COLLECT_H
#define STS_LIGHTSPEED_COLLECT_H

#include <array>
#include <cstdint>
#include <vector>

#include "snapshot.h" // Snap::snapshotLen（本头直接用 `Snap::snapshotLen`，不再经 `kSnapshotLen`）
#include "combat/BattleContext.h"

namespace sts::collect
{

    // 一个决策点的记录（= Python `run_battle` 往 `Traj` 里追加的那一行）。
    struct Frame
    {
        std::array<float, Snap::snapshotLen> obs; // 该决策点的根观测（`snapshotOf` 的原样输出）
        std::vector<int> pi_idx;                  // 根边（统一下标）…
        std::vector<int64_t> pi_n;                // …与它的访问数（**未归一**；调用方再归一）
        std::vector<int> legal;                   // 该决策点**全部**合法统一下标（= 掩码）
        bool is_sel = false;                      // 是否 CARD_SELECT 空间（决定 π/mask 的宽度）
        int a_idx = -1;                           // 实际执行的统一下标
    };

    // 一场战斗的终局读数：字段与 Python `zspec.Terminal` **同名同序**（缓存列由它派生）。
    struct Term
    {
        std::int64_t t_win = 0, t_hp = 0, t_max_hp = 0, t_turn = 0;
        double t_enemy = 0.0, t_escore = 0.0;
    };

    // 一场自对弈 / 教师对局的结果：整条决策序列 + 终局。
    struct GameOut
    {
        std::vector<Frame> frames;
        Term term;
        // 终局判定（`Term::t_win` 是它的 0/1 投影，丢掉"未决"这一档）。评估要区分"打输"与
        // "到步数上限还没分胜负" ⇒ 留原始 `Outcome`。
        Outcome outcome = Outcome::UNDECIDED;
    };

    // ── 两个采集器共用的工具（定义在 `selfplay.cpp`）───────────────────────────────
    //
    // ⚠️ 只**声明**在这儿：改动实现只重编 `selfplay.cpp` 一处（本头的其它消费者不受影响）。
    // 等第三个消费者出现再抽成自己的 `.cpp`。

    // 终局读数（`t_win/t_hp/t_max_hp/t_turn/t_enemy/t_escore`；列名与 `zspec.Terminal` 同序）。
    Term readTerm(const BattleContext &b);

    // 该决策点的观测（`snapshotOf` 的原样输出，归一化在那边做）。
    // ⚠️ 宽度是 `Snap::snapshotLen`（定长数组）⇒ 不传也不存"观测长度"。
    std::array<float, Snap::snapshotLen> snapshotObs(const BattleContext &b);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_COLLECT_H