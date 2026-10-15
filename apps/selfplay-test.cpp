//
// 自对弈热循环的 C++ 测试（`selfplay` 的驱动 + 前向服务）——**不经过 Python**。
//
// 为什么要有它：`_chk_selfplay.py` 是"Python 侧的对拍脚本"，而 `selfplay` 是**纯 C++** 的
// （造局面 / 搜索 / 前向服务 / 落盘）。它的契约（首次前向必须先于起服务线程、K=1 逐位可复现、
// 帧/π/动作的自洽）应该由**同一个语言**来断言 —— 出问题时也不必先起一个 Python 进程。
//
// ⚠️ 需要一份**网络存档**（**由外部给**）：`selfplay-test <存档.onnx> [K] [sims] [reps]`
//    （C++ 侧只认 ONNX Runtime，见 `bindings/net_eval.h`）。`onnxnet.py` 负责导出。
//    测试不替训练侧造图（网络定义只有 Python 一份）。
//
// 用法：
//   selfplay-test <net.onnx>              # 默认 K=1、sims=4、连续 1 次
//   selfplay-test <net.onnx> 4 16         # K=4、sims=16
//   selfplay-test <net.onnx> 1 4 20       # 「反复调用」模式（一进程内多次调，守卫那种用法）
//   selfplay-test <net.onnx> 8 4 1 --pool 1   # **池化（补场）路径**：池宽 1 ⇒ 结果必须逐位等于逐场单跑
//   selfplay-test <net.onnx> 1 4 1 --cpu-rows 10  # **尾段分流**（窄批走 CPU 会话）：K=1 时与不分流
//                                             #   逐位相同（同存档同 EP ⇒ 只换哪条线程算）
//   selfplay-test <net.onnx> --sweep 96   # **扫描**：`_chk_selfplay` ② 那张表（多遭遇 × 多 seed，
//                                       #  含 act4 / CARD_SELECT 帧）——只跑 CULTIST@4 会漏整类形状。
//
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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

