//
// 自对弈热循环：**整段在 C++**（K 棵独立树 + 共享前向服务）—— 与 `teacher.cpp` 同形：
// 任务表（`encs.h`）→ 造局面（`scenebuild.h`）→ 搜索 → 落盘（`shardio.h`）全在 C++，
// Python 只给配方、要路径。导出面就是 `{ SelfplayConfig, selfplay, selfplayRun, registerSelfplay }`。
//
// 形状（为什么是这样，2026-10-12 重写）：
//   · **树内恒 1 行**（每轮下降 1 条路径）：STS 每帧合法动作 mean ≈ 4.7，树内批 k>1 会让降序
//     阶段的 Q 退化成同一个常数（`N` 只在回传时涨，而降序只看 `nv`）⇒ 那 k 条路径里只有第 1 条
//     真在搜索，剩下的是"按先验采样"。
//   · **固定 W 条 worker 线程 + S 棵在飞的树**（`cfg.workers` 条线程、`cfg.pool` 棵并肩，与
//     `teacher.cpp` 的 worker 池同形）：树**不绑定线程**（每棵树是一台可挂起的状态机，推一步就回
//     池，谁抢到谁继续）⇒ **批宽 = 在飞的树数 S，线程只有 W 条**；碰上要前向就把请求塞进池、不占
//     线程，活交给前向服务线程。
//     ⚠️ 曾经是"一棵树一条线程"（K 可到 2000+）：那时每个批完成都要 `notify_all` 叫醒**全部 K
//        条**线程来抢同一把锁 ⇒ 实测 K=2016 时 CPU 15.7 核（16 核 cgroup）而 GPU 只有 4~11%，
//        几乎全是惊群与调度开销。**线程数（≤ 核数）与批宽（要宽）是两件事**，别用"树数 = 线程数"
//        把两者捆在一起。
//     ⚠️ 也别把树**静态分给线程**（每线程持 k 棵、攒够 k 行再一起等）：那让 worker 卡在自己那 k 棵
//        上（尾块只能干等），而这里的 ready 队列是**抢占式**的，长短局自然均衡。
//     ⇒ 在飞的树数（`cfg.pool`）只受内存（每棵 ≈ 3~7.5 MB）约束；`workers` 只影响"谁在跑"。
//   · **任务表滑窗**（`selfplayPooled`）：跑完一场**立刻**补一场（**树槽** `S` 与**交付槽** `D` 分开：
//     跑完就腾树槽，结果按任务号落交付槽、等队头）⇒ 在飞的场数稳定在 `cfg.pool`。
//     ⚠️ **批宽 = 在飞的场数**（一棵树同时只挂 1 个前向请求）：一次前向的固定开销（~200 次算子派发、
//        实测 ~1.5 ms/调用）按行摊 ⇒ 在飞场数太少（几十场）会让批宽掉到十几行、每行 112 µs（实测
//        543 s vs 202 s）。**但 B≈80 就过了拐点**：再宽只剩 ~1.4× 余量（按行 ~25 µs 里 18 µs 与批宽
//        无关，GPU 只忙 16%）⇒ 想更快得让"一次前向"本身变便宜，不是继续加宽。
//     ⚠️ 两个坑（都会把在飞场数塌到 1/3）：① 树槽与交付槽不能合成一个环（跑完没交付的场会占着树槽）；
//        ② 交付落后量 `D` 要留够（`D = n_task`）—— 顶到上限就停开新场。
//   · **批 = 前向服务里当时攒到的请求**（`NNEval`：队列 + 服务线程，KataGo `NNEvaluator::
//     evaluate` 同形）。树上阻塞在自己的请求上 ⇒ 批宽由到达时序决定（`batch_wait_us` 可以强制攒批，
//     但只在池子塌了的时候才划算：池子正常时它是白等，实测 235 s vs 202 s）。
//     ⚠️ **代价**：批的组成不再只由树序决定 ⇒ K>1 **不再逐位可复现**（前向本来就与批宽相关：
//        GEMM 归约顺序 / kernel 选择）。**K=1** 批恒 1 行 ⇒ 仍与冻 golden 逐位对拍。
//   · **按场序边完成边交付**（`emit`）：主线程是唯一的写者/统计者，只处理**队头** ⇒ 不必等所有
//     场跑完（交付落后只是内存：一场的帧 ≈ 70 KB）。
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



