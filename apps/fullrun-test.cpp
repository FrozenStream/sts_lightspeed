//
// 整局测试：**战斗用网络决策，非战斗借用引擎自带的固定模式**（`--noncombat random` 可换回随机）。
//
// 目的：验证"网络能把一整局从头走完"这条端到端链路 —— 不只是单场战斗（`selfplay-test` 已守住
// 那一段）。整局的状态机在 `GameContext`（Neow / 地图 / 奖励 / 事件 / 商店 / 营火 / Boss 遗物 /
// 宝箱），战斗在 `BattleContext`；两层的动作空间本来就不同：
//   · 非战斗：`search::GameAction`（进节点 / 选奖励 / 选事件选项 / 买货 / 露营 / 选 Boss 遗物…）
//   · 战斗：  `search::Action`（出牌 / 药水 / 结束回合 / 选牌）
//
// 战斗怎么用网络：直接复用生产那条 MCTS（`collect::selfplay`：同一份树、同一条前向、同一个选动作
// 规则）。它是"在**副本**上整场跑完再按帧交付"的，不驱动真实 `GameContext` ⇒ 这里把交付的动作
// 序列**回放**到真实 `BattleContext` 上，再 `exitBattle(gc)` 把 HP / 牌组 / 遗物 / RNG 写回整局：
//   · 引擎对同一状态 + 同一动作是**确定**的 ⇒ 回放出的轨迹与搜索里那条完全相同（不是"重新掷一遍"）；
//   · 每步都用 `actionToIdx` **回读校验**（下标 → 动作 → 下标必须相等）：统一索引空间的逆映射一旦
//     有损（比如药水丢弃的 target 编码、选牌空间），第一步就当场失败，而不是悄悄换一条轨迹；
//   · 回放完必须 `bc.outcome == 网络交付的 outcome`，且不能是 UNDECIDED（MAX_STEPS 内没打完 = 失败）。
//   · 唯一的"状态机约定"：触发多选屏（`EXHAUST_MANY` / `GAMBLE`，只枚举"一张都不选"、
//     统一下标里没有编码）的遗物/药水/卡由 `applyRunBans`（`scenebuild.cpp` 的 ban 表）在
//     **每个非战斗动作后**追认移除 —— 与生产同一张表（`buildBattle` 在构造期过滤）⇒
//     进战斗时身上不可能带触发物，网络永远不面对多选屏。
//
// 非战斗怎么选：**借用引擎自带的固定模式**（`search::SimpleAgent::stepOutOfCombat`：按出牌序/稀有度
// 挑卡、按优先级挑 Boss 遗物、按权重挑地图路线、营火看血量决定睡/敲……）—— 也就是 `SimpleAgent`
// 打整局时用的那一套，仓库里"非战斗该怎么选"的唯一既有答案。`--noncombat random` 可换回均匀随机
// （压状态机覆盖用；两种模式都按 run seed 可复现）。
// `MATCH_AND_KEEP` / `COLOSSEUM` / `PRISMATIC_SHARD` 由引擎的 `disable*` 常量关掉（`GameContext.h`）
// ⇒ 不会撞上"这几个特例在 `getAllActionsInState` 里没有动作"的坑。
//
// 用法：
//   fullrun-test <net.onnx>                        # seed=0、sims=8、1 局
//   fullrun-test <net.onnx> 7 64                   # seed=7、sims=64（更强、更慢）
//   fullrun-test <net.onnx> 0 8 --runs 5           # 连跑 seed 0..4
//   fullrun-test <net.onnx> 0 8 --repeat 2         # 同一 seed 跑两遍，读数（校验和）必须逐位相同
//   fullrun-test <net.onnx> 0 8 --ascension 10 --max-steps 2000
//   fullrun-test <net.onnx> 0 8 --skip-battles --runs 20   # 只压非战斗状态机（战斗直接结算）
//   fullrun-test <net.onnx> 0 8 --noncombat random        # 非战斗换回均匀随机（默认是固定模式）
//
// 退出码：0 = 全部走完且自检通过；1 = 任一条自检失败（终局没定 / 回放不一致 / 界面没有可选动作）；
//         看门狗超时 = abort（非零退出码，见 `--watchdog`）。
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "action.h"   // idxToAction / actionToIdx（`sts::collect` 的动作层）
#include "selfplay.h" // SelfplayConfig / selfplay / GameOut
#include "netprobe.h" // probeForwardChecked（裸网络模式的会话建立校验）
#include "scenebuild.h" // applyRunBans（ban 表追认：触发物不进战斗）

#include "game/GameContext.h"
#include "sim/search/GameAction.h"
#include "sim/search/SimpleAgent.h"        // 非战斗的"引擎固定模式"（stepOutOfCombat）
#include "sim/search/ScumSearchAgent2.h"   // 原版**搜索** agent（UCT + 整局 playout）

