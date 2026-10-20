//
// Created by gamerpuppy on 7/4/2021.
//

#ifndef STS_LIGHTSPEED_MONSTERGROUP_H
#define STS_LIGHTSPEED_MONSTERGROUP_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iostream>

#include "constants/MonsterEncounters.h"
#include "constants/MonsterIds.h"
#include "combat/Monster.h"
#include "data_structure/fixed_list.h"

namespace sts {

    // ── 「没有合法目标」这个窗口：查询侧挡住，动作侧 `.at()`（2026-10-16 采集段错误的修复）──────
    // 觉醒者转阶段 / 暗灵待复活那段：半死怪**不算可选中的**，`monstersAlive` 会掉到 0，而
    // `areMonstersBasicallyDead()` 仍是 false（半死算活着 ⇒ 这场没结束）。此时
    // `getRandomMonsterIdx(aliveOnly=true)` / `getFirstTargetable()` 都回 **-1**（"没有合法目标"，
    // 这是它们的契约），但**引擎里有几处把这个 -1 直接当成怪物下标用了**：
    //   `Havoc` / `DistilledChaos` / 遗物（`Player.cpp`）把 `getRandomMonsterIdx(...)` 的结果当
    //   `PlayTopCard(monsterTargetIdx)` 传下去，以及 `CardQueueItem::randomTarget` 那条路。
    // ⚠️ 为什么这不是"读一下空怪就完了"：`arr` 是 `MonsterGroup` 的**第一个数组**，`arr[-1]` 落在
    //    `BattleContext` 前面那些字段上（不是数组边界外的空洞）⇒ 能读出"看起来活着、能打"的内存，
    //    于是 `AttackEnemy(-1, ...)` 一路走到 `Monster::attacked()`；那条路里的 Hand Drill 分支又会把
    //    `this->idx`（= 从野内存里读出来的 -1509947384）塞进 `DebuffEnemy` ⇒ 真踩内存、采集进程
    //    SIGSEGV（实测：觉醒者半死那场，第 3 轮采集在 1800/2016 场崩）。
    // ⇒ 分两层：
    //   · **查询侧**（`CardInstance::canUse` / `BattleContext::playCardQueueItem`）：越界就是**没有合法
    //     目标**，这张牌**打不出去**（原版也打不出这种牌）—— 这才是"半死窗口"里该有的行为；
    //   · **动作侧**（`Actions::*` 里"按 idx 找某只怪"的那些）：**不做静默空转**，一律 `arr.at(idx)`。
    //     越界 ⇒ `std::out_of_range` 当场抛出（响亮失败，符合本仓"绝不静默跳过"的纪律）；
    //     修好之后它**不该**再被触发，留着只为"下次真出问题别又变成野内存 SIGSEGV"。
    inline void reportInvalidMonsterIdx(const char *site, std::uint64_t seed, int idx, int monsterCount,
                                        int monstersAlive)
    {
        static std::atomic<bool> reported{false};
        bool expected = false;
        if (!reported.compare_exchange_strong(expected, true))
        {
            return; // 每进程只报第一次（这个窗口里每一帧都会命中，全打出来会淹掉日志）
        }
        std::fprintf(stderr,
                     "[警告] 排队卡的目标越界：%s idx=%d（monsterCount=%d monstersAlive=%d seed=%llu）"
                     "⇒ 这张牌打不出去（原版也没有合法目标）\n",
                     site, idx, monsterCount, monstersAlive,
                     static_cast<unsigned long long>(seed));
        std::fflush(stderr);
    }

    struct MonsterGroup {
        int monstersAlive = 0;
        int monsterCount = 0;
        std::array<Monster, 5> arr;

        std::bitset<5> extraRollMoveOnTurn;
        std::bitset<5> skipTurn;

        [[nodiscard]] bool areMonstersBasicallyDead() const;
        [[nodiscard]] int getAliveCount() const;
        [[nodiscard]] int getTargetableCount() const; // calculated here, not fast
        [[nodiscard]] int getFirstTargetable() const;
        [[nodiscard]] int getRandomMonsterIdx(Random &rng, bool aliveOnly=true) const;

        /// `arr[idx]` 只允许 `[0, monsterCount)` —— **用 `monstersAlive` 判是不够的**（见上面 `arr[-1]`
        /// 那段），所以"找目标"的**查询**（`canUse` / `playCardQueueItem`）一律先过这一道；
        /// **动作层**不查、直接用 `arr.at(idx)`（越界当场抛，见下）。
        [[nodiscard]] bool isIdxValid(int idx) const { return idx >= 0 && idx < monsterCount; }

        // initialization

        void init(BattleContext &bc, MonsterEncounter encounter);
        void createMonsters(BattleContext &bc, MonsterEncounter encounter);
        void createMonster(BattleContext &bc, MonsterId id);

        void createStrongHumanoid(BattleContext &bc);
        void createStrongWildlife(BattleContext &bc);
        void createWeakWildlife(BattleContext &bc);
        void createShapes(BattleContext &bc, int count);

        static MonsterId getAncientShape(Random &miscRng);
        static MonsterId getGremlin(Random &rng);
        static MonsterId getLouse(Random &miscRng);
        static MonsterId getSlaver(Random &miscRng);

        // actions
        void doMonsterTurn(BattleContext &bc); // take a turn for the next monster

        void applyPreTurnLogic(BattleContext &bc);
        void applyEmeraldEliteBuff(BattleContext &bc, int buffType, int act);

    };

    std::ostream &operator<<(std::ostream &os, const MonsterGroup &g);

}
#endif //STS_LIGHTSPEED_MONSTERGROUP_H