#include "collect.h" // Frame / Term / GameOut / readTerm / snapshotObs（采集侧通用）

namespace sts::collect
{

    // 值口径 `z_spec` / `c_puct` / `q_init` 都由调用方算好（Python 侧 `zspec`）⇒ 这里只搬运。
    struct SelfplayConfig
    {
        std::string model_path; // 网络存档（ONNX Runtime 认 `.onnx`，见 `bindings/onnx_eval.h`）
        std::string z_spec;  // 终局价值口径
        std::string device;  // 空/"cpu" = CPU
        int sims = 3000;      // 每个决策点的模拟数（= 轮数，树内恒 1 行）
        // **随机化预算**（KataGo 的 randomized playout cap，arXiv:1902.10565）：多数决策点跑
        // `sims_cheap`、少数（比例 `full_frac`）跑 `sims`。理由：全表恒 3000 次 ⇒ 标签量大但几乎
        // 全是"低预算的形状"；少量高预算决策点提供更深的标签，整体在**同墙钟**下覆盖更多局面。
        // ⚠️ `sims_cheap <= 0` = 关：恒用 `sims`，且**不消耗** rng 流 ⇒ 旧行为逐位不变。
        int sims_cheap = 0;
        double full_frac = 0.25; // 走完整预算（`sims`）的决策点比例（仅 `sims_cheap > 0` 时生效）
        int max_steps = 1000; // 单局步数上限（与 `collect.MAX_STEPS` 同）
        int threads = 1;      // 前向的 intra-op 线程数（生产 = 1）
        int pool = 512;       // **池宽** = 在飞的场数（一棵树同时只挂 1 个前向请求 ⇒ 这也是批宽的上限）
        int workers = 0;      // worker 线程数（0 = 硬件并发数，上限 = `pool`）；树**不绑定线程**，跑一步就回池
        // **批宽上限**：一次前向最多吃 `batch_target` 行（0 = 不设限 = 全拿走）。
        // ⚠️ 实测**无差**：分母是"在飞场数"（池宽），与取批策略无关 ⇒ 设上限只会**变窄**（池子比上限宽时
        //    它就成了新瓶颈）⇒ 默认 0，别设。
        int batch_target = 0;
        // **攒批**（µs；0 = 不等，来一批发一批）：队列非空后再等，直到"这一轮该来的都来了"或到这个
        // 上限（`batch_target` 攒够也提前发车）。
        // ⚠️ **只在池子塌了的时候才该开**：那时批宽自锁在十几行、固定开销摊不薄，等一等能把批拉到 60+
        //    （墙钟 543 → 316 s）。池子正常时它是**白等**：实测池宽 512 等 6 ms 反而 235 s（服务线程只剩
        //    78% 忙）vs 不等 **202 s** ⇒ 默认 0。
        int batch_wait_us = 0;
        double c_puct = 0.0, q_init = 0.0;
        bool sample = false;          // 按访问分布采样（默认 argmax）
        double eps = 0.0;             // 均匀探索概率
        int temp_moves = 0;           // 前多少个决策点按访问数采样（AGZ 的 τ=1 阶段）
        double dirichlet_total = 0.0; // 根噪声的**总浓度**（每动作 α = 总浓度 / 合法动作数）
        double dirichlet_eps = 0.0;   // 根噪声权重（0 = 关）。⚠️ **每个新根**都重混一次：不只
                                      //    新建树根，子树提升为新根时也混（见 `mixRootNoise`）

