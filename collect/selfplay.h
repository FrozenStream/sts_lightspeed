//
// 自对弈热循环：**整段在 C++**（K 棵独立树 + 共享前向服务）—— 与 `teacher.cpp` 同形：
// 任务表（`encs.h`）→ 造局面（`scenebuild.h`）→ 搜索 → 落盘（`shardio.h`）全在 C++，
// Python 只给配方、要路径。导出面就是 `{ SelfplayConfig, selfplay, selfplayRun, registerSelfplay }`。
//
// 形状（为什么是这样，2026-10-12 重写）：
//   · **树内恒 1 行**（每轮下降 1 条路径）：STS 每帧合法动作 mean ≈ 4.7，树内批 k>1 会让降序
//     阶段的 Q 退化成同一个常数（`N` 只在回传时涨，而降序只看 `nv`）⇒ 那 k 条路径里只有第 1 条
//     真在搜索，剩下的是"按先验采样"。
//   · **一棵树一条线程**：树与树**不互相等**（旧版用"相位屏障"把每轮缝成流水线 ⇒ 前向延迟直接
//     进所有线程的周期）。⇒ 批宽 = 并肩的树数（`SelfplayConfig::games`）：树线程都阻塞在自己的
//     请求上、**不占 CPU** ⇒ 这个数只受内存（每棵树 ≈ 7.5 MB）约束，不受核数约束。
//   · **批 = 前向服务里当时攒到的请求**（`NNEval`：队列 + 服务线程，KataGo `NNEvaluator::
//     evaluate` 同形）。树上阻塞在自己的请求上 ⇒ 批宽由到达时序决定。
//     ⚠️ **代价**：批的组成不再只由树序决定 ⇒ K>1 **不再逐位可复现**（前向本来就与批宽相关：
//        GEMM 归约顺序 / kernel 选择）。**K=1** 批恒 1 行 ⇒ 仍与冻 golden 逐位对拍。
//   · **按场序边完成边交付**（`emit`）：主线程是唯一的写者/统计者，只处理**队头** ⇒ 不必等所有
//     场跑完，内存只由"队头落后的那几场"决定（旧的"整块 K 场跑完再落盘"把内存绑在 `games` 上）。
//
// 观测：直接调 `snapshotObs`（`collect.h`），**不再由宿主 TU 注入 `write_obs`** —— 那条注入是
// Python 驱动时代的遗产（当时观测写进哪块缓冲由 Python 侧决定）；现在两个采集器都直接调它。
//
// 探索（采样 / 根噪声）在这里做，值口径走 `z_spec`（`terminalValue`）⇒ 热循环**不碰 Python**。
// ⚠️ 随机数流是 `std::mt19937_64`，与旧 Python 路径的 `np.random.default_rng(seed*1000003+17)`
//    **不是同一条流** ⇒ 开着探索时两条路的行为序列不可逐位比（探索全关时才是同样的 argmax）。
//
#ifndef STS_LIGHTSPEED_SELFPLAY_H
#define STS_LIGHTSPEED_SELFPLAY_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "collect.h" // Frame / Term / GameOut / readTerm / snapshotObs（采集侧通用）

namespace sts::collect
{

    // 值口径 `z_spec` / `c_puct` / `q_init` 都由调用方算好（Python 侧 `zspec`）⇒ 这里只搬运。
    struct SelfplayConfig
    {
        std::string ts_path; // TorchScript 存档（必须在**目标设备**上 trace）
        std::string z_spec;  // 终局价值口径
        std::string device;  // 空/"cpu" = CPU
        int sims = 3000;      // 每个决策点的模拟数（= 轮数，树内恒 1 行）
        int max_steps = 1000; // 单局步数上限（与 `collect.MAX_STEPS` 同）
        int threads = 1;      // 前向的 intra-op 线程数（生产 = 1）
        int games = 8;        // 并肩几棵树（= 前向批宽，也是"一批"的场数）。只改墙钟与批组成
        double c_puct = 0.0, q_init = 0.0;
        bool sample = false;          // 按访问分布采样（默认 argmax）
        double eps = 0.0;             // 均匀探索概率
        int temp_moves = 0;           // 前多少个决策点按访问数采样（AGZ 的 τ=1 阶段）
        double dirichlet_total = 0.0; // 根噪声的**总浓度**（每动作 α = 总浓度 / 合法动作数）
        double dirichlet_eps = 0.0;   // 根噪声权重（0 = 关）
    };

    // 跑 K 场自对弈：`battles[j]` 的树与真实状态都归本函数独占（调用方给**拷贝**）；
    // 每棵树由**一条线程**跑到底，前向提交给内部的前向服务（见上）。
    // `seeds[j]` 只喂探索用的 RNG（`std::mt19937_64`），与局面无关（局面已由调用方造好）。
    //
    // `emit(j, GameOut&&)` 按**场序**回调（主线程是唯一的调用者）⇒ 调用方可以边完成边落盘/统计。
    // ⚠️ 回调里别碰 `battles` / `cfg` 之外的共享状态（它在主线程上，但此时别的树还在跑）。
    void selfplay(const std::vector<BattleContext> &battles,
                  const std::vector<std::uint64_t> &seeds,
                  const SelfplayConfig &cfg,
                  const std::function<void(std::size_t, GameOut &&)> &emit);

    // ── 整段采集入口（Python 只当**触发器**）─────────────────────────────────────
    //
    // 与 `teacherRun` 同形：任务表（`encJobs`）→ 造局面（`scenebuild`）→ 搜索 → 落盘（分片）
    // **全在 C++**。Python 只给配方（`cfg`）、seed 段与输出目录，拿回分片路径。
    //
    // `out_dir` 下产出若干 `.bin` 分片（`blk<i>.bin` 命名，**补零** ⇒ 字符串序 == 行序 == 任务序）；
    // ⚠️ 落盘**按场序**：同一场的行必须连续（读侧靠"终局列相同的连续行"认场 —— 落盘里没有任何
    //    场身份列，训练也不需要它们）。
    std::vector<std::string> selfplayRun(int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed,
                                         const std::string &out_dir, int shard_rows);

    void registerSelfplay(pybind11::module_ &m);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_SELFPLAY_H
