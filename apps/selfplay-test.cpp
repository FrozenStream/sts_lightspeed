//
// 自对弈热循环的 C++ 测试（`selfplay` 的驱动 + 前向服务）——**不经过 Python**。
//
// 为什么要有它：`_chk_selfplay.py` 是"Python 侧的对拍脚本"，而 `selfplay` 是**纯 C++** 的
// （造局面 / 搜索 / 前向服务 / 落盘）。它的契约（首次前向必须先于起服务线程、K=1 逐位可复现、
// 帧/π/动作的自洽）应该由**同一个语言**来断言 —— 出问题时也不必先起一个 Python 进程。
//
// ⚠️ 需要一份 TorchScript 存档（**由外部给**）：`selfplay-test <存档.ts> [K] [sims] [reps]`。
//    测试不替训练侧造图（网络定义只有 Python 一份）。
//
// 用法：
//   selfplay-test <net.ts>              # 默认 K=1、sims=4、连续 1 次
//   selfplay-test <net.ts> 4 16         # K=4、sims=16
//   selfplay-test <net.ts> 1 4 20       # 「反复调用」模式（一进程内多次调，守卫那种用法）
//   selfplay-test <net.ts> --sweep 96   # **扫描**：`_chk_selfplay` ② 那张表（多遭遇 × 多 seed，
//                                       #  含 act4 / CARD_SELECT 帧）——只跑 CULTIST@4 会漏整类形状。
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "action.h"      // 动作层（`sts::collect`）
#include "encs.h"        // EncJob
#include "scenebuild.h"  // buildBattle / ascensionOf
#include "selfplay.h"
#include "shardio.h"     // kPolicyDim / kSelectDim（π 的宽度按空间分）
#include "snapshot.h"    // snapshotOf / Snap（观测宽度）

#include "constants/MonsterEncounters.h"

#include "test_util.h"

using namespace sts;
using namespace sts::collect;

