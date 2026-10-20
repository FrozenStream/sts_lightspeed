#ifndef STS_LIGHTSPEED_TEACHER_H
#define STS_LIGHTSPEED_TEACHER_H

#include <cstdint>
#include <string>
#include <vector>

#include "collect.h"

namespace sts::collect
{

    struct TeacherConfig
    {
        int sims = 3000;      // 每个决策点的 UCT 模拟数
        int max_steps = 1000; // 单场步数上限（与 `collect.MAX_STEPS` 同）
        double tau = 0.0;     // 温度：0 = argmax(π)；>0 = 按 π^(1/τ) 采样
        double eps = 0.0;     // 均匀探索概率（改**执行**的动作，π 不变）
        int threads = 0;      // 并行度（0 = 硬件并发）；不改数值，只改墙钟
    };

    std::vector<std::string> teacherRun(int n_per, const TeacherConfig &cfg, std::uint64_t start_seed, const std::string &out_dir, int shard_rows);

    Outcome teacherPlay(const BattleContext &start, const TeacherConfig &cfg, std::uint64_t seed);
    // 评估用：同时返回终局 `GameOut`（含 term）⇒ 可算战损。
    GameOut teacherPlayGame(const BattleContext &start, const TeacherConfig &cfg, std::uint64_t seed);

    // 从给定局面开始，只打**当前回合**，看是否在回合结束前杀死 `target`。
    bool teacherTurnKills(const BattleContext &start, int target, const TeacherConfig &cfg,
                          std::uint64_t seed, int max_actions = 50);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_TEACHER_H