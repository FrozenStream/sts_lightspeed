//
// 评估触发器（`sts.eval_run`）：**同一批局面、多个策略**各打一遍，报胜率。
//
// 为什么要它：门控（"候选 vs 当前 best 谁更强"）必须**配对** —— 同一 `(遭遇, 层, seed)` 造出的
// 局面喂给所有策略，胜率差才是策略的差别而不是局面的差别。旧实现是 Python 的 `eval_encs.py`
// （每项一个子进程 + 自己造局面 + 自己统计），2026-10-14 随 Python 侧评估一起删；这里与
// `teacher_run` / `selfplay_run` 同形：**任务表 + 造局面 + 搜索 + 统计全在 C++**，Python 只当触发器。
//
// 策略：
//   · `random`            —— 合法动作里均匀取（基线，也是"这局面有多好打"的尺子）
//   · `teacher:<sims>`    —— 引擎 UCT（`BattleScumSearcher2`），与教师采集同一份实现
//   · `net:<path>:<sims>` —— 网络自己的 MCTS（`selfplay` 的 K=1 树，逐决策点一步）
//
// ⚠️ 配对的前提是**同一段 seed**：所有策略跑同一批 `(item, seed)`，所以两个策略的 `games` 相同
//    时胜率可直接相减。`games` 不同（某项中途出错）时只比各自胜率，不再声称配对。
//
#ifndef STS_LIGHTSPEED_COLLECT_EVAL_H
#define STS_LIGHTSPEED_COLLECT_EVAL_H

#include <cstdint>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>

#include "selfplay.h" // SelfplayConfig / selfplay（net 策略走它）

namespace sts::collect
{

    // 一个策略。`kind` = "random" / "teacher" / "net"；`label` 是报给调用方的名字（唯一）。
    struct EvalSpec
    {
        std::string kind;
        std::string label;
        std::string ts_path; // "net" 用（TorchScript 存档）
        std::string device;  // "net" 用（空 = CPU）
        std::string z_spec;  // "net" 用（终局价值口径，只为和采集同源）
        int sims = 0;        // "teacher" / "net" 的搜索预算
    };

    struct EvalSpecStats
    {
        std::int64_t games = 0;      // 打完的场数
        std::int64_t wins = 0;       // `Outcome::PLAYER_VICTORY`
        std::int64_t undecided = 0;  // 到 `max_steps` 还没分出胜负
    };

    // 每个策略打完整张遭遇表 × `n_per` 场（表来自 `encs.h`，与采集同源）。
    // `games` = 一次并肩几棵树（只影响墙钟与批宽）；`max_steps` = 单场步数上限。
    // 返回与 `specs` 同序的统计；`specs[i].label` 是它的名字。
    std::vector<EvalSpecStats> evalRun(const std::vector<EvalSpec> &specs, int n_per,
                                       std::uint64_t start_seed, int games, int max_steps);

    void registerEval(pybind11::module_ &m);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_COLLECT_EVAL_H
