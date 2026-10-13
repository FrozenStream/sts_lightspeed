#ifndef STS_LIGHTSPEED_TEACHER_H
#define STS_LIGHTSPEED_TEACHER_H

#include <cstdint>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>

#include "collect.h" // Frame / Term / GameOut / readTerm / snapshotObs（采集侧通用）

namespace sts::collect
{

struct TeacherConfig
{
    int sims = 3000;                 // 每个决策点的 UCT 模拟数
    int max_steps = 1000;            // 单场步数上限（与 `collect.MAX_STEPS` 同）
    double tau = 0.0;                // 温度：0 = argmax(π)；>0 = 按 π^(1/τ) 采样
    double eps = 0.0;                // 均匀探索概率（改**执行**的动作，π 不变）
    double exploration_parameter = 0.0;   // 引擎 UCB 常数；0 = 引擎默认（3√2）
    int threads = 0;                 // 并行度（0 = 硬件并发）；不改数值，只改墙钟
};

// ⚠️ 本头**只有一个**导出面：`teacherRun` + `registerTeacher`。教师侧的其余东西
//    （`runTeacherGame` / `runTeacherAll` / `pickTeacherAction`）都在
//    `teacher.cpp` 的匿名 namespace 里 —— 曾经 `pickTeacherAction` 挂在这里的导出面是为了让
//    守卫单测它，而守卫早改成"整条链对着教师分片对拍" ⇒ 那条导出面成了死面（且当时的注释
//    **说它已收回匿名 namespace，代码却不在**，名实不符就是这么来的）。
// ⚠️ 终局读数（`Term`）与观测（`snapshotObs`）的**唯一实现**在 `selfplay.cpp`（采集侧通用，
//    声明在 `collect.h`）。本文件曾有一个 `teacherTerm()` 的壳，零引用 ⇒ 2026-10-11 删掉。

// ── 整段采集入口（**任务表 + 造局面 + 搜索 + 落盘全在 C++**）──────────────────────
//
// 这是**唯一的生产面**：自己按 `encs.h` 的引擎派生表铺开任务、自己 `buildBattle` 造局面、
// 自己算观测（`snapshot.h`）并落盘。所以不需要从 Python 收 battles / 注入观测写入器。
//
// `out_dir` 下会产出若干 `.bin` 分片（`blk<i>_<p>` 命名），返回它们的路径列表。
// ⚠️ 落盘**单线程按场序**：同一场的行必须连续（读侧靠"终局列相同的连续行"认场 ——
//    落盘里没有任何场身份列，训练也不需要它们）。
std::vector<std::string> teacherRun(int n_per, const TeacherConfig &cfg, std::uint64_t start_seed,
                                    const std::string &out_dir, int shard_rows);

/// 用教师策略把**给定局面**打完（评估要"同一局面、不同策略"配对比较），只回终局判定。
/// ⚠️ 与 `teacherRun` 逐决策点是**同一份实现**（同一个 UCT + 同一个取舍规则）。
Outcome teacherPlay(const BattleContext &start, const TeacherConfig &cfg, std::uint64_t seed);

void registerTeacher(pybind11::module_ &m);

} // namespace sts::collect

#endif //STS_LIGHTSPEED_TEACHER_H