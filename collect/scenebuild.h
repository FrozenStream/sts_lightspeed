//
// 按层造发育状态 —— **唯一一份实现**（Python `scene.py` 只剩转发）。表在 `devshape`，
// ⚠️ 但本文件的常量**已经不是它的副本**：两边的数已经各改各的（见下）。
//
// 抽样一律泊松（段内各档 / 删牌 / 升级的张数），每个 RNG 流用 `seed * 素数 + 素数` 派生。
// ⚠️ 泊松走 `std::poisson_distribution`（2026-10-11 从手写 Knuth 换过来，本机逐位不变）——
//    它**不是标准规定的算法**，两家实现在 mean ≈ 12 处换算法 ⇒ 抽取次数会变 ⇒ 整条流后面
//    全变。本表各均值 ≤ 9.91 在安全区，但**这是隐式不变量**（见 `scenebuild.cpp` 的注记）。
//
// ⚠️ 实测（2026-10-09）：`shapeFloor` **不消耗战斗 RNG**。它是 `gc -> BattleContext::init`
//    之前对 `GameContext` 的纯写入，而 `init` 从 `gc` 里派生战斗 RNG 状态 ⇒ 不 reshape 与
//    reshape 出来的战斗**起手快照逐位不同**（牌组/hp 不同），但**抽牌序只由 seed 决定**。
//    ⇒ 安全网是"同一 seed 下形状可复现"。
//
// 为什么搬到 C++：自对弈现在整段在 C++（`selfplay.{h,cpp}`）⇒ 若造局面还在 Python，每采一批
// 就要把所有战斗对象送回 Python（pybind 拷贝 + GIL）——那是整条路上唯一的 Python 热点。
//
#ifndef STS_LIGHTSPEED_SCENEBUILD_H
#define STS_LIGHTSPEED_SCENEBUILD_H

#include <utility>
#include <vector>

#include "combat/BattleContext.h"
#include "game/GameContext.h"

namespace sts::collect
{

// `(seed, 遭遇)` → 一局战斗。`floor` 决定发哪一段的牌组/升级/遗物/药水/hp/金币，
// `ascension` 只在 A10+ 触发 `ASCENDERS_BANE`（调用方按 `seed` 抽，见 `scene.ascension_of`）。
BattleContext buildBattle(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension);

// 与 `buildBattle` 同一套 rng，但把整组**局内**读数一起交出来（诊断 / `_chk_scene` 对拍用）。
struct StagedBattle
{
    BattleContext b;
    int cur_hp = 0, max_hp = 0, gold = 0;
    std::vector<std::pair<int, int>> deck;        // [(CardId, upgraded?)]
    std::vector<int> relics;                      // RelicId
    std::vector<int> potions;                     // Potion
};

StagedBattle buildStaged(std::uint64_t seed, MonsterEncounter enc, int floor, int ascension);

// `scene.ascension_of(seed)`：按 seed 抽本场的进阶等级（唯一决定 A10+ 的 `ASCENDERS_BANE`）。
int ascensionOf(std::uint64_t seed);

// 层号 → 幕（`sts.act_of`）：17 / 34 / 51 是幕分界。**唯一一处**，`buildStaged` 与
// `apps/scene-test.cpp` 都读它（以前 Python 与 C++ 各写一遍同样的三元式，C++ 那份还漏了 51）。
int actOf(int floor);

// 场景**不发**的遗物 / 药水（`BANNED_RELICS` / `BANNED_POTIONS`）—— `_chk_shape.py` 要拿它
// 扫"发放池里有没有引擎表达不了的东西"，所以得从唯一的那份常量里出来。
std::vector<int> bannedRelics();
std::vector<int> bannedPotions();

} // namespace sts::collect

#endif //STS_LIGHTSPEED_SCENEBUILD_H