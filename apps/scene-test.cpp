//
// `collect/scenebuild.cpp`（层数 → 发育状态）的 **C++ 测试**。
//
// 为什么是 C++ 而不是 `_chk_shape.py`：被测对象与 Python 训练侧**毫无关系**（纯引擎逻辑），
// 写在 Python 里只是多一层 pybind 往返、多一层"忘了重编"的风险。照 KataGo 的分法
// （`cpp/tests/test*.cpp` + `./katago runtests` / `runoutputtests`）：**纯 C++ 逻辑跟代码
// 同语言、同构建**，只有参照物本身是 Python 的那几项（曾对 `_chk_selfplay` / `_chk_teacher` 的
// TorchScript trace）才留在 Python。
//
// 两类断言，分开对待：
//   · **不变量**：自己就能判真假（幕分界 / 同 seed 复现 / 数值域 / 禁用件 / 趋势）⇒ `TEST_ASSERT`。
//     这类**不需要**任何外部参照表 —— 以前 `_chk_shape` 拿 `devshape` 那张已经漂移的表当"表值"
//     对账（容差还被放宽到 ±3 张，比两边的差还宽），是空转。
//   · **钉住的**：`--dump`（默认就打到 stdout）把固定一批 `(seed, 遭遇, 层)` 的 run 级读数打成
//     **纯文本**，与 `collect/tests/golden_scene.txt` 逐行比 ⇒ 红了能看出是**哪一行、哪一项**变了。
//     有意改口径时刷新 golden = 重跑一遍重定向（同 KataGo 的 `| tee tests/results/…txt`）。
//
//   scene-test                                              # 跑不变量
//   scene-test --golden collect/tests/golden_scene.txt      # 再与 golden 逐行对账
//   scene-test --dump > collect/tests/golden_scene.txt      # 刷新 golden（有意改口径时）
//
// ⚠️ 人看的输出一律走 **stderr**，stdout 只留给 `--dump` 的 golden ⇒ `> 文件` 永远是干净的。
//
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "constants/Cards.h"     // getCardStringId
#include "constants/Potions.h"   // getPotionName
#include "constants/Relics.h"    // getRelicName
#include "constants/MonsterEncounters.h"
#include "encs.h"        // representativeFloor（采集口径的「遭遇 → 代表层」）
#include "scenebuild.h"  // buildBattle / buildStaged / actOf / bannedRelics / bannedPotions
#include "snapshot.h"     // snapshotOf（两条出口对拍用）

#include "test_util.h"

using namespace sts;
using namespace sts::collect;

namespace
{

// 一个用例 = `(场景种子, 遭遇, 层)`；进阶由 `ascensionOf(seed)` 抽（与生产同一个抽法）。
struct Case
{
    std::uint64_t seed;
    MonsterEncounter enc;
    int floor;
};

// 固定的一批用例：**每个遭遇各取它的代表层**（`encs.h` 的采集口径）× 3 个 seed，
// 再补一批"段边界"样本 —— 段选错是这张表最容易出的错，而段界正好落在这些层上。
std::vector<Case> cases()
{
    std::vector<Case> v;
    for (MonsterEncounter e : MonsterEncounterPool::allEnemies)
    {
        const int fl = representativeFloor(e);
        for (std::uint64_t s : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{7}})
        {
            v.push_back({s, e, fl});
        }
    }
    // 段界（`STAGE_EDGES = {9,17,26,34,41}`）+ 上下各一格 + act4 那两级。
    for (int fl : {0, 1, 8, 9, 16, 17, 25, 26, 33, 34, 40, 41, 50, 53})
    {
        v.push_back({3, MonsterEncounter::THREE_SENTRIES, fl});
    }
    return v;
}

std::string joinNames(const std::string &sep, const std::vector<std::string> &v)
{
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        if (i > 0)
        {
            out += sep;
        }
        out += v[i];
    }
    return out;
}

// 一局 → 一行可读文本。**牌/遗物/药水都写名字**（写数字的话 diff 出来没法看）：
// 这行的作用就是"人肉 diff 时能立刻读出变了什么"。
std::string dumpLine(const Case &c)
{
    const StagedBattle st = buildStaged(c.seed, c.enc, c.floor, ascensionOf(c.seed));

    std::vector<std::string> deck;
    deck.reserve(st.deck.size());
    for (const std::pair<int, int> &d : st.deck)
    {
        deck.push_back(std::string(getCardStringId(static_cast<CardId>(d.first))) +
                       (d.second ? "+" : ""));
    }
    std::vector<std::string> relics;
    relics.reserve(st.relics.size());
    for (int r : st.relics)
    {
        relics.push_back(getRelicName(static_cast<RelicId>(r)));
    }
    std::vector<std::string> potions;
    potions.reserve(st.potions.size());
    for (int p : st.potions)
    {
        const Potion pot = static_cast<Potion>(p);
        potions.push_back(pot == Potion::EMPTY_POTION_SLOT || pot == Potion::INVALID
                              ? std::string("-")
                              : std::string(getPotionName(pot)));
    }

    std::ostringstream os;
    os << "seed=" << c.seed << " enc=" << monsterEncounterEnumNames[static_cast<int>(c.enc)]
       << " floor=" << c.floor << " act=" << actOf(c.floor) << " asc=" << ascensionOf(c.seed)
       << " hp=" << st.cur_hp << "/" << st.max_hp << " gold=" << st.gold << " deck[" << deck.size()
       << "]=" << joinNames(",", deck) << " relics[" << relics.size() << "]=" << joinNames(",", relics)
       << " potions[" << potions.size() << "]=" << joinNames(",", potions);
    return os.str();
}