// 池化路径（**池宽 < 任务数** ⇒ 走"跑完一场立刻补一场"那条路）。这条路上的下标逻辑与
// `start_task`/`emitted` 计数是独立的，`runAll`（池宽 = 任务数）根本碰不到它 ⇒ 单独守。
std::vector<GameOut> runAllPooled(const std::vector<BattleContext> &battles,
                                  const std::vector<std::uint64_t> &seeds, const SelfplayConfig &cfg,
                                  std::size_t pool)
{
    std::vector<GameOut> outs(battles.size());
    std::vector<TaskIn> tasks;
    tasks.reserve(battles.size());
    for (std::size_t j = 0; j < battles.size(); ++j)
    {
        tasks.push_back(TaskIn{battles[j], seeds[j]});
    }
    selfplayPooled(
        tasks.size(), pool, [&](std::size_t j) { return tasks[j]; }, cfg,
        [&](std::size_t j, GameOut &&g) { outs[j] = std::move(g); });
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

// **池化（补场）路径**的守卫：池宽 < 任务数时走"跑完一场立刻补一场"，那条路上的环槽下标 /
// `started` · `emitted` 计数是独立逻辑，`runAll`（池宽 = 任务数）根本碰不到它。
// ⚠️ `pool == 1` 时只有一棵树在飞 ⇒ 批恒 1 行 ⇒ 结果必须**逐位等于**"一场一场单跑"（这就是这条
//    路上最强的断言：补场计数错了、槽位串了就立刻对不上）。池宽 > 1 时批组成由时序决定（本来
//    就与批宽相关）⇒ 只断言形状与"任务序交付"。
void checkPooled(const std::vector<BattleContext> &battles, const std::vector<std::uint64_t> &seeds,
                 const SelfplayConfig &cfg, std::size_t pool)
{
    const std::size_t K = battles.size();
    std::vector<TaskIn> tasks;
    tasks.reserve(K);
    for (std::size_t j = 0; j < K; ++j)
    {
        tasks.push_back(TaskIn{battles[j], seeds[j]});
    }
    const auto run = [&]
    {
        std::vector<GameOut> outs(K);
        selfplayPooled(
            K, pool, [&](std::size_t j) { return tasks[j]; }, cfg,
            [&](std::size_t j, GameOut &&g)
            {
                if (j >= K) { fail("交付下标超出任务数"); }
                if (!outs[j].frames.empty()) { fail("同一个交付下标被交付了两次"); }
                outs[j] = std::move(g);
            });
        return outs;
    };

    std::vector<GameOut> games = run();
    if (games.size() != K) { fail("交付的场数与任务数不符"); }
    checkFrames(games, pool == 1 ? "池化 pool=1" : "池化（第一跑）");

    if (pool == 1)
    {
        // 逐场比：`digest` 会把"在这一批里的第几场"写进读数，所以两边都按**单场**的姿势取读数。
        std::size_t n_rows = 0;
        for (std::size_t j = 0; j < K; ++j)
        {
            const std::vector<GameOut> ref = runAll({battles[j]}, {seeds[j]}, cfg); // 单跑（批恒 1 行）
            const std::vector<GameOut> mine{games[j]};                              // 滑窗里的第 j 场
            const std::vector<std::string> dref = digest(ref);
            const std::vector<std::string> dmine = digest(mine);
            if (dref != dmine)
            {
                fail("池宽 1 的第 " + std::to_string(j) + " 场与单跑不一致（" +
                     std::to_string(dref.size()) + " vs " + std::to_string(dmine.size()) + " 帧）");
            }
            n_rows += dmine.size();
        }
        std::fprintf(stderr, "  ✓ 池宽 1（一次只有一棵树在飞）：%zu 帧与逐场单跑逐位相同\n", n_rows);
        return;
    }
    const std::vector<GameOut> b = run();
    checkFrames(b, "池化（第二跑）");
    std::fprintf(stderr, "  ✓ 池宽 %zu：两跑形态都自洽（池宽 < 任务数 ⇒ 批组成由时序决定，"
                         "不逐位比）\n",
                 pool);
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

void sweep(const std::string &ts, int sims, int n_seed, int workers)
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
            cfg.model_path = ts;
            cfg.z_spec = "enginenorm";
            cfg.device = "cpu";
            cfg.sims = sims;
            cfg.max_steps = 40;
            cfg.threads = 1;
            cfg.pool = 1; // 单场：池宽恒 1
            cfg.workers = workers;
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
        std::fprintf(stderr, "用法：selfplay-test <net.onnx> [K=1] [sims=4] [reps=1] [--workers N]\n"
                             "      selfplay-test <net.onnx> [K=8] [sims=4] [--pool P]   # 池化（补场）路径\n"
                             "      selfplay-test <net.onnx> [K=1] [sims=4] [--cpu-rows N] # 尾段分流（③）\n"
                             "      selfplay-test <net.onnx> --sweep [sims] [n_seed] [--workers N]\n"
                             "      --workers（worker 线程数，0 = 硬件并发数）/ --pool（池宽，见 `selfplayPooled`）\n"
                             "      --cpu-rows（待服行数 ≤ N 就走 CPU 会话；⚠️ 用**同一份存档**、K=1 时断言\n"
                             "                 分流与不分流逐位相同 —— 本地/CUDA 守卫都靠这条压分流管线）\n"
                             "      必须排**最后**，且一次只能给一个。\n");
        return 2;
    }
    const std::string ts = argv[1];
    int workers = 0; // 0 = 硬件并发数（库默认）
    std::size_t pool = 0; // 0 = 不测池化路径（`> 0` ⇒ 跑 `selfplayPooled`，池宽 = 这个值）
    int cpu_rows = 0;   // > 0 ⇒ **开尾段分流**（窄批走 CPU 会话；`--cpu-rows N`）
    int cpu_sessions = 2;
    for (int i = 2; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--workers") == 0)
        {
            workers = std::atoi(argv[i + 1]);
            argc = i; // 旗标及其值不进位置参数
            break;
        }
        if (std::strcmp(argv[i], "--pool") == 0)
        {
            pool = static_cast<std::size_t>(std::max(1, std::atoi(argv[i + 1])));
            argc = i;
            break;
        }
        if (std::strcmp(argv[i], "--cpu-rows") == 0)
        {
            cpu_rows = std::max(1, std::atoi(argv[i + 1]));
            argc = i;
            break;
        }
    }
    if (argc > 2 && std::strcmp(argv[2], "--sweep") == 0)
    {
        sweep(ts, argc > 3 ? std::atoi(argv[3]) : 96, argc > 4 ? std::atoi(argv[4]) : 1, workers);
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
    cfg.model_path = ts;
    cfg.z_spec = "enginenorm";
    cfg.device = "cpu";
    cfg.sims = sims;
    cfg.max_steps = 40;
    cfg.threads = 1;
    cfg.pool = static_cast<int>(K); // 池宽 = 任务数 ⇒ 整表一次全开（这条路的语义）
    cfg.workers = workers;
    cfg.c_puct = 1.5;
    cfg.q_init = 0.0;
    // **尾段分流**（`--cpu-rows N`）：待服行数 ≤ N 时交给 CPU 会话（见 `collect/selfplay.h`）。
    // ⚠️ 这里主会话与 CPU 会话**同一份存档、同一个 EP**（都 CPU、intra 都是 1）⇒ 分流只该换"哪条
    //    线程算"，**不该换一个字节** ⇒ K=1 时守卫顺手断言这一点（见 ③）。
    cfg.model_path_cpu = cpu_rows > 0 ? ts : std::string();
    cfg.cpu_rows = cpu_rows;
    cfg.cpu_sessions = cpu_sessions;

    // 池形状自述：K ≤ workers 时每线程一棵（与"一棵树一条线程"同形）；否则每线程持 k 棵并肩。
    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int w_eff = std::min(workers > 0 ? workers : hw, static_cast<int>(K));
    std::fprintf(stderr, "[t] battles done K=%zu workers=%d（实际 %d 条，每线程 %d 棵）\n", K, workers,
                 w_eff, static_cast<int>((K + static_cast<std::size_t>(w_eff) - 1) /
                                         static_cast<std::size_t>(w_eff)));

    // **池化路径**：池宽 < 任务数 ⇒ 走"跑完一场立刻补一场"（生产形状）。池宽 1 的断言最强（逐位）。
    if (pool > 0)
    {
        checkPooled(battles, seeds, cfg, pool);
        std::fprintf(stderr, "✅ 池化路径全过（池宽 %zu / 任务数 %zu）\n", pool, K);
        return 0;
    }
    // ⓪ **反复调用**（守卫就是这种用法：同一进程里几十次）——它压的是
    //    "前向服务 / `OnnxEvaluator` 的生命周期与清理"这类**只在第 N 次才炸**的问题。
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
        if (a[g].term.is_win != b[g].term.is_win || a[g].term.cur_hp != b[g].term.cur_hp)
        {
            fail("同参数两次跑的终局读数不同（场 " + std::to_string(g) + "）");
        }
    }
    std::fprintf(stderr, "  ✓ 终局读数两次跑一致（%zu 场）\n", a.size());

    // ③ **尾段分流**开着再跑一次：与关着比读数。
    //    K=1（批恒 1 行、两侧同存档同 EP）⇒ **逐位必须相同**：分流只换"哪条线程算"。
    //    K>1 ⇒ 分流会改批的划分 ⇒ GEMM 归约顺序变 ⇒ 只管"帧仍然自洽"，不作逐位断言。
    if (cpu_rows > 0)
    {
        const std::vector<GameOut> c = runAll(battles, seeds, cfg);
        checkFrames(c, "尾段分流");
        const std::vector<std::string> dc = digest(c);
        std::size_t diff = 0;
        for (std::size_t i = 0; i < std::min(dc.size(), da.size()); ++i)
        {
            if (dc[i] != da[i])
            {
                ++diff;
            }
        }
        const std::string how = "尾段分流（CPU×" + std::to_string(cpu_sessions) + "，≤" +
                                std::to_string(cpu_rows) + " 行）";
        if (K == 1)
        {
            if (dc != da)
            {
                fail(how + "与不分流的读数不同（K=1 时两侧同存档同 EP ⇒ 应当逐位相同；不同 " +
                     std::to_string(diff) + " 行）");
            }
            std::fprintf(stderr, "  ✓ %s 与不分流：%zu 行读数逐位相同（K=1）\n", how.c_str(),
                         dc.size());
        }
        else
        {
            std::fprintf(stderr, "  ✓ %s：帧自洽；与不分流比 %zu/%zu 行读数不同（K=%zu，分流改批"
                                 "划分 ⇒ 归约顺序变，不作逐位断言）\n",
                         how.c_str(), diff, dc.size(), K);
        }
    }
    std::fprintf(stderr, "✅ 全过\n");
    return 0;
}