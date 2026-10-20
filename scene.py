"""按层造发育状态 —— **Python 侧只做转发**，唯一的实现是 C++ 的 `collect/scenebuild.cpp`。

这份文件曾经是整套形状逻辑的**第二实现**（牌池、发牌/升级/删牌、遗物、药水、hp/金币各一份），
与 C++ 版早就漂移：同进阶下同一个 `(seed, 层)` 的快照 **20/20 组都不同**。而生产（采集 /
自对弈 / 评测）走的一直是 C++ ⇒ 那条 Python 路只剩"守卫在评另一套场"的作用 ⇒ 整段删掉。
要**战斗**用 `stage_battle`（`BattleContext`，也就是生产走的那条路）。造局态的 run 级读数
（牌组/遗物/药水/hp/金币）**只有 C++ 测试出口**：`collect/scenebuild.h` 的 `buildStaged`
（`apps/scene-test.cpp` 的 golden 用它）。Python 不再导出 `build_staged` —— 它没有调用方，
而且 `BattleContext` 在开战时已经改过 hp / 遗物位图，反推不出 run 级那份。
**进阶**按 seed 抽（`ascension_of`，唯一决定 A10+ 的 `ASCENDERS_BANE`）。
"""
import slaythespire as sts


def ascension_of(seed: int) -> int:
    """按 seed 抽本场的进阶等级 —— 转发 `sts.ascension_of`。"""
    return sts.ascension_of(int(seed))


def act_of(floor: int) -> int:
    """层号 → 幕（17/34/51 是幕分界）—— 转发 `sts.act_of`。"""
    return sts.act_of(int(floor))


def stage_battle(seed: int, enc: sts.MonsterEncounter, floor: int) -> sts.BattleContext:
    """按层造一局战斗（采集 / 自对弈 / 评测 / 守卫的**同一个**入口）。

    实现就是 `sts.build_battle(seed, enc, floor, ascension_of(seed))`：层号由调用方给，
    进阶按 seed 抽。
    """
    return sts.build_battle(int(seed), enc, int(floor), ascension_of(seed))
