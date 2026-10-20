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
//   · `net:<path>[:<sims>]` —— 网络策略；**不给 `:sims`（= 0）= 裸网络**：逐决策点直接取网络
//                             先验的 argmax（没有任何搜索遮挡）；给 `:sims` 才走它自己的 MCTS
//                             （`selfplay` 的 K 棵树，那时 `sims` 必须 ≥ 2）。
//
// ⚠️ 配对的前提是**同一段 seed**：所有策略跑同一批 `(item, seed)`，所以两个策略的 `games` 相同
//    时胜率可直接相减。`games` 不同（某项中途出错）时只比各自胜率，不再声称配对。
//
#ifndef STS_LIGHTSPEED_COLLECT_EVAL_H
#define STS_LIGHTSPEED_COLLECT_EVAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "selfplay.h" // SelfplayConfig / selfplay（net 策略走它）

namespace sts::collect
{

    // 一个策略。`kind` = "random" / "simple" / "teacher" / "net"；`label` 是报给调用方的名字（唯一）。
    struct EvalSpec
    {
        std::string kind;
        std::string label;
        std::string model_path; // "net" 用：网络存档（ONNX Runtime 认 `.onnx`，见 `net_eval.h`）
        std::string device;     // "net" 用（空 = CPU）
        std::string z_spec;     // "net" 用（终局价值口径，只为和采集同源）
        int sims = 0;           // "teacher" / "net" 的搜索预算
        int workers = 0;        // "net" 用：MCTS worker 线程数（0 = 硬件并发；评估入口应传 cgroup 配额）
        std::string model_path_cpu; // "net" 用：尾段走的 CPU 存档（空 = 用主存档 + CPU）
    };

    struct EvalSpecStats
    {
        std::int64_t games = 0;      // 打完的场数
        std::int64_t wins = 0;       // `Outcome::PLAYER_VICTORY`
        std::int64_t undecided = 0;  // 到 `max_steps` 还没分出胜负
        // 终局血量/回合（`Term`）。teacher 是 const-start 接口，暂时拿不到终局血量。
        std::int64_t hp_sum = 0, max_hp_sum = 0;          // 全部场（终局）
        std::int64_t pre_hp_sum = 0, pre_max_hp_sum = 0;  // 全部场（战前）
        std::int64_t win_hp_sum = 0, win_max_hp_sum = 0;  // 仅胜场（终局）
        std::int64_t win_pre_hp_sum = 0, win_pre_max_hp_sum = 0;
        std::int64_t turn_sum = 0;

        // 逐遭遇读数（按 `MonsterEncounter` 下标；`games=0` 表示该 spec 没打这个遭遇）。
        struct Enc
        {
            std::string name;
            bool hard = false;
            std::int64_t games = 0, wins = 0, undecided = 0;
            std::int64_t hp_sum = 0, max_hp_sum = 0;
            std::int64_t pre_hp_sum = 0, pre_max_hp_sum = 0;
            std::int64_t win_hp_sum = 0, win_max_hp_sum = 0;
            std::int64_t win_pre_hp_sum = 0, win_pre_max_hp_sum = 0;
            std::int64_t turn_sum = 0;
        };
        std::vector<Enc> per_enc;
    };

    // 每个策略打完整张遭遇表 × `n_per` 场（表来自 `encs.h`，与采集同源）。
    // `games` = 一次并肩几棵树（只影响墙钟与批宽）；`max_steps` = 单场步数上限。
    // 返回与 `specs` 同序的统计；`specs[i].label` 是它的名字。
    std::vector<EvalSpecStats> evalRun(const std::vector<EvalSpec> &specs, int n_per,
                                       std::uint64_t start_seed, int games, int max_steps,
                                       bool hard_only = false);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_COLLECT_EVAL_H