std::string dumpAll(const std::vector<Case> &cs)
{
    std::string out = "# scenebuild golden —— 刷新：scene-test --dump > collect/tests/golden_scene.txt\n";
    for (const Case &c : cs)
    {
        out += dumpLine(c);
        out += '\n';
    }
    return out;
}

bool contains(const std::vector<int> &v, int x)
{
    for (int a : v)
    {
        if (a == x)
        {
            return true;
        }
    }
    return false;
}

// ── 不变量 ────────────────────────────────────────────────────────────────────────────

void checkActOf()
{
    // 幕分界固定装置：act1 战斗在 floor 1..16、act2 从 18 起（17 是幕间节点）⇒ 17/34/51。
    // act4（51..53：盾与矛 / 心）在引擎里是**一等公民**：`transitionToAct(4)` 走 `Map::act4Map()` +
    // `boss = THE_HEART` + `eliteMonsterList = SHIELD_AND_SPEAR`（那条路**不**调 `generateMonsters()`
    // ⇒ 不碰 `bosses[act-1]` 那个越界下标）；`returnRandomRelicTier`、`shrineList`、`SimpleAgent`
    // 里都有 `act == 4` 分支。本断言对的就是 python 原版 `ACT_EDGES = (17, 34, 51)`。
    TEST_ASSERT(actOf(0) == 1 && actOf(16) == 1 && actOf(17) == 2);
    TEST_ASSERT(actOf(33) == 2 && actOf(34) == 3 && actOf(50) == 3);
    TEST_ASSERT(actOf(51) == 4 && actOf(53) == 4 && actOf(60) == 4);
    int prev = actOf(0);
    for (int f = 1; f <= 60; ++f)
    {
        const int cur = actOf(f);
        TEST_ASSERT(cur >= prev);
        prev = cur;
    }
    std::fprintf(stderr, "  ✓ actOf：幕分界 17/34/51（act4 从 51 起）、0..60 单调不减\n");
}

void checkReproducible(const std::vector<Case> &cs)
{
    for (const Case &c : cs)
    {
        // 同 (seed, 遭遇, 层) 两次 ⇒ 逐项相同。
        TEST_ASSERT(dumpLine(c) == dumpLine(c));
    }
    // 不同 seed 必须长出不同局面（否则 RNG 没接上）。
    const Case a{0, MonsterEncounter::THREE_SENTRIES, 37};
    const Case b{12345, MonsterEncounter::THREE_SENTRIES, 37};
    TEST_ASSERT(dumpLine(a) != dumpLine(b));
    std::fprintf(stderr, "  ✓ 同 seed 逐项复现（%zu 项）；不同 seed 长出不同局面\n", cs.size());
}

void checkDomain(const std::vector<Case> &cs)
{
    const std::vector<int> bannedRelicIds = bannedRelics();
    const std::vector<int> bannedPotionIds = bannedPotions();
    for (const Case &c : cs)
    {
        const StagedBattle st = buildStaged(c.seed, c.enc, c.floor, ascensionOf(c.seed));

        TEST_ASSERT(!st.deck.empty());
        for (const std::pair<int, int> &d : st.deck)
        {
            // 牌组里不许有 `INVALID`（占位/空槽），也不许有升级位取值越界。
            TEST_ASSERT(d.first > static_cast<int>(CardId::INVALID) && d.first <= static_cast<int>(CardId::ZAP));
            TEST_ASSERT(d.second == 0 || d.second == 1);
        }
        TEST_ASSERT(1 <= st.cur_hp && st.cur_hp <= st.max_hp);
        TEST_ASSERT(st.gold >= 0);
        // 遗物：不许重复、不许含禁用件（禁用表两半在两条发放路上都点不到，见 scenebuild.cpp）。
        for (std::size_t i = 0; i < st.relics.size(); ++i)
        {
            TEST_ASSERT(!contains(bannedRelicIds, st.relics[i]));
            for (std::size_t j = i + 1; j < st.relics.size(); ++j)
            {
                TEST_ASSERT(st.relics[i] != st.relics[j]);
            }
        }
        // 药水：≤ 3 瓶（`potionCapacity` 上界）、不放回（抽中哪瓶就移哪瓶 ⇒ 不许重复）、
        // 不许含禁用件。
        std::vector<int> got;
        for (int p : st.potions)
        {
            const Potion pot = static_cast<Potion>(p);
            if (pot == Potion::EMPTY_POTION_SLOT || pot == Potion::INVALID)
            {
                continue;
            }
            TEST_ASSERT(!contains(bannedPotionIds, p));
            TEST_ASSERT(!contains(got, p));
            got.push_back(p);
        }
        TEST_ASSERT(got.size() <= 3);
    }
    std::fprintf(stderr,
                 "  ✓ 取值域：牌组非空无 INVALID / 1≤hp≤max / gold≥0 / 遗物·药水不重复且不含禁用件\n");
}

