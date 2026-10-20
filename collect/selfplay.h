#ifndef STS_LIGHTSPEED_SELFPLAY_H
#define STS_LIGHTSPEED_SELFPLAY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "collect.h" // Frame / Term / GameOut / readTerm / snapshotObs（采集侧通用）

namespace sts::collect
{
    // 值口径 `z_spec` / `c_puct` / `q_init` 都由调用方算好（Python 侧 `zspec`）⇒ 这里只搬运。
    struct SelfplayConfig
    {
        std::string model_path;     // 网络存档（ONNX Runtime 认 `.onnx`，见 `bindings/onnx_eval.h`）
        std::string model_path_cpu; // CPU 用的存档（`.onnx`；空 = tail_mode 用主存档 + CPU）

        std::string z_spec; // 终局价值口径
        std::string device; // 空/"cpu" = CPU

        int sims = 4096; // 每个决策点的模拟数
        double sims_cheap_frac = 0.125;
        double full_frac = 0.25; // 走完整预算（`sims`）的决策点比例（仅 `sims_cheap_frac > 0` 时生效）

        int pool = 1024; // **池宽** = 在飞的场数（一棵树同时只挂 1 个前向请求 ⇒ 这也是批宽的上限）
        int workers = 0; // worker 线程数（0 = 硬件并发数，上限 = `pool`）；树**不绑定线程**，跑一步就回池

        int max_steps = 1000;       // 单局步数上限（与 `collect.MAX_STEPS` 同）
        int long_decay_start = 200; // 长局衰减起点

        double c_puct = 0.0, q_init = 0.0;
        bool sample = false;          // 按访问分布采样（默认 argmax）
        double eps = 0.1;             // 均匀探索概率
        int temp_moves = 0;           // 前多少个决策点按访问数采样（AGZ 的 τ=1 阶段）
        double dirichlet_total = 0.0; // 根噪声的**总浓度**（每动作 α = 总浓度 / 合法动作数）
        double dirichlet_eps = 0.0;   // 根噪声权重（0 = 关）。⚠️ **每个新根**都重混一次：不只新建树根，子树提升为新根时也混（见 `mixRootNoise`）
    };

    // 一场的输入（调度单元）= **一个造好的局面**。调度器只认识这个值；**造局时机由 TaskFactory 决定**：
    // `selfplayRun` 在领到任务号时才 `buildBattle`，`selfplay` 则用调用方已有的 battles。
    // ⚠️ 不要在 `launchGame` 里硬编码 `buildBattle`：那会把调度器和采集任务表绑死。
    // ⚠️ 探索用的 RNG（`Tree::rng`）由 `battle.seed` 播种，**没有第二个"探索 seed"**：
    //    原先的 `TaskIn{battle, seed}` 在所有调用点 `seed` 都恒等于 `battle.seed`（`BattleContext::init`
    //    就是 `seed = gc.seed`）⇒ 那个字段是纯冗余，2026-10-18 删掉、`TaskIn` 一起撤销。
    //    由此"同一局面 + 同一网络 ⇒ 同一条轨迹"是**结构上**成立的，不靠调用方自觉对齐两个 seed。

    // 任务源 / 结果槽的调用面：`make_task` 可能被多个 worker 并发调用；
    // `emit` 只在主线程、按任务序调用。别名只做签名收敛，不改变线程契约。
    using TaskFactory = std::function<BattleContext(std::size_t)>;
    using GameSink = std::function<void(std::size_t, GameOut &&)>;

    // ── 池化自对弈：任务表 + **滑窗** ──────────────────────────────────────────────
    // 共 `n_task` 个任务，最多 `pool` 场在飞。完成后释放树槽，并在交付窗口允许时补场。
    // 树槽与交付环分离：交付环容量 D = min(n_task, 8 × 池宽)，交付后重试因环满暂停的补场。
    // ⚠️ 这是吞吐的命门：一棵树同时只能挂 1 个在飞前向请求 ⇒ **批宽 ≤ 在飞的场数**。而"在飞场数"在
    //    整表一次性全开时会**单调衰减**（短局几秒跑完就走）⇒ 批宽最后锁在几十行，每次前向那 ~200 次
    //    算子派发的固定开销摊不薄（实测均批 84 行 ⇒ 20.6k 行/s、CPU 5.7/16 核、GPU 21%）。
    // `make_task(j)` 每个任务调用一次，可能在主线程或不同 worker 上并发执行。
    // 回调须支持并发；树槽仅由持有它的线程访问，不按任务号固定分配。
    // `emit(j, GameOut&&)` 主线程唯一调用，按完成序；`GameOut.task` 是任务号，调用方按需回填。
    void selfplayPooled(std::size_t n_task, std::size_t pool, const TaskFactory &make_task, const SelfplayConfig &cfg, const GameSink &emit);

    // 跑 K 场自对弈（**整表一次全开**：池宽 = 任务数，不补场）：`battles[j]` 的树与真实状态都归本函数
    // 独占（调用方给**拷贝**）；这 K 棵树由 `cfg.workers` 条线程分担（树**不绑定线程**：推一步就回池，
    // 谁抢到谁继续），前向提交给内部的前向服务（见上）。
    // 探索 RNG 由各场的 `battle.seed` 播种（见 `TaskFactory` 那段：没有独立的探索 seed）。
    // 测试/逐位对拍走这条（K=1 时批恒 1 行 ⇒ 与 golden 可逐位对拍）。
    // ⚠️ 回调里别碰 `battles` / `cfg` 之外的共享状态（它在主线程上，但此时别的树还在跑）。
    void selfplay(const std::vector<BattleContext> &battles,
                  const SelfplayConfig &cfg,
                  const GameSink &emit);

    // ── 整段采集入口（Python 只当**触发器**）─────────────────────────────────────
    //
    // 与 `teacherRun` 同形：任务表（`encJobs`）→ 造局面（`scenebuild`）→ 搜索 → 落盘（分片）
    // **全在 C++**。Python 只给配方（`cfg`）、seed 段与输出目录，拿回分片路径。
    //
    // `out_dir` 下产出若干 `.bin` 分片（`blk<i>.bin` 命名，**补零** ⇒ 字符串序 == 行序 == 任务序）；
    // ⚠️ 落盘**按场序**：同一场的行必须连续（读侧靠"终局列相同的连续行"认场 —— 落盘里没有任何
    //    场身份列，训练也不需要它们）。
    std::vector<std::string> selfplayRun(int n_per, const SelfplayConfig &cfg, std::uint64_t start_seed,
                                         const std::string &out_dir, int shard_rows,
                                         bool hard_only = false);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_SELFPLAY_H
