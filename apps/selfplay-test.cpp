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
//   selfplay-test <net.onnx> 8 4 1 --tail-guard  # **尾段**（worker 就地前向）：池宽 1 ⇒ 首次交付后由 CPU
//                                             #   会话接手；同存档同 EP ⇒ 与逐场单跑逐位相同
//   selfplay-test <net.onnx> --sweep 96   # **扫描**：`_chk_selfplay` ② 那张表（多遭遇 × 多 seed，
//                                       #  含 act4 / CARD_SELECT 帧）——只跑 CULTIST@4 会漏整类形状。
//   selfplay-test <net.onnx> --stall-guard      # 交付环满 ⇒ 补场重试
//   selfplay-test <net.onnx> --emit-throw-guard # 交付抛异常必须可捕获（不是 terminate）
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
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
std::vector<GameOut> runAll(const std::vector<BattleContext> &battles, const SelfplayConfig &cfg)
{
    std::vector<GameOut> outs(battles.size());
    selfplay(battles, cfg, [&](std::size_t j, GameOut &&g)
             { outs[j] = std::move(g); });
    return outs;
}

// 池化路径（**池宽 < 任务数** ⇒ 走"跑完一场立刻补一场"那条路）。这条路上的下标逻辑与
// `start_task`/`emitted` 计数是独立的，`runAll`（池宽 = 任务数）根本碰不到它 ⇒ 单独守。
std::vector<GameOut> runAllPooled(const std::vector<BattleContext> &battles,
                                  const SelfplayConfig &cfg, std::size_t pool)
{
    std::vector<GameOut> outs(battles.size());
    std::vector<BattleContext> tasks;
    tasks.reserve(battles.size());
    for (const BattleContext &battle : battles)
    {
        tasks.push_back(battle);
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
void checkPooled(const std::vector<BattleContext> &battles, const SelfplayConfig &cfg,
                 std::size_t pool)
{
    const std::size_t K = battles.size();
    std::vector<BattleContext> tasks;
    tasks.reserve(K);
    for (const BattleContext &battle : battles)
    {
        tasks.push_back(battle);
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
            const std::vector<GameOut> ref = runAll({battles[j]}, cfg); // 单跑（批恒 1 行）
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
            const auto s = static_cast<std::uint64_t>(seed);
            std::vector<BattleContext> battles;
            battles.push_back(buildBattle(s, c.enc, c.floor, ascensionOf(s)));
            SelfplayConfig cfg;
            cfg.model_path = ts;
            cfg.z_spec = "enginenorm";
            cfg.device = "cpu";
            cfg.sims = sims;
            cfg.sims_cheap_frac = 0.0; // 守卫要的是精确 sims（两档随机化预算只在采集里开）
            cfg.max_steps = 40;
            cfg.pool = 1; // 单场：池宽恒 1
            cfg.workers = workers;
            cfg.c_puct = 1.5;
            cfg.q_init = 0.0;
            const std::vector<GameOut> outs = runAll(battles, cfg);
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

// 交付环（`D = min(K, 8S)`）相关的两条路，都必须跑完：
//   A. **环满 ⇒ 补场重试**。复现姿势（2026-10-16 实测）：池宽 1（`D = 8`）、把**首次交付**放慢 ⇒
//      树槽在 `started - emitted` 顶到 8 之后全部跑空，池子停在 9/K 不动。
//   B. **环满时的并发补场**。`K > D`（池宽 1 时 `D = 8`）⇒ 整轮都在"跑完一场补一场"里，`retire`
//      与主线程交付后的补场会**同时**想开新场 —— 两个触发源必须抢不到同一个空槽（否则同槽开两场、
//      其中一场永远不落交付槽，实测报"交付槽被覆盖"）。A 那条覆盖不到它（A 里 worker 是睡着的）。
// ⚠️ 判据是"**跑完**"而不是"没崩"：这类 bug 的表现是**静静地不返回**，所以配看门狗，超时就报错
//    （否则守卫自己会永远挂着，看不出是失败）。
void deliveryGuard(const std::string &ts, int sims)
{
    const auto make = [&](std::size_t j)
    { return buildBattle(j, MonsterEncounter::CULTIST, 4, ascensionOf(j)); };
    const auto run = [&](std::size_t K, std::size_t pool, long slow_first_us, const char *what)
    {
        SelfplayConfig cfg;
        cfg.model_path = ts;
        cfg.z_spec = "enginenorm";
        cfg.device = "cpu";
        cfg.sims = sims;
        cfg.sims_cheap_frac = 0.0; // 同上：延迟/交付守卫不掺随机化预算
        cfg.max_steps = 40;
        cfg.workers = static_cast<int>(std::min<std::size_t>(pool, 4));
        cfg.c_puct = 1.5;
        cfg.q_init = 0.0;

        std::vector<BattleContext> tasks;
        for (std::size_t j = 0; j < K; ++j)
        {
            tasks.push_back(make(j));
        }
        std::vector<GameOut> outs(K);
        std::atomic<bool> finished{false};
        std::thread watchdog(
            [&]
            {
                for (int i = 0; i < 1200; ++i) // 1200 × 50 ms = 60 s 上限
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (finished.load())
                    {
                        return;
                    }
                }
                std::fprintf(stderr, "❌ %s：60 s 内没跑完（池宽 %zu / %zu 场）\n", what, pool, K);
                std::fflush(stderr);
                std::abort(); // 挂着的是池子里的线程 ⇒ 只能把进程带走
            });
        std::size_t delivered = 0;
        const auto body = [&]
        {
            selfplayPooled(
                K, pool, [&](std::size_t j) { return tasks[j]; }, cfg,
                [&](std::size_t j, GameOut &&g)
                {
                    if (slow_first_us > 0 && delivered == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(slow_first_us));
                    }
                    if (j >= K || !outs[j].frames.empty())
                    {
                        fail(std::string(what) + "：交付下标越界或被交付两次");
                    }
                    outs[j] = std::move(g);
                    ++delivered;
                });
        };
        try
        {
            body();
        }
        catch (const std::exception &e)
        {
            finished.store(true);
            watchdog.join();
            fail(std::string(what) + "：抛异常（" + e.what() + "）");
        }
        finished.store(true);
        watchdog.join();
        if (delivered != K)
        {
            fail(std::string(what) + "：交付场数 " + std::to_string(delivered) + " ≠ " +
                 std::to_string(K));
        }
        std::fprintf(stderr, "  ✓ %s：%zu 场全交付（池宽 %zu）\n", what, delivered, pool);
    };

    run(12, 1, 1500000, "交付环满（首次交付慢 1.5 s）⇒ 补场重试");
    run(24, 1, 0, "交付环满常态（K > D，retire 与主线程交付并发补场）");
    run(24, 3, 0, "交付环满常态（池宽 3）");
}

// `emit`（写盘）抛异常：必须**能**被外层捕获，而不是 `std::terminate`。
// 旧代码在交付循环里直接 `emit(...)` ⇒ 异常往外抛时局部 `std::vector<std::thread>` 先析构，
// 还 joinable ⇒ terminate（实测：外层 catch 根本来不及）。
void emitThrowGuard(const std::string &ts, int sims)
{
    const std::size_t K = 4;
    std::vector<BattleContext> tasks;
    for (std::size_t j = 0; j < K; ++j)
    {
        tasks.push_back(buildBattle(j, MonsterEncounter::CULTIST, 4, ascensionOf(j)));
    }
    SelfplayConfig cfg;
    cfg.model_path = ts;
    cfg.z_spec = "enginenorm";
    cfg.device = "cpu";
    cfg.sims = sims;
    cfg.sims_cheap_frac = 0.0; // 同上
    cfg.max_steps = 40;
    cfg.workers = 2;
    cfg.c_puct = 1.5;
    cfg.q_init = 0.0;

    bool caught = false;
    std::size_t delivered = 0;
    try
    {
        selfplayPooled(
            K, 2, [&](std::size_t j) { return tasks[j]; }, cfg,
            [&](std::size_t, GameOut &&)
            {
                if (++delivered == 2)
                {
                    throw std::runtime_error("注入的写盘失败");
                }
            });
    }
    catch (const std::exception &e)
    {
        caught = true;
        std::fprintf(stderr, "  ✓ 交付抛异常被捕获（不是 terminate）：%s（已交付 %zu 场）\n", e.what(),
                     delivered - 1);
    }
    if (!caught)
    {
        fail("交付异常没有传出来（旧行为是 std::terminate；这里连 catch 都没进）");
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "用法：selfplay-test <net.onnx> [K=1] [sims=4] [reps=1] [--workers N]\n"
                             "      selfplay-test <net.onnx> [K=8] [sims=4] [--pool P]   # 池化（补场）路径\n"
                             "      selfplay-test <net.onnx> --sweep [sims] [n_seed] [--workers N]\n"
                             "      selfplay-test <net.onnx> [K=8] --tail-guard   # 尾段（worker 就地前向，池宽 1）\n"
                             "      selfplay-test <net.onnx> --stall-guard      # 交付环满 ⇒ 补场重试（曾停在 9/12）\n"
                             "      selfplay-test <net.onnx> --emit-throw-guard # 交付抛异常必须可捕获（不是 terminate）\n"
                             "      --workers（worker 线程数，0 = 硬件并发数）/ --pool（池宽，见 `selfplayPooled`）\n"
                             "      必须排**最后**，且一次只能给一个。\n");
        return 2;
    }
    const std::string ts = argv[1];
    int workers = 0; // 0 = 硬件并发数（库默认）
    bool sweep_mode = false; // `--sweep [sims] [n_seed]`
    int sweep_sims = 96;
    int sweep_seeds = 1;
    std::size_t pool = 0; // 0 = 不测池化路径（`> 0` ⇒ 跑 `selfplayPooled`，池宽 = 这个值）
    bool tail_guard = false;       // `--tail-guard`：尾段（worker 就地前向）与逐场单跑逐位比
    bool stall_guard = false;      // `--stall-guard`：交付环满 ⇒ 补场重试
    bool emit_throw_guard = false; // `--emit-throw-guard`：交付异常必须可捕获
    // ⚠️ 一次扫完**所有**旗标，且**只在 `--` 开头的参数上认**（位置参数跳过）：
    //    · 原版 `break` 在第一个旗标处 ⇒ 只能给一个旗标（实测把守卫坑了：想同时给 `--pool`
    //      与 `--tail-guard` 就给不上）；
    //    · 若把位置参数也当"第一个旗标"，`argc` 会被截到 2 ⇒ `--sweep` 静默退化成普通跑
    //      （守卫"看起来全过"其实没跑）—— 这是同一处踩过的第二次，所以这次连 `--sweep` 也一起
    //      收进旗标体系，不再靠 `argv[2]` 兜。
    int first_flag = -1; // 第一个旗标的下标（位置参数只取它左边；-1 = 没有旗标）
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a.rfind("--", 0) != 0) { continue; } // 位置参数：由 `argv[2..4]` 直接读
        if (first_flag < 0) { first_flag = i; }
        const bool has_val = i + 1 < argc;
        if (a == "--workers" && has_val) { workers = std::atoi(argv[i + 1]); ++i; }
        else if (a == "--pool" && has_val)
        {
            pool = static_cast<std::size_t>(std::max(1, std::atoi(argv[i + 1])));
            ++i;
        }
        else if (a == "--sweep")
        {
            sweep_mode = true;
            if (has_val && argv[i + 1][0] != '-') { sweep_sims = std::atoi(argv[i + 1]); ++i; }
            if (i + 1 < argc && argv[i + 1][0] != '-') { sweep_seeds = std::atoi(argv[i + 1]); ++i; }
        }
        else if (a == "--stall-guard") { stall_guard = true; }
        else if (a == "--tail-guard") { tail_guard = true; }
        else if (a == "--emit-throw-guard") { emit_throw_guard = true; }
        else
        {
            std::fprintf(stderr, "⚠️ 不认识的旗标：%s（忽略）\n", a.c_str());
        }
    }
    argc = first_flag < 0 ? argc : first_flag; // 位置参数只取旗标之前那几个
    if (sweep_mode)
    {
        sweep(ts, sweep_sims, sweep_seeds, workers);
        return 0;
    }
    if (stall_guard)
    {
        // ⚠️ 小 `sims`：要复现"环满"必须让 **8 场**在那 1.5 s 的交付窗口里跑完（`D = 8S = 8`）
        deliveryGuard(ts, 8);
        std::fprintf(stderr, "✅ 交付环守卫全过\n");
        return 0;
    }
    if (emit_throw_guard)
    {
        emitThrowGuard(ts, 8);
        std::fprintf(stderr, "✅ 交付异常守卫全过\n");
        return 0;
    }
    const std::size_t K = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 1;
    const int sims = argc > 3 ? std::atoi(argv[3]) : 4;
    const int reps = argc > 4 ? std::atoi(argv[4]) : 1;   // 连续调用次数（守卫那种"反复调用"模式）
    if (K < 1) { fail("K 至少 1"); }

    // 造局面（与采集/门禁**同一个入口**）。
    std::vector<BattleContext> battles;
    for (std::size_t j = 0; j < K; ++j)
    {
        battles.push_back(buildBattle(j, MonsterEncounter::CULTIST, 4, ascensionOf(j)));
    }

    SelfplayConfig cfg;
    cfg.model_path = ts;
    cfg.z_spec = "enginenorm";
    cfg.device = "cpu";
    cfg.sims = sims;
    cfg.sims_cheap_frac = 0.0; // 逐位对拍要的是精确 sims（头文件默认是生产的两档预算）
    cfg.max_steps = 40;
    cfg.pool = static_cast<int>(K); // 池宽 = 任务数 ⇒ 整表一次全开（这条路的语义）
    cfg.workers = workers;
    cfg.c_puct = 1.5;
    cfg.q_init = 0.0;
    cfg.model_path_cpu = tail_guard ? ts : std::string();

    // 池形状自述：K ≤ workers 时每线程一棵（与"一棵树一条线程"同形）；否则每线程持 k 棵并肩。
    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int w_eff = std::min(workers > 0 ? workers : hw, static_cast<int>(K));
    std::fprintf(stderr, "[t] battles done K=%zu workers=%d（实际 %d 条，每线程 %d 棵）\n", K, workers,
                 w_eff, static_cast<int>((K + static_cast<std::size_t>(w_eff) - 1) /
                                         static_cast<std::size_t>(w_eff)));

    // **池化路径**：池宽 < 任务数 ⇒ 走"跑完一场立刻补一场"（生产形状）。池宽 1 的断言最强（逐位）。
    if (pool > 0)
    {
        checkPooled(battles, cfg, pool);
        std::fprintf(stderr, "✅ 池化路径全过（池宽 %zu / 任务数 %zu）\n", pool, K);
        return 0;
    }
    // ③ 尾段（worker 就地前向）：池宽 1 ⇒ 首次交付后由 CPU 会话接手；同存档同 EP、批恒 1 行
    //    ⇒ 逐场必须与单跑逐位相同。
    if (tail_guard)
    {
        if (K < 2) { fail("--tail-guard 需要 K ≥ 2（尾段要在首次交付之后才接手）"); }
        SelfplayConfig tail_cfg = cfg;
        tail_cfg.model_path_cpu = ts;
        tail_cfg.workers = 1;
        checkPooled(battles, tail_cfg, 1);
        std::fprintf(stderr, "✅ 尾段守卫全过（%zu 场，首次交付后由 CPU 会话接手）\n", K);
        return 0;
    }
    // ⓪ **反复调用**（守卫就是这种用法：同一进程里几十次）——它压的是
    //    "前向服务 / `OnnxEvaluator` 的生命周期与清理"这类**只在第 N 次才炸**的问题。
    for (int r = 0; r < reps; ++r)
    {
        const std::vector<GameOut> tmp = runAll(battles, cfg);
        if (tmp.empty()) { fail("反复调用里有一次一帧都没走"); }
    }
    if (reps > 1) { std::fprintf(stderr, "  ✓ 连续 %d 次调用都正常\n", reps); }

    // ① 跑两次，比读数（K=1 应逐位相同；K>1 只保证"两次跑一致"这一层形态）
    SelfplayConfig baseline = cfg;
    baseline.model_path_cpu.clear();
    const std::vector<GameOut> a = runAll(battles, baseline);
    checkFrames(a, "第一次跑");
    const std::vector<GameOut> b = runAll(battles, baseline);
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

    std::fprintf(stderr, "✅ 全过\n");
    return 0;
}