// 两条出口必须来自同一个构造：`buildBattle` 的快照 == `buildStaged(...).b` 的快照。
void checkBothExitsAgree()
{
    int n = 0;
    for (const Case &c : cases())
    {
        if (n++ % 17 != 0) // 抽一批就够，全跑没额外信息
        {
            continue;
        }
        const BattleContext a = buildBattle(c.seed, c.enc, c.floor, ascensionOf(c.seed));
        const StagedBattle st = buildStaged(c.seed, c.enc, c.floor, ascensionOf(c.seed));
        const auto sa = snapshotOf(a), sb = snapshotOf(st.b);
        TEST_ASSERT(sa.size() == sb.size());
        for (std::size_t i = 0; i < sa.size(); ++i)
        {
            TEST_ASSERT(sa[i] == sb[i]);
        }
    }
    std::fprintf(stderr, "  ✓ buildBattle 与 buildStaged(...).b 的快照逐位相同\n");
}

// 层数↑ ⇒ 牌组张数 / 遗物件数 单调↑（对**多个 seed 求均值**：单 seed 噪声太大，
// 这不是"阈值放宽"，是把样本量加够）。
void checkTrend()
{
    const int floors[4] = {4, 20, 37, 50};
    double deckMean[4] = {0, 0, 0, 0};
    double relicMean[4] = {0, 0, 0, 0};
    const int nSeeds = 32;
    for (int i = 0; i < 4; ++i)
    {
        for (int s = 0; s < nSeeds; ++s)
        {
            const StagedBattle st =
                buildStaged(static_cast<std::uint64_t>(s), MonsterEncounter::THREE_SENTRIES, floors[i],
                            ascensionOf(static_cast<std::uint64_t>(s)));
            deckMean[i] += static_cast<double>(st.deck.size());
            relicMean[i] += static_cast<double>(st.relics.size());
        }
        deckMean[i] /= nSeeds;
        relicMean[i] /= nSeeds;
    }
    for (int i = 1; i < 4; ++i)
    {
        TEST_ASSERT(deckMean[i] > deckMean[i - 1]);
        TEST_ASSERT(relicMean[i] > relicMean[i - 1]);
    }
    std::fprintf(stderr,
                 "  ✓ 层数↑ ⇒ 平均牌组 %.1f→%.1f→%.1f→%.1f 张、遗物 %.1f→%.1f→%.1f→%.1f 件"
                 "（各 %d seed 均值）\n",
                 deckMean[0], deckMean[1], deckMean[2], deckMean[3], relicMean[0], relicMean[1],
                 relicMean[2], relicMean[3], nSeeds);
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        testutil::fatal("读不到 golden 文件: " + path);
    }
    std::ostringstream os;
    os << in.rdbuf();
    // golden 可能被 git 以 CRLF 检出（`--dump` 在 Windows 上写的也是 CRLF）⇒ 比较前统一换行。
    std::string s = os.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

} // namespace

int main(int argc, char **argv)
{
    const std::vector<Case> cs = cases();

    bool dumpOnly = false;
    std::string goldenPath;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--dump") == 0)
        {
            dumpOnly = true;
        }
        else if (std::strcmp(argv[i], "--golden") == 0 && i + 1 < argc)
        {
            goldenPath = argv[++i];
        }
        else
        {
            std::fprintf(stderr, "用法: scene-test [--dump] [--golden <path>]\n");
            return 2;
        }
    }

    const std::string dump = dumpAll(cs);

    if (dumpOnly)
    {
        std::fwrite(dump.data(), 1, dump.size(), stdout);
        return 0;
    }

    std::fprintf(stderr, "scenebuild 测试（%zu 项用例）\n", cs.size());
    checkActOf();
    checkReproducible(cs);
    checkDomain(cs);
    checkBothExitsAgree();
    checkTrend();

    if (!goldenPath.empty())
    {
        testutil::expectText("scenebuild golden", dump, readFile(goldenPath));
        std::fprintf(stderr, "  ✓ 与 %s 逐行相同\n", goldenPath.c_str());
    }

    std::fprintf(stderr, "\n✅ 全过\n");
    return 0;
}