        // ── **尾段（窄批）走 CPU**：`model_path_cpu` 空 = 关 ────────────────────────────────
        // 为什么：一次前向的耗时**基本与批宽无关**（实测 GPU：批 1 → 0.98 ms、批 256 → 1.41 ms）
        // ⇒ 它是 **kernel 派发受限**（每次 ~200 个算子都要重新派发），不是算力受限。于是
        // "只剩几棵树在飞"的尾段（批 1~几行）每行比宽批贵 ~200×：实测一整轮 2411 s 里有 **1877 s**
        // 花在最后 16 场的 ~160 万次 1~2 行调用上（GPU 只 40%、14 条 worker 87% 在睡）。
        // CPU 侧正好相反：批 1 每次 **0.40 ms**（GPU 1.13 ms）、且**多会话能横向扩**
        // （1 会话 2201 次/s、4 会话 7164、8 会话 10083；而 CUDA 加会话不 scale —— GPU 侧 launch 串行）。
        // ⇒ 待服行数 ≤ `cpu_rows` 时交给 CPU 会话（每个会话一次只吃 **1 行**：CPU 成本对行数近似线性，
        //   拆开不亏，还能让多个会话并行）；超过则仍走主设备（宽批在 GPU 上每行只要几 µs）。
        // 分界来自实测：CPU 4 会话 ≈ 9.9k 行/s，GPU 到批 ~10 行才追平。
        // ⚠️ 关着时（空路径）**逐位不变**：`apps/selfplay-test` 就是这么跑的 ⇒ 在役守卫不受影响。
        // ⚠️ 开着时**尾段那几行的数值实现换了**（CPU 会话与 GPU 会话的 ONNX 差 ~1e-6 级，同"换后端"
        //   那一类）⇒ 换开关 = 换数据谱系（配方里记着 `cpu_rows` / `cpu_sessions`）。
        std::string model_path_cpu; // 尾段用的 CPU 存档（`.onnx`；空 = 关）
        int cpu_sessions = 4;       // CPU 侧服务线程数（各持一个独立会话 ⇒ 内部缓冲不共享）
        int cpu_rows = 10;          // 待服行数 ≤ 它 ⇒ 走 CPU；0 = 关
    };

    // 一场的输入（池化路径用**回调**喂 ⇒ 任务表可以是流式的，不必先造一批局面）
    struct TaskIn
    {
        BattleContext battle;
        std::uint64_t seed = 0;
    };

    // ── 池化自对弈：任务表 + **滑窗** ──────────────────────────────────────────────
    // 共 `n_task` 个任务、池里**始终**最多 `pool` 场在飞；一场跑完（交付掉）立刻从任务表补一场。
    // ⚠️ 这是吞吐的命门：一棵树同时只能挂 1 个在飞前向请求 ⇒ **批宽 ≤ 在飞的场数**。而"在飞场数"在
    //    整表一次性全开时会**单调衰减**（短局几秒跑完就走）⇒ 批宽最后锁在几十行，每次前向那 ~200 次
    //    算子派发的固定开销摊不薄（实测均批 84 行 ⇒ 20.6k 行/s、CPU 5.7/16 核、GPU 21%）。
    // `make_task(j)` **由主线程调**（任务 j 落在环槽 `j % pool`：`j` 与 `j+pool` 不会同时在飞）⇒
    // 造局面（`buildStaged`）与搜索重叠，池宽与线程数彼此独立。
    // `emit(j, GameOut&&)` 按**任务序**回调（主线程是唯一调用者）：交付序 = 任务序 = 数据行序。
    void selfplayPooled(std::size_t n_task, std::size_t pool,
                        const std::function<TaskIn(std::size_t)> &make_task, const SelfplayConfig &cfg,
                        const std::function<void(std::size_t, GameOut &&)> &emit);

    // 跑 K 场自对弈（**整表一次全开**：池宽 = 任务数，不补场）：`battles[j]` 的树与真实状态都归本函数
    // 独占（调用方给**拷贝**）；这 K 棵树由 `cfg.workers` 条线程分担（树**不绑定线程**：推一步就回池，
    // 谁抢到谁继续），前向提交给内部的前向服务（见上）。
    // `seeds[j]` 只喂探索用的 RNG（`std::mt19937_64`），与局面无关（局面已由调用方造好）。
    // 测试/逐位对拍走这条（K=1 时批恒 1 行 ⇒ 与 golden 可逐位对拍）。
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



} // namespace sts::collect

#endif // STS_LIGHTSPEED_SELFPLAY_H