#include "test_util.h"

using namespace sts;
using namespace sts::collect;

namespace
{

[[noreturn]] void fail(const std::string &what)
{
    testutil::fatal(what);
}

// ── 看门狗 ──────────────────────────────────────────────────────────────────────
// 整局测试最糟的失败形态不是"报错"，而是**静静地不返回**（2026-10-18 实测遇到过一次性挂住：
// 100% CPU、10 分钟不动、之后 100+ 局都没能复现）。所以配一条硬上限：超过 `--watchdog` 秒没有
// 任何进展（每场战斗开始/结束、每个非战斗动作都会 `touch()`）就打印现场并 abort —— 挂着的是
// 搜索/引擎里的线程，只能把进程带走（`selfplay-test` 的 `stall-guard` 同一套做法）。
std::atomic<std::int64_t> g_last_progress_ms{0};
std::atomic<bool> g_watchdog_stop{false};
std::atomic<std::uint64_t> g_current_seed{0};

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void touchProgress()
{
    g_last_progress_ms.store(nowMs(), std::memory_order_relaxed);
}

void startWatchdog(int seconds, std::string stage)
{
    if (seconds <= 0)
    {
        return;
    }
    touchProgress();
    const std::int64_t limit_ms = static_cast<std::int64_t>(seconds) * 1000;
    std::thread([limit_ms, stage = std::move(stage)]
                {
                    while (!g_watchdog_stop.load(std::memory_order_relaxed))
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        const std::int64_t idle = nowMs() - g_last_progress_ms.load(std::memory_order_relaxed);
                        if (idle <= limit_ms)
                        {
                            continue;
                        }
                        std::fprintf(stderr,
                                     "\n❌ 看门狗：seed=%llu 已 %lld ms 没有进展（%s）——"
                                     "卡在某次搜索/引擎调用里；先加 `--verbose` 重跑定位。\n",
                                     static_cast<unsigned long long>(
                                         g_current_seed.load(std::memory_order_relaxed)),
                                     static_cast<long long>(idle), stage.c_str());
                        std::fflush(stderr);
                        std::abort();
                    }
                })
        .detach();
}

// 失败信息里要能定位"哪一局、走到哪了" —— 否则整局测试报错等于只说了"炸了"。
std::string where(const GameContext &gc)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "seed=%llu 层=%d 幕=%d screen=%d event=%d",
                  static_cast<unsigned long long>(gc.seed), gc.floorNum, gc.act,
                  static_cast<int>(gc.screenState), static_cast<int>(gc.curEvent));
    return buf;
}

std::string battleWhere(const BattleContext &bc)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s 层=%d 幕=%d",
                  monsterEncounterStrings[static_cast<int>(bc.encounter)], bc.floorNum,
                  (bc.floorNum - 1) / 17 + 1);
    return buf;
}

