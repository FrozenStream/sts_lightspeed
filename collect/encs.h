//
// 采集任务流：**遭遇名单直接读引擎的总表**，采集侧只出"代表层"这一个决定。
//
// ⚠️ 这里**不抄**任何遭遇名单：
//     · 任务流逐项遍历 `MonsterEncounterPool::allEnemies`（引擎的遭遇总表）
//     · 代表层查 `MonsterEncounterPool` 的弱怪 / 强怪 / 精英池 + `sts::bosses`
//       （act1..3 的三选一 Boss，由 `GameContext.cpp` 的 `generateBoss()` 提出来共享）
//       + `sts::kEventEncounters`（`MonsterEncounter` 里 Events 那一段）
//   ⇒ 引擎加一个遭遇（或调权重）时这里**自动跟上**，不会出现"两份名单悄悄分叉"。
//
// ⚠️ 代表层是**采集口径**（不是引擎给的）：同一个遭遇在不同层拿到不同发育状态，取哪个层由
//    `representativeFloor` 决定。改它 = 改采集分布 ⇒ 换缓存名（`collect_encs.ENCS_FP` 那套）。
//
#ifndef STS_LIGHTSPEED_ENCS_H
#define STS_LIGHTSPEED_ENCS_H

#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "constants/MonsterEncounters.h"

namespace sts::collect
{

    // act 1..3 的 Boss 表**不在这里** —— 它在 `constants::bosses`（引擎与采集共用一份）。
    // act4（"幕末之后"）的两个引擎没有池（`generateBoss` 的 `bosses[act-1]` 会越界），
    // 所以按枚举名列出。
    static constexpr MonsterEncounter Act4Enc[2] = {MonsterEncounter::SHIELD_AND_SPEAR, MonsterEncounter::THE_HEART};

    // 事件遭遇（= `MonsterEncounter` 里 `// Events` 那一段）。
    static constexpr MonsterEncounter eventEnc[] = {
        MonsterEncounter::LAGAVULIN_EVENT,
        MonsterEncounter::COLOSSEUM_EVENT_SLAVERS,
        MonsterEncounter::COLOSSEUM_EVENT_NOBS,
        MonsterEncounter::MASKED_BANDITS_EVENT,
        MonsterEncounter::MUSHROOMS_EVENT,
        MonsterEncounter::MYSTERIOUS_SPHERE_EVENT,
    };

    // ⚠️ `EncJob` / `encJobs` 原先住在嵌在 `encs.h` 里的 `namespace py`（= `sts::py`），2026-10-11
        //    起直接在 `sts::collect` 里（"目录名 = 命名空间名"）⇒ 那层 `namespace py { }` 已删。
            // 一个采集项 = `(seed, 遭遇, 代表层)`。**全仓只有这一个类型**：它既是调度单元
        // （`encJobs` 铺任务 / `runTeacherAll` 领任务），也是落盘时要写的三个场级标量
        // （`shardAddRow` 直接收它）。⇒ 不另立"落盘专用结构"：字段是同一组，拆开只会多一次转换。
        // ⚠️ `enc` 用**枚举**（类型安全）；降级成落盘列的 `i32` 只发生在 `shardAddRow` 内部。
        struct EncJob
        {
            std::uint64_t seed = 0;
            MonsterEncounter enc = MonsterEncounter::INVALID;
            int floor = 0;
        };

        inline int representativeFloor(MonsterEncounter enc)
        {
            for (int act = 0; act < 3; ++act)
            {
                for (int k = 0; k < MonsterEncounterPool::weakCount[act]; ++k)
                    if (MonsterEncounterPool::weakEnemies[act][k] == enc)
                        return act == 0 ? 4 : (act == 1 ? 20 : 37);

                for (int k = 0; k < MonsterEncounterPool::strongCount[act]; ++k)
                    if (MonsterEncounterPool::strongEnemies[act][k] == enc)
                        return act == 0 ? 12 : (act == 1 ? 30 : 45);

                for (int k = 0; k < 3; ++k)
                    if (MonsterEncounterPool::elites[act][k] == enc)
                        return act == 0 ? 12 : (act == 1 ? 30 : 45);

                for (int k = 0; k < 3; ++k)
                    if (sts::bosses[act][k] == enc)
                        return act == 0 ? 16 : (act == 1 ? 33 : 50);
            }

            for (MonsterEncounter e : Act4Enc)
                if (e == enc)
                    return 53;

            for (MonsterEncounter e : eventEnc)
            {
                if (e == enc)
                {
                    switch (enc)
                    {
                    case MonsterEncounter::LAGAVULIN_EVENT:
                    case MonsterEncounter::MUSHROOMS_EVENT:
                        return 12;
                    default:
                        return 30;
                    }
                }
            }
            throw std::runtime_error("representativeFloor：unKnown encounter: {}" + std::to_string(static_cast<int>(enc)));
        }

        inline std::vector<EncJob> encJobs(int n_per, std::uint64_t start_seed = 0)
        {
            const auto &t = MonsterEncounterPool::allEnemies;
            std::vector<EncJob> jobs;
            jobs.reserve(std::size(t) * static_cast<std::size_t>(n_per));
            for (MonsterEncounter e : t)
            {
                const int fl = representativeFloor(e);
                for (std::uint64_t i = 0; i < n_per; ++i)
                    jobs.push_back(EncJob{start_seed + i, e, fl});
            }
            return jobs;
        }

    } // namespace sts::collect

#endif // STS_LIGHTSPEED_ENCS_H