namespace
{

[[noreturn]] void fail(const std::string &what)
{
    testutil::fatal(what);
}

// 把 `selfplay` 的交付序结果收成一个 vector（测试要逐帧检查 ⇒ 不能只收终局）。
std::vector<GameOut> runAll(const std::vector<BattleContext> &battles,
                            const std::vector<std::uint64_t> &seeds, const SelfplayConfig &cfg)
{
    std::vector<GameOut> outs(battles.size());
    selfplay(battles, seeds, cfg, [&](std::size_t j, GameOut &&g)
             { outs[j] = std::move(g); });
    return outs;
}

// 从一个 `GameOut` 里取该帧的 π（未归一 → 归一）与合法集。
struct FrameView
{
    std::vector<float> pi;    // 归一化后的访问分布（该帧自己的空间宽度）
    std::vector<int> legal;
    bool is_sel = false;
    int a_idx = -1;
};

FrameView view(const Frame &fr)
{
    FrameView v;
    v.is_sel = fr.is_sel;
    v.a_idx = fr.a_idx;
    const int dim = fr.is_sel ? kSelectDim : kPolicyDim;
    v.pi.assign(static_cast<std::size_t>(dim), 0.0f);
    float total = 0.0f;
    for (std::size_t i = 0; i < fr.pi_idx.size(); ++i)
    {
        const int at = fr.pi_idx[i];
        if (at >= 0 && at < dim) { v.pi[static_cast<std::size_t>(at)] = static_cast<float>(fr.pi_n[i]); }
        total += static_cast<float>(fr.pi_n[i]);
    }
    if (total > 0.0f) { for (float &x : v.pi) { x /= total; } }
    v.legal = fr.legal;
    return v;
}

// 一场的"读数"：逐帧的 a_idx / is_sel / 合法集 / 归一后的 π（用于"同参数两次跑逐位相同"）。
std::vector<std::string> digest(const std::vector<GameOut> &outs)
{
    std::vector<std::string> rows;
    char line[512];
    for (std::size_t g = 0; g < outs.size(); ++g)
    {
        for (std::size_t k = 0; k < outs[g].frames.size(); ++k)
        {
            const FrameView v = view(outs[g].frames[k]);
            std::string pi;
            for (std::size_t i = 0; i < v.pi.size(); ++i)
            {
                if (v.pi[i] > 0.0f)
                {
                    std::snprintf(line, sizeof(line), "%zu:%.9g,", i, static_cast<double>(v.pi[i]));
                    pi += line;
                }
            }
            std::string legal;
            for (int i : v.legal) { legal += std::to_string(i) + ","; }
            std::snprintf(line, sizeof(line), "g=%zu k=%zu a=%d sel=%d pi=[%s] legal=[%s]",
                          g, k, v.a_idx, static_cast<int>(v.is_sel), pi.c_str(), legal.c_str());
            rows.emplace_back(line);
        }
    }
    return rows;
}

void checkFrames(const std::vector<GameOut> &outs, const char *where)
{
    int n_frames = 0;
    for (const GameOut &g : outs)
    {
        if (g.frames.empty()) { fail(std::string(where) + "：有一场一帧都没走"); }
        for (const Frame &fr : g.frames)
        {
            FrameView v = view(fr);
            // π 必须归一（"根没有任何被访问过的边"那条已经在 C++ 里抛过了，这里再验一次形状）
            float s = 0.0f;
            for (float x : v.pi) { s += x; }
            if (!(s > 0.999f && s < 1.001f))
                        {
                            fail(std::string(where) + "：某帧的 π 不归一（sum=" + std::to_string(s) + "）");
                        }
            // 执行的动作必须在合法集里
            bool found = false;
            for (int i : v.legal) { if (i == v.a_idx) { found = true; } }
            if (!found)
            {
                fail(std::string(where) + "：a_idx 不在合法集里（帧 " +
                     std::to_string(n_frames) + "，a_idx=" + std::to_string(v.a_idx) + "）");
            }
            ++n_frames;
        }
    }
    std::fprintf(stderr, "  ✓ %s：%zu 场 / %d 帧，π 归一、a_idx ∈ legal\n", where, outs.size(),
                 n_frames);
}

// 扫描：`_chk_selfplay` ② 那张表（每个 `(enc, floor, seed)` 一场，K=1）。
// ⚠️ 覆盖 act4 与 CARD_SELECT 帧 —— 这些形状**只在别的遭遇上出现**（只跑 CULTIST 测不到）。
// `ENC_FLOORS` 是"代表层"里挑出来的几个（含 act1/act2/act3 的普通战与 act4）。
struct SweepCase
{
    MonsterEncounter enc;
    int floor;
};

const SweepCase kSweep[] = {
    {MonsterEncounter::CULTIST, 4},
    {MonsterEncounter::JAW_WORM, 4},
    {MonsterEncounter::TWO_LOUSE, 4},
    {MonsterEncounter::GREMLIN_GANG, 12},
    {MonsterEncounter::LOTS_OF_SLIMES, 12},
    {MonsterEncounter::THREE_SENTRIES, 12},
    {MonsterEncounter::AUTOMATON, 33},
    {MonsterEncounter::THREE_DARKLINGS, 37},
    {MonsterEncounter::MAW, 45},
    {MonsterEncounter::AWAKENED_ONE, 50},
    {MonsterEncounter::SHIELD_AND_SPEAR, 53},
    {MonsterEncounter::THE_HEART, 53},
};

void sweep(const std::string &ts, int sims, int n_seed)
{
    int total_frames = 0;
    int n_case = 0;
    for (const SweepCase &c : kSweep)
    {
        for (int seed = 0; seed < n_seed; ++seed)
        {
            std::vector<BattleContext> battles;
            std::vector<std::uint64_t> seeds;
            seeds.push_back(static_cast<std::uint64_t>(seed));
            battles.push_back(
                buildBattle(seeds.back(), c.enc, c.floor, ascensionOf(seeds.back())));
            SelfplayConfig cfg;
            cfg.ts_path = ts;
            cfg.z_spec = "enginenorm";
            cfg.device = "cpu";
            cfg.sims = sims;
            cfg.max_steps = 40;
            cfg.threads = 1;
            cfg.games = 1;
            cfg.c_puct = 1.5;
            cfg.q_init = 0.0;
            const std::vector<GameOut> outs = runAll(battles, seeds, cfg);
            const std::string where = monsterEncounterEnumNames[static_cast<int>(c.enc)] +
                                      std::string("@") + std::to_string(c.floor) +
                                      " seed=" + std::to_string(seed);
            checkFrames(outs, where.c_str());
            total_frames += static_cast<int>(outs[0].frames.size());
            ++n_case;
        }
    }
    std::fprintf(stderr, "✅ 扫描全过：%d 例 / %d 帧（sims=%d，每例 %d 个 seed）\n", n_case,
                 total_frames, sims, n_seed);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "用法：selfplay-test <net.ts> [K=1] [sims=4] [reps=1] | --sweep [sims]\n");
        return 2;
    }
    const std::string ts = argv[1];
    if (argc > 2 && std::strcmp(argv[2], "--sweep") == 0)
    {
        sweep(ts, argc > 3 ? std::atoi(argv[3]) : 96, argc > 4 ? std::atoi(argv[4]) : 1);
        return 0;
    }
    const std::size_t K = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 1;
    const int sims = argc > 3 ? std::atoi(argv[3]) : 4;
    const int reps = argc > 4 ? std::atoi(argv[4]) : 1;   // 连续调用次数（守卫那种"反复调用"模式）
    if (K < 1) { fail("K 至少 1"); }

    // 造局面（与采集/门禁**同一个入口**）。
    std::vector<BattleContext> battles;
    std::vector<std::uint64_t> seeds;
    for (std::size_t j = 0; j < K; ++j)
    {
        seeds.push_back(static_cast<std::uint64_t>(j));
        battles.push_back(buildBattle(seeds[j], MonsterEncounter::CULTIST, 4, ascensionOf(seeds[j])));
    }

    SelfplayConfig cfg;
    cfg.ts_path = ts;
    cfg.z_spec = "enginenorm";
    cfg.device = "cpu";
    cfg.sims = sims;
    cfg.max_steps = 40;
    cfg.threads = 1;
    cfg.games = static_cast<int>(K);
    cfg.c_puct = 1.5;
    cfg.q_init = 0.0;

    std::fprintf(stderr, "[t] battles done K=%zu\n", K);
    // ⓪ **反复调用**（守卫就是这种用法：同一进程里几十次）——它压的是
    //    "前向服务 / `TorchEvaluator` 的生命周期与清理"这类**只在第 N 次才炸**的问题。
    for (int r = 0; r < reps; ++r)
    {
        const std::vector<GameOut> tmp = runAll(battles, seeds, cfg);
        if (tmp.empty()) { fail("反复调用里有一次一帧都没走"); }
    }
    if (reps > 1) { std::fprintf(stderr, "  ✓ 连续 %d 次调用都正常\n", reps); }

    // ① 跑两次，比读数（K=1 应逐位相同；K>1 只保证"两次跑一致"这一层形态）
    const std::vector<GameOut> a = runAll(battles, seeds, cfg);
    checkFrames(a, "第一次跑");
    const std::vector<GameOut> b = runAll(battles, seeds, cfg);
    checkFrames(b, "第二次跑");
    const std::vector<std::string> da = digest(a), db = digest(b);
    if (da != db)
    {
        const std::size_t n = std::min(da.size(), db.size());
        std::size_t at = n;
        for (std::size_t i = 0; i < n; ++i) { if (da[i] != db[i]) { at = i; break; } }
        std::string msg = "两次跑读数不同";
        if (at < n) { msg += "（第一处不同 @" + std::to_string(at) + "：\n  预期 " + da[at] +
                             "\n  实际 " + db[at] + "）"; }
        else { msg += "（行数 " + std::to_string(da.size()) + " vs " + std::to_string(db.size()) + "）"; }
        fail(msg);
    }
    std::fprintf(stderr, "  ✓ 同参数两次跑：%zu 行读数逐位相同（K=%zu sims=%d）\n", da.size(), K, sims);

    // ② 终局读数与访问总数（K>1 时这就是"可复现"该有的强度）
    for (std::size_t g = 0; g < a.size(); ++g)
    {
        if (a[g].term.t_win != b[g].term.t_win || a[g].term.t_hp != b[g].term.t_hp)
        {
            fail("同参数两次跑的终局读数不同（场 " + std::to_string(g) + "）");
        }
    }
    std::fprintf(stderr, "  ✓ 终局读数两次跑一致（%zu 场）\n", a.size());
    std::fprintf(stderr, "✅ 全过\n");
    return 0;
}