// 一串动作 + 终局状态的**顺序敏感**校验和：两次同 seed 跑出来必须一样。
void mix(std::uint64_t &h, std::uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

SelfplayConfig netConfig(const std::string &onnx, int sims, int max_steps, const std::string &z_spec)
{
    SelfplayConfig cfg;
    cfg.model_path = onnx;
    cfg.device = "cpu"; // 测试不占卡；CUDA 核非确定，本来也不该进"可复现"的断言
    cfg.z_spec = z_spec;
    cfg.sims = sims;
    cfg.sims_cheap_frac = 0.0; // 整局测试要的是精确 sims（头文件默认是生产的两档预算）
    cfg.max_steps = max_steps;
    cfg.pool = 1; // 单场：一次只有一棵树在飞（批恒 1 行 ⇒ 与网络单跑逐位一致）
    cfg.workers = 1;
    // 生产配方（`zspec.DEFAULT_SPEC = "hp"`）的两个搜索常数：`c_puct_for("hp") = 1.5`、
    // `q_init_for("hp") = 0`。⚠️ 换口径必须一起换（`zspec.py` 的 `Z_SCALE` / `Z_MID`）——
    // 本测试只钉死生产这一档，别的口径会 `--z-spec` 时给出告警（见 `main`）。
    cfg.c_puct = 1.5;
    cfg.q_init = 0.0;
    return cfg;
}

// 一场战斗：网络整场跑完（副本）→ 回放到真实 `bc` → 校验终局。返回决策点数。
std::int64_t playBattleWithNet(const SelfplayConfig &cfg, BattleContext &bc, std::uint64_t &digest,
                               bool verbose)
{
    if (verbose)
    {
        std::fprintf(stderr, "  战斗开始：%s\n", battleWhere(bc).c_str());
        std::fflush(stderr);
    }

    GameOut out;
    selfplay(std::vector<BattleContext>{bc}, cfg,
             [&](std::size_t, GameOut &&g)
             { out = std::move(g); });

    for (std::size_t i = 0; i < out.frames.size(); ++i)
    {
        const Frame &f = out.frames[i];
        const search::Action a = idxToAction(bc, f.a_idx);
        const int round_trip = actionToIdx(bc, a);
        if (round_trip != f.a_idx)
        {
            fail("战斗回放：" + battleWhere(bc) + " 第 " + std::to_string(i) +
                 " 帧的动作下标对不上（交付 idx=" + std::to_string(f.a_idx) + "，回读=" +
                 std::to_string(round_trip) + "）");
        }
        a.execute(bc);
        mix(digest, static_cast<std::uint64_t>(f.a_idx) * 2 + 1);
    }

    if (bc.outcome != out.outcome)
    {
        fail("战斗回放：" + battleWhere(bc) + " 的终局与网络交付的不一致（回放=" +
             std::to_string(static_cast<int>(bc.outcome)) + "，交付=" +
             std::to_string(static_cast<int>(out.outcome)) + "）");
    }
    if (bc.outcome == Outcome::UNDECIDED)
    {
        fail("战斗在 max_steps=" + std::to_string(cfg.max_steps) + " 内没打完：" + battleWhere(bc) +
             "（网络在绕圈 / 局面卡死，先看这一场的轨迹）");
    }

    mix(digest, static_cast<std::uint64_t>(out.frames.size()));
    mix(digest, static_cast<std::uint64_t>(bc.outcome) + 0x1000);
    if (verbose)
    {
        std::fprintf(stderr, "  战斗结束：%s 决策=%zu → %s（hp=%d/%d）\n", battleWhere(bc).c_str(),
                     out.frames.size(),
                     bc.outcome == Outcome::PLAYER_VICTORY ? "胜" : "负", bc.player.curHp,
                     bc.player.maxHp);
        std::fflush(stderr);
    }
    return static_cast<std::int64_t>(out.frames.size());
}

// 裸网络打完一场：**不建树、不搜索**，每步在合法动作里取 policy 头 logits 最大的那个
// （`CARD_SELECT` 用 select 头）。这条政策与 `eval.cpp` 的 `playNetBatch` 完全一致 —— 门控量
// "网络本身"用的就是它，所以这里的数可以直接和门控读数对照。
std::int64_t playBattleBare(sts::py::NetEvaluator &ev, BattleContext &bc, std::uint64_t &digest,
                            int max_steps, bool verbose)
{
    constexpr int kObsLen = Snap::snapshotLen;
    std::int64_t decisions = 0;
    if (verbose)
    {
        std::fprintf(stderr, "  战斗开始（裸网）：%s\n", battleWhere(bc).c_str());
        std::fflush(stderr);
    }

    while (bc.outcome == Outcome::UNDECIDED)
    {
        if (decisions >= max_steps)
        {
            fail("战斗在 max_steps=" + std::to_string(max_steps) + " 内没打完：" + battleWhere(bc) +
                 "（裸网在绕圈 / 局面卡死）");
        }
        const bool is_sel = bc.inputState == InputState::CARD_SELECT;
        const std::vector<std::pair<search::Action, int>> legal = legalActionsWithIdx(bc);
        if (legal.empty())
        {
            fail("战斗里没有合法动作：" + battleWhere(bc));
        }
        const auto obs = snapshotOf(bc);
        const auto fwd = ev.eval(obs.data(), 1, kObsLen); // 指针只在下一次 eval 前有效 ⇒ 立刻读
        const float *logits = is_sel ? fwd.ps : fwd.pa;
        std::size_t best = 0;
        for (std::size_t i = 1; i < legal.size(); ++i)
        {
            if (logits[legal[i].second] > logits[legal[best].second])
            {
                best = i;
            }
        }

        mix(digest, static_cast<std::uint64_t>(legal[best].second) * 2 + 5);
        legal[best].first.execute(bc);
        ++decisions;
    }

    mix(digest, static_cast<std::uint64_t>(decisions) + 0x2000);
    mix(digest, static_cast<std::uint64_t>(bc.outcome) + 0x3000);
    if (verbose)
    {
        std::fprintf(stderr, "  战斗结束（裸网）：%s 决策=%lld → %s（hp=%d/%d）\n",
                     battleWhere(bc).c_str(), static_cast<long long>(decisions),
                     bc.outcome == Outcome::PLAYER_VICTORY ? "胜" : "负", bc.player.curHp,
                     bc.player.maxHp);
        std::fflush(stderr);
    }
    return decisions;
}

struct RunResult
{
    GameOutcome outcome = GameOutcome::UNDECIDED;
    int floor = 0;
    int act = 0;
    int hp = 0;
    int max_hp = 0;
    int gold = 0;
    int deck = 0;
    int relics = 0;
    int battles = 0;
    std::int64_t net_decisions = 0;
    std::int64_t noncombat = 0;
    std::uint64_t digest = 0;
    double seconds = 0.0;
};

// 战斗的两种打法：
//   · Search —— `collect::selfplay` 的 MCTS（生产同一条；`sims` 轮决策点预算）；
//   · Bare   —— **裸网络**：一步搜索都不跑，直接在每个决策点取"合法动作里 policy 头 logits 最大"
//               （CARD_SELECT 用 select 头）。与 `eval.cpp` 的 `playNetBatch`（门控用它量裸网）同一条政策。
enum class BattlePolicy
{
    Search,
    Bare,
    Agent, // 原版启发式：`SimpleAgent::playoutBattle`（**没有搜索**，纯手写优先级）
    Scum   // 原版搜索 agent：`ScumSearchAgent2::playout`（UCT + 整局随机 playout，`--agent-sims` 轮/次）
};

// 非战斗环节的两档策略：
//   · Engine —— **借用引擎自带的固定模式**（`search::SimpleAgent` 的 `stepOutOfCombat`）：按出牌
//     顺序/稀有度挑卡、按优先级挑 Boss 遗物、按权重挑地图路线、营火看血量决定睡/敲……这是
//     `SimpleAgent` 打整局时用的那一套，也是仓库里"非战斗该怎么选"的唯一既有答案。
//   · Random —— 均匀随机（压状态机覆盖用：会走到固定模式不去的地方，但牌组质量随机）。
enum class NonCombatPolicy
{
    Engine,
    Random
};

RunResult playRun(const SelfplayConfig &cfg, std::uint64_t seed, int ascension, bool verbose,
                  bool skip_battles, NonCombatPolicy noncombat_policy, BattlePolicy battle_policy,
                  int agent_sims)
{
    const auto t0 = std::chrono::steady_clock::now();
    GameContext gc(CharacterClass::IRONCLAD, seed, ascension);

    // ── 纯原版**搜索** agent：整局交给 `ScumSearchAgent2::playout` ─────────────────────
    // 这是仓库里"原版 agent"的本体（`apps/test.cpp` 的 `agentMt` 就是这么跑的）：UCT +
    // 每个 simulation 打完一整场（`BattleScumSearcher2::step` → `playoutRandom`），非战斗也用它
    // 自己的策略。它与"网络战斗 / 引擎非战斗"的混合驱动是两套东西 ⇒ 这里整局交给它、直接返回。
    // ⚠️ 每个 search 之后会顺着解走 `stepsWithSolution=15` 步才重搜 ⇒ 墙钟由 `agent_sims` 决定。
    if (battle_policy == BattlePolicy::Scum)
    {
        search::ScumSearchAgent2 scum;
        scum.simulationCountTotal = 0; // ⚠️ 这个字段没有默认值（未初始化）⇒ 必须显式清零
        scum.simulationCountBase = agent_sims;
        scum.rng = std::default_random_engine(static_cast<unsigned>(seed));
        scum.playout(gc);

        RunResult r;
        r.outcome = gc.outcome;
        r.floor = gc.floorNum;
        r.act = gc.act;
        r.hp = gc.curHp;
        r.max_hp = gc.maxHp;
        r.gold = gc.gold;
        r.deck = static_cast<int>(gc.deck.size());
        r.relics = static_cast<int>(gc.relics.relics.size());
        r.net_decisions = scum.simulationCountTotal; // 这一档记的是**搜索模拟数**，不是决策数
        r.digest = 0xcbf29ce484222325ULL;
        mix(r.digest, static_cast<std::uint64_t>(r.outcome));
        mix(r.digest, static_cast<std::uint64_t>(r.floor));
        mix(r.digest, static_cast<std::uint64_t>(r.hp));
        mix(r.digest, static_cast<std::uint64_t>(r.gold));
        mix(r.digest, static_cast<std::uint64_t>(r.deck));
        mix(r.digest, static_cast<std::uint64_t>(r.net_decisions));
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    }

    // `--skip-battles`：引擎的调试开关（`enterBattle` 直接结算）⇒ 不经过网络，专门压**非战斗**
    // 那一整套界面（奖励 / 事件 / 商店 / 营火 / 宝箱 / Boss 遗物 / 幕切换 / 心脏）。
    gc.skipBattles = skip_battles;
    std::mt19937_64 random(seed * 1000003ULL + 17ULL); // 随机模式的流（按 seed 播种 ⇒ 可复现）
    // 固定模式的载体：`mapPath` 这类状态跨界面保留（地图路线是一次算好、逐层走的）⇒ 每局一个。
    search::SimpleAgent agent;
    agent.print = false;
    // 裸网络模式：一个会话跑完整局（每场重建会话要多花几百 ms，测试不必）。
    std::unique_ptr<sts::py::NetEvaluator> bare_eval;
    if (battle_policy == BattlePolicy::Bare && !skip_battles)
    {
        bare_eval = std::make_unique<sts::py::NetEvaluator>(cfg.model_path, cfg.device, 1);
        probeForwardChecked(*bare_eval); // 头宽必须等于动作空间（存档与 `azlayout` 漂了当场抛）
    }

    RunResult r;
    r.digest = 0xcbf29ce484222325ULL;
    constexpr std::int64_t kMaxActions = 200000; // 保险丝：整局状态机原地打转时别挂死

    while (gc.outcome == GameOutcome::UNDECIDED)
    {
        if (r.net_decisions + r.noncombat > kMaxActions)
        {
            fail("整局在 " + std::to_string(kMaxActions) + " 个动作内没走完（" + where(gc) + "）");
        }

        if (gc.screenState == ScreenState::BATTLE)
        {
            BattleContext bc;
            bc.init(gc);
            touchProgress();           // 看门狗：进战斗（搜索期间没有别的进展信号）
            if (battle_policy == BattlePolicy::Bare)
            {
                r.net_decisions += playBattleBare(*bare_eval, bc, r.digest, cfg.max_steps, verbose);
            }
            else if (battle_policy == BattlePolicy::Agent)
            {
                // 纯原版参照：引擎自带的战斗策略（与 `SimpleAgent::playout` 里那条一模一样）。
                // 只记账：`playoutBattle` 自己的动作都进 `agent.actionHistory`。
                const std::size_t before = agent.actionHistory.size();
                agent.playoutBattle(bc);
                for (std::size_t i = before; i < agent.actionHistory.size(); ++i)
                {
                    mix(r.digest, static_cast<std::uint64_t>(agent.actionHistory[i]) * 2 + 7);
                }
                if (bc.outcome == Outcome::UNDECIDED)
                {
                    fail("原版 agent 的战斗没打完：" + battleWhere(bc));
                }
                r.net_decisions += static_cast<std::int64_t>(agent.actionHistory.size() - before);
                if (verbose)
                {
                    std::fprintf(stderr, "  战斗结束（原版 agent）：%s 决策=%zu → %s（hp=%d/%d）\n",
                                 battleWhere(bc).c_str(),
                                 agent.actionHistory.size() - before,
                                 bc.outcome == Outcome::PLAYER_VICTORY ? "胜" : "负",
                                 bc.player.curHp, bc.player.maxHp);
                    std::fflush(stderr);
                }
            }
            else
            {
                r.net_decisions += playBattleWithNet(cfg, bc, r.digest, verbose);
            }
            bc.exitBattle(gc); // HP / 牌组 / 遗物 / 药水 / RNG 写回整局
            touchProgress();
            ++r.battles;
            continue;
        }

        if (verbose)
        {
            std::fprintf(stderr, "  [%lld] %s\n",
                         static_cast<long long>(r.net_decisions + r.noncombat), where(gc).c_str());
            std::fflush(stderr);
        }

        if (noncombat_policy == NonCombatPolicy::Engine)
        {
            // 引擎自带的固定模式。`stepOutOfCombat` 一次调用可能落**多**个动作（奖励屏先拿药水再
            // 选牌；act4 的地图屏先转场再走节点）⇒ 依 `actionHistory` 的增长逐条记账。
            const std::size_t before = agent.actionHistory.size();
            agent.curGameContext = &gc;
            agent.stepOutOfCombat(gc);
            // 奖励 / 商店 / 事件可能发出触发物（Gambling Chip / Elixir / 无色 Purity…）⇒
            // 每个非战斗动作后追认一遍 ban 表，与生产（`buildBattle` 构造期过滤）同一张表。
            applyRunBans(gc);
            const std::size_t after = agent.actionHistory.size();
            if (after == before)
            {
                fail("固定模式在这个界面没有给出动作（" + where(gc) + "）");
            }
            for (std::size_t i = before; i < after; ++i)
            {
                mix(r.digest, static_cast<std::uint64_t>(agent.actionHistory[i]) * 2 + 3);
            }
            if (verbose)
            {
                std::fprintf(stderr, "        固定模式动作 %zu 个（末个 bits=%d）\n", after - before,
                             agent.actionHistory.back());
                std::fflush(stderr);
            }
            r.noncombat += static_cast<std::int64_t>(after - before);
            touchProgress();
            continue;
        }

        const std::vector<search::GameAction> all = search::GameAction::getAllActionsInState(gc);
        std::vector<search::GameAction> legal;
        legal.reserve(all.size());
        for (const search::GameAction &a : all)
        {
            if (a.isValidAction(gc))
            {
                legal.push_back(a);
            }
        }
        if (legal.empty())
        {
            fail("非战斗界面没有可选动作（" + where(gc) + "，枚举出 " + std::to_string(all.size()) +
                 " 个、全部无效）");
        }

        std::uniform_int_distribution<std::size_t> pick(0, legal.size() - 1);
        const search::GameAction a = legal[pick(random)];
        if (verbose)
        {
            std::fprintf(stderr, "        动作=%u（可选 %zu 个）\n", a.bits, legal.size());
            std::fflush(stderr);
        }
        mix(r.digest, static_cast<std::uint64_t>(a.bits) * 2 + 3);
        a.execute(gc);
        applyRunBans(gc); // 同上：触发物（ban 表）不许带进战斗
        touchProgress();
        ++r.noncombat;
    }

    r.outcome = gc.outcome;
    r.floor = gc.floorNum;
    r.act = gc.act;
    r.hp = gc.curHp;
    r.max_hp = gc.maxHp;
    r.gold = gc.gold;
    r.deck = static_cast<int>(gc.deck.size());
    r.relics = static_cast<int>(gc.relics.relics.size());
    mix(r.digest, static_cast<std::uint64_t>(r.outcome));
    mix(r.digest, static_cast<std::uint64_t>(r.floor));
    mix(r.digest, static_cast<std::uint64_t>(r.hp));
    mix(r.digest, static_cast<std::uint64_t>(r.gold));
    mix(r.digest, static_cast<std::uint64_t>(r.deck));
    mix(r.digest, static_cast<std::uint64_t>(r.relics));
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

void printResult(std::size_t index, std::size_t runs, std::uint64_t seed, const RunResult &r,
                 BattlePolicy battle_policy)
{
    const char *progress = battle_policy == BattlePolicy::Scum ? "搜索模拟" : "战斗决策";
    char battles[32];
    if (battle_policy == BattlePolicy::Scum)
    {
        std::snprintf(battles, sizeof(battles), "-"); // 整局交给 agent，不单独数场次
    }
    else
    {
        std::snprintf(battles, sizeof(battles), "%d", r.battles);
    }
    std::fprintf(stderr,
                 "[%zu/%zu seed=%llu] %s 幕=%d 层=%d hp=%d/%d 金=%d 牌=%d 遗物=%d 战=%s "
                 "%s=%lld 非战斗=%lld 校验和=%016llx %.1fs\n",
                 index + 1, runs, static_cast<unsigned long long>(seed),
                 r.outcome == GameOutcome::PLAYER_VICTORY ? "胜" : "负", r.act, r.floor, r.hp,
                 r.max_hp, r.gold, r.deck, r.relics, battles, progress,
                 static_cast<long long>(r.net_decisions), static_cast<long long>(r.noncombat),
                 static_cast<unsigned long long>(r.digest), r.seconds);
    std::fflush(stderr);
}

void usage()
{
    std::fprintf(stderr,
                 "用法：fullrun-test <net.onnx> [seed=0] [sims=8] [--runs N] [--repeat N]\n"
                 "                   [--ascension N] [--max-steps N] [--z-spec NAME]\n"
                 "\n"
                 "  战斗用网络（`collect::selfplay` 的 MCTS），非战斗用**引擎自带的固定模式**\n"
                 "  （`SimpleAgent::stepOutOfCombat`：地图 / 奖励 / 事件 / 商店 / 营火 / Boss 遗物 /\n"
                 "  宝箱）；`--noncombat random` 换回均匀随机。每局必须走到胜负。\n"
                 "\n"
                 "  --runs N      从 seed 起连跑 N 局（默认 1）\n"
                 "  --repeat N    每局跑 N 遍，要求校验和逐位相同（默认 1；2 = 可复现性自检）\n"
                 "  --ascension N 进阶等级（默认 0）\n"
                 "  --max-steps N 单场战斗的决策点上限（默认 1000，与生产 `MAX_STEPS` 同）\n"
                 "  --z-spec N    终局价值口径（默认 hp，与生产 `zspec.DEFAULT_SPEC` 同；\n"
                 "                搜索常数只按 hp 标定 —— 换口径要一起换 `c_puct`/`q_init`）\n"
                 "  --bare        战斗用**裸网络**（0 次搜索：合法动作里取 policy argmax），\n"
                 "                与门控 `eval_run` 量网络本身那条政策一致；也等价于位置参数 sims=0\n"
                 "  --battle M    战斗策略：search（默认，MCTS）/ bare（裸网络）/ agent（原版**启发式**，\n"
                 "                无搜索）/ scum（原版**搜索** agent `ScumSearchAgent2`，整局交给它）\n"
                 "  --agent-sims N  `scum` 档的 `simulationCountBase`（默认 5；每轮搜一次整场 playout）\n"
                 "  --watchdog N  多久没有进展就判卡死并 abort（秒，默认 300；0 = 关）\n"
                 "  --noncombat M 非战斗策略：engine = 引擎自带的固定模式（`SimpleAgent::stepOutOfCombat`，\n"
                 "                默认）/ random = 均匀随机（压状态机覆盖用）\n"
                 "  --verbose     逐界面 / 逐战斗打印（排查卡住时用）\n"
                 "  --skip-battles 引擎的调试开关：战斗直接结算（不走网络）⇒ 专门压**非战斗**状态机\n"
                 "                （奖励 / 事件 / 商店 / 营火 / Boss 遗物 / 幕切换 / act4）。\n"
                 "                网络不会加载 ⇒ 第一个参数给个**占位路径**即可（不要求真的 .onnx）\n");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage();
        return 2;
    }

    const std::string onnx = argv[1];
    std::uint64_t seed0 = 0;
    int sims = 8;
    int runs = 1;
    int repeat = 1;
    int ascension = 0;
    int max_steps = 1000;
    std::string z_spec = "hp"; // 生产口径（`zspec.DEFAULT_SPEC`）；训练好的网就是按它训的
    bool verbose = false;
    bool skip_battles = false;
    int watchdog = 300;
    NonCombatPolicy noncombat_policy = NonCombatPolicy::Engine;
    BattlePolicy battle_policy = BattlePolicy::Search;
    int agent_sims = 5; // `ScumSearchAgent2::simulationCountBase`（`apps/test.cpp` 的默认值）

    int positional = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        const bool has_val = i + 1 < argc;
        if (a == "--runs" && has_val)
        {
            runs = std::max(1, std::atoi(argv[++i]));
        }
        else if (a == "--repeat" && has_val)
        {
            repeat = std::max(1, std::atoi(argv[++i]));
        }
        else if (a == "--ascension" && has_val)
        {
            ascension = std::max(0, std::atoi(argv[++i]));
        }
        else if (a == "--max-steps" && has_val)
        {
            max_steps = std::max(2, std::atoi(argv[++i]));
        }
        else if (a == "--z-spec" && has_val)
        {
            z_spec = argv[++i];
        }
        else if (a == "--verbose")
        {
            verbose = true;
        }
        else if (a == "--skip-battles")
        {
            skip_battles = true;
        }
        else if (a == "--watchdog" && has_val)
        {
            watchdog = std::max(0, std::atoi(argv[++i]));
        }
        else if (a == "--bare")
        {
            battle_policy = BattlePolicy::Bare; // 裸网络：sims 归 0，一步搜索都不跑
            sims = 0;
        }
        else if (a == "--battle" && has_val)
        {
            const std::string mode = argv[++i];
            if (mode == "search")
            {
                battle_policy = BattlePolicy::Search;
            }
            else if (mode == "bare")
            {
                battle_policy = BattlePolicy::Bare;
                sims = 0;
            }
            else if (mode == "agent")
            {
                battle_policy = BattlePolicy::Agent; // 原版**启发式**（`SimpleAgent`，无搜索）
            }
            else if (mode == "scum")
            {
                battle_policy = BattlePolicy::Scum; // 原版**搜索** agent（`ScumSearchAgent2`）
            }
            else
            {
                std::fprintf(stderr,
                             "⚠️ --battle 只认 search / bare / agent / scum（给的是 %s）\n",
                             mode.c_str());
            }
        }
        else if (a == "--agent-sims" && has_val)
        {
            // `ScumSearchAgent2::simulationCountBase`：每次搜索做几轮**整场随机 playout**
            // （`apps/test.cpp` 的 `agentMt` 默认 = 5；这个数直接决定墙钟）。
            agent_sims = std::max(1, std::atoi(argv[++i]));
        }
        else if (a == "--noncombat" && has_val)
        {
            const std::string mode = argv[++i];
            if (mode == "engine")
            {
                noncombat_policy = NonCombatPolicy::Engine;
            }
            else if (mode == "random")
            {
                noncombat_policy = NonCombatPolicy::Random;
            }
            else
            {
                std::fprintf(stderr, "⚠️ --noncombat 只认 engine / random（给的是 %s）\n",
                             mode.c_str());
            }
        }
        else if (a.size() >= 2 && a[0] == '-' && a[1] == '-')
        {
            std::fprintf(stderr, "⚠️ 不认识的旗标：%s（忽略）\n", a.c_str());
        }
        else if (positional == 0)
        {
            seed0 = std::strtoull(a.c_str(), nullptr, 10);
            ++positional;
        }
        else if (positional == 1)
        {
            const int v = std::atoi(a.c_str());
            if (v <= 0)
            {
                battle_policy = BattlePolicy::Bare; // `sims 0` = 裸网络（与 `--bare` 同义）
                sims = 0;
            }
            else
            {
                sims = std::max(2, v);
            }
            ++positional;
        }
        else
        {
            std::fprintf(stderr, "⚠️ 多余的位置参数：%s（忽略）\n", a.c_str());
        }
    }

    const SelfplayConfig cfg = netConfig(onnx, sims, max_steps, z_spec);
    if (z_spec != "hp")
    {
        std::fprintf(stderr,
                     "⚠️ z_spec=%s：本测试的 `c_puct` / `q_init` 是 **hp** 口径的常数"
                     "（1.5 / 0）；换口径要按 `zspec.py` 的 `Z_SCALE` / `Z_MID` 一起换。\n",
                     z_spec.c_str());
    }
    std::fprintf(stderr,
                 "整局测试：net=%s 战斗=%s 局数=%d 每局重复=%d 进阶=%d max_steps=%d z=%s"
                 " 非战斗=%s%s\n",
                 onnx.c_str(),
                 battle_policy == BattlePolicy::Bare
                     ? "裸网络（无搜索）"
                     : (battle_policy == BattlePolicy::Agent
                            ? "原版启发式（SimpleAgent）"
                            : (battle_policy == BattlePolicy::Scum ? "原版搜索 agent（ScumSearchAgent2）"
                                                                   : "MCTS")),
                 runs, repeat, ascension, max_steps, z_spec.c_str(),
                 noncombat_policy == NonCombatPolicy::Engine ? "engine（固定模式）" : "random",
                 skip_battles ? "（跳过战斗：只压非战斗界面）" : "");
    if (battle_policy == BattlePolicy::Search)
    {
        std::fprintf(stderr, "  搜索预算 sims=%d\n", sims);
    }
    else if (battle_policy == BattlePolicy::Scum)
    {
        std::fprintf(stderr, "  agent 预算 simulationCountBase=%d（每次搜索的整场 playout 数）\n",
                     agent_sims);
        if (watchdog > 0)
        {
            // 整局是**一次不透明的 `playout` 调用**：搜一次 5 万轮 playout 本来就可能几分钟，
            // 看门狗分不清"慢"与"挂" ⇒ 这一档自动关掉（成本用 `--agent-sims` 控制）。
            std::fprintf(stderr, "  （scum 档自动关闭看门狗：单场搜索可能就需要几分钟）\n");
            watchdog = 0;
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    int wins = 0;
    int losses = 0;
    std::int64_t total_battles = 0;
    std::int64_t total_net = 0;
    std::int64_t total_noncombat = 0;
    std::int64_t total_floors = 0;

    startWatchdog(watchdog,
                  (skip_battles ? "跳过战斗 / 非战斗状态机" : "网络战斗") +
                      std::string(" + ") +
                      (noncombat_policy == NonCombatPolicy::Engine ? "固定模式非战斗"
                                                                   : "随机非战斗"));

    for (int k = 0; k < runs; ++k)
    {
        const std::uint64_t seed = seed0 + static_cast<std::uint64_t>(k);
        g_current_seed.store(seed, std::memory_order_relaxed);
        RunResult first;
        try
        {
            first = playRun(cfg, seed, ascension, verbose, skip_battles, noncombat_policy,
                            battle_policy, agent_sims);
            for (int rep = 1; rep < repeat; ++rep)
            {
                const RunResult again = playRun(cfg, seed, ascension, verbose, skip_battles,
                                                noncombat_policy, battle_policy, agent_sims);
                if (again.digest != first.digest || again.outcome != first.outcome ||
                    again.floor != first.floor || again.battles != first.battles ||
                    again.net_decisions != first.net_decisions || again.noncombat != first.noncombat)
                {
                    fail("同一 seed 两次跑不一致（seed=" + std::to_string(seed) + "）：校验和 " +
                         std::to_string(first.digest) + " vs " + std::to_string(again.digest));
                }
            }
        }
        catch (const std::exception &e)
        {
            fail("seed=" + std::to_string(seed) + "：整局抛异常：" + e.what());
        }

        printResult(static_cast<std::size_t>(k), static_cast<std::size_t>(runs), seed, first,
                    battle_policy);
        if (repeat > 1)
        {
            std::fprintf(stderr, "  ✓ 同 seed 跑 %d 遍：校验和/场面逐位相同\n", repeat);
        }

        wins += first.outcome == GameOutcome::PLAYER_VICTORY ? 1 : 0;
        losses += first.outcome == GameOutcome::PLAYER_VICTORY ? 0 : 1;
        total_battles += first.battles;
        total_net += first.net_decisions;
        total_noncombat += first.noncombat;
        total_floors += first.floor;
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr,
                 "\n✅ %d 局全部走完：胜 %d / 负 %d，平均层数 %.1f；战斗 %lld 场、网络决策 %lld、"
                 "非战斗动作 %lld；%.1fs\n",
                 runs, wins, losses, static_cast<double>(total_floors) / runs,
                 static_cast<long long>(total_battles), static_cast<long long>(total_net),
                 static_cast<long long>(total_noncombat), seconds);
    g_watchdog_stop.store(true, std::memory_order_relaxed); // 收工：别再让看门狗把进程带走
    return 0;
}
