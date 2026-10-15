# 项目笔记（唯一在役副本）

> **2026-10-14 合并精简**：由旧 `notes.md`（477 KB）+ `DIARY.md`（521 KB）两份合并而来，原文都在
> git 历史里（`git show <该提交>:.agent/DIARY.md`）。这里只留**仍然成立**的事实，且每条都对着
> 当天的代码核过；刻意删掉的：流水账、已删代码的设计史、已修 bug 的排查过程。
>
> ⚠️ 本文件是**追加式文本、感知不到用户撤回**：任何「现在是 X」「Y 已删」型断言，据此动手前先跑
> 命令核实现场（`Test-Path` / `Select-String` 一下，几秒钟的事）。

## 1. 在役结构（2026-10-14 核实）

C++ 引擎 + Python 训练侧 + pybind11 绑定。`目录名 == 命名空间名`：`collect/` 一律 `sts::collect`，
`bindings/`+`apps/` 留 `sts::py`；跨目录全限定或显式 `using`，**不建别名**。

| 层 | 在役文件 | 说明 |
|---|---|---|
| 热循环（全 C++） | `collect/selfplay.{h,cpp}` | 自对弈：**W 条 worker 线程 + K 棵在飞的树**（ready 队列，树**不绑线程**）+ 前向服务 `NNEval` |
| | `collect/teacher.cpp` | 教师采集 `runTeacherAll`：W 条 worker **领号**（`j = started++`，在飞满 `S = 2W` 就等）+ 出口按场序排空 |
| | `collect/eval.{h,cpp}` | 配对评估；`net:<path>`（不给 `:sims`）= 裸网络走 `playNetBatch` |
| | `collect/collect.h` | `Frame/Term/GameOut` + `readTerm`/`snapshotObs`（两个采集器共用，谁都不 include 对方） |
| | `collect/{encs,scenebuild,snapshot,action}.{h,cpp}` | 任务表 / 造局面 / 观测快照 / 动作层 |
| 容器 | `collect/shardio.h` | **分片容器（唯一容器）** v2，头顶 24 字节 |
| 绑定 | `bindings/slaythespire.cpp` | `snapshot_layout()` / `action_layout()`（**布局与动作宽度的唯一来源**） |
| | `bindings/torch_eval.{h,cpp}` | `TorchEvaluator`：TorchScript 前向，支持 device/cuda，内部记 H2D/前向/D2H 分段耗时 |
| 触发/训练（Python） | `collect_selfplay.py` / `collect_teacher.py` / `eval_gate.py` | **薄触发器**：拼产物名 + 装 `*Config` + 打一行读数 |
| | `train.py` / `aznet.py`（网络定义唯一一份）/ `zspec.py`（值口径+搜索旋钮）/ `collect.py`（读侧 `CollectData`）/ `collect_io.py`（字节层） | |
| | `tsnet.py` / `ckpt.py` / `metrics.py` / `runtime.py` / `scene.py` | trace 缓存 / 权重名与内容指纹 / 读数 / 命名+配额 / 场景转发 |
| 循环与运维 | `_chain.sh`（自迭代，**无参数**，旋钮是裸常量）；`sync.sh` + `_push/_start/_kill/_status/_log/_wait/_check/_attach.sh` | `sync.sh` 已重构为**薄分发器**，实现别往里加 |
| 守卫 | `apps/{engine,scene,selfplay}-test.cpp` | Python 侧 `_chk_*.py` **已全部删除**，守卫都在 C++ |

- 根目录 `*.py` 全表（17）：`azaction azlayout aznet check_card_data ckpt collect collect_io
  collect_selfplay collect_teacher eval_gate metrics runtime scene train tsnet watch zspec`。
- **已死的名字，别再用**：`tboard.py` `walk.py` `rlenv.py` `eval_walk.py` `_chk_*.py` `azsearch.py`
  `devshape.py` `_mix_caches.py`、`chain.sh`（全仓零引用）、Python 侧 `MCTSNode` / `teacher_action` /
  `DEFAULT_EXPLORATION_PARAMETER`。教师搜索只有 C++ 一份，旋钮是 `TeacherConfig::exploration_parameter`
  （0 = 引擎默认 3√2，见 `collect/teacher.h`）。
- 改名对照（2026-10-01）：`_az_train.py`→`train.py`、`_env.py`→`runtime.py`、`_zspec.py`→`zspec.py`、
  `_ckpt.py`→`ckpt.py`、`_tb.py`→（无，`tboard` 已死）、`_sync.sh`→`sync.sh`；评估入口现名 **`eval_gate.py`**。

## 2. 契约 / 不变量

1. **观测布局唯一来源 = `bindings/slaythespire.cpp` 的 `Snap`**（`snapshotOf()` 与常量同住一个编译单元）。
   Python 侧 `azlayout.LAYOUT = sts.snapshot_layout()`，**别硬编码偏移**。当前 `len = 1603`，其中
   `counts_len 7`（monsterCount/monstersAlive/手牌/抽牌堆/弃牌堆/消耗堆/deckSize）、`hand_slots 10`、
   `pile_slots 128`、`deck_slots 96`、`monster_slots 5` × `monster_fields 9`、`card_select_slots 128`
   (`card_select_task_types 21`)、`potion_slots 5`、`relic_len 181`。改布局 = 作废全部缓存。
2. **观测 ≠ 网络输入**：网络输入在 `aznet._inputs` 另建（`in_dim` 由 `net_cfg()` 与布局现推），
   五个嵌入宽度（`CARD_RANK 32`/`MONSTER_RANK 16`/`INTENT_MOVE_RANK 16`/`RELIC_RANK 16`/`POTION_RANK 16`）
   **直接决定 `in_dim` ⇒ 改值 = 作废旧权重**；`hidden`/`depth`/`dropout` 只改容量（不进权重名，见 §5）。
3. **动作编码**：`POLICY_DIM = sts.action_layout()["count"] = 96`（手牌 10×stride 6 → `potion_base 60`、
   药水 5×stride 7、`end_turn 95`）；`SELECT_DIM = 128`。任何合法动作必须能编进**唯一**统一下标，
   编不进 = `RuntimeError("动作无法编码进统一下标…")`。
   ⚠️ `MULTI_CARD_SELECT` **编不进统一下标** ⇒ 触发它的药水/遗物从场景发放池排除：
   `BANNED_POTIONS = {ELIXIR_POTION, GAMBLERS_BREW, ENTROPIC_BREW, COLORLESS_POTION}`（铁甲因此少 2 药水）、
   `BANNED_RELICS[5]` 含 `GAMBLING_CHIP`（少 1 遗物）。真修法 = 候选 multi-hot + `confirm` 动作，未做。
4. **分片容器 v2**（`collect/shardio.h`，与 `collect_io.py` 同源）：头 24 = `magic(8)+version(4)=2+ncols(4)+nrows(8)`；
   列目录每项 36 = `name(16)+dtype(4)+width(8)+offset(8)`；列清单顺序 = 文件物理顺序；**场身份
   （seed/enc/floor）不落盘**（训练不看，段号由终局列现推）。`obs_dim`/`space` 由列宽派生。
   ⚠️ 旧格式（v1）被**响亮拒绝**，不是静默兼容。
5. **按场序交付**：同一场的行必须连续（读侧靠「终局列逐行相同的连续段 = 一场」认场）；写侧
   （`selfplayRun`/`teacherRun`）出口按场序排空 ⇒ 行序 == 场序，产物逐字节可复现（`threads=1/4/8` sha256 同）。
6. **值口径与搜索旋钮同尺度**（`zspec.py`）：口径只三种 —— `hp`（**默认**：胜=剩余血比例、败=−1）/
   `hp_turn` / `enginenorm`（`t_escore/(100*max_hp)`）。`Z_SCALE = {enginenorm 0.420, hp 0.618, hp_turn 0.618}`、
   `Z_MID = {enginenorm 0.786, hp 0.0, hp_turn 0.0}`；`C_PUCT_BASE = 1.5`（基准尺度 `C_PUCT_REF = 0.618` ⇒
   `c_puct_for(spec)`：`hp` 1.5、`enginenorm` 1.019）、`q_init_for(spec)` 取区间中点、`DIRICHLET_TOTAL = 10.83`
   （KataGo 口径的总浓度，每动作 α = 总浓度/合法动作数）。**换口径必须同时换这三者**，否则等于偷偷改探索强度。
   `z` 只依赖终局列 ⇒ 换口径**不必重采**，但历史 `val_ce`/胜率**不可比**。
7. **种子纪律**：`collect.COLLECT_SEED0 = 0`（自对弈/教师采集）、`collect.EVAL_SEED0 = 100_000_000`
   （评估，held-out；工具拒绝更低的 seed0）。对照必须**同 seed 配对**；换种子段 ⇒ 绝对胜率不可比。
   `MAX_STEPS = 1000`（单局步数上限，与 `SelfplayConfig::max_steps` 同）。
8. **设备与 trace 必须同设备**（`tsnet.py` + `bindings/torch_eval.cpp`）：图里的 buffer 常量**不随
   `module.to()` 搬** ⇒ 存档必须按设备各 trace 一份（文件名带 `-cpu`/`-cuda`）。trace 前必须
   `torch._C._jit_set_profiling_executor(False)`，C++ `TorchEvaluator` 构造里同落这一句（否则数值对不上）。
   存档按内容指纹 + 设备命名、只读共享、原子落盘（`.tmp`→`replace`）。
9. **前向必须 float32**（C++ 侧用 double 会与 Python 差 1e-8 级 ⇒ 十几步后轨迹分叉）；`NNEval` 的
   **第一次前向必须在主线程跑过**再起服务线程（头宽度/CUDA 上下文/执行计划那时才建立）。
10. **复现性边界**：只有 **K=1**（单树、批恒 1 行）能与冻 golden **逐位对拍**；K>1 的批组成由到达时序
    决定、而前向与批宽相关 ⇒ **不可能逐位复现**，性质由「同参数两次跑一致」这类统计断言守
    （`apps/selfplay-test.cpp` 的 K=1 / `--sweep`）。CUDA 核非确定 ⇒ cuda 路径上连 K=1 的逐位复现也没有。
11. **训练线程数是配方**：换线程数 = 换轨迹（1 vs 8 线程 `val_ce` 差 0.01~0.02、权重最大差 0.6~0.9）；
    **同线程数跨进程/跨 torch 版本逐位可复现**（2.8.0+cu128 ↔ 2.12.1+cu130 逐位同）。差异随训练步数放大。
12. **RNG 不是同一条流**：`std::mt19937_64`（C++ 搜索）vs `np.random.default_rng`（旧 Python 路径）
    ⇒ 开探索（π 采样/eps/温度/根噪声）时两条路的行为序列**不可逐位比**；探索全关时都是 argmax 才可比。
13. **`edges` 与枚举序**：`root.edges` 插入序 == `enumerateActions` 枚举序；并列取「枚举序第一个」，
    而 `np.argmax` 取「下标序第一个」⇒ 低 sims 下会分叉（实测 16% 帧）。
14. **写缓冲上限**：`shard_rows = 200000` 行/分片 ≈ **1.5 GB 写缓冲**（`ShardWriter` 攒够才 flush）——
    当前内存第二大头，未改。
15. **构建**：本地用 VS 的 cmake（`C:\Program Files\Microsoft Visual Studio\18\Community\...\CMake\bin\cmake.exe --build build --config Release`）；
    跑 `build/Release/*-test.exe` 需要把 `.venv\Lib\site-packages\torch\lib` 和
    `C:\Users\<user>\AppData\Local\Python\pythoncore-3.14-64` 放进 `PATH`（否则 `0xC0000135`）。
    ⚠️ `include/sts_common.h` **无条件** `#define sts_asserts`（自检默认开），但块体里用的是 C 的
    `assert` ⇒ `-DNDEBUG`（Release）下**被预处理掉、静默失效**（`include/combat/ActionQueue.h:72` 那种写法）
    ⇒ 断言类守卫要用 Debug 跑；别把 `sts_asserts` 删掉当"修好"。

## 3. 旋钮与实测数字（为什么是这个值）

### `_chain.sh`（自迭代循环，2026-10-14 在役值）
`N_PER=32`（每项场数，总场数 = ENCS 表项数 63 × N_PER）、`SIMS=3000`、`GAMES=2016`（**在飞的树数 = 前向批宽，
不是线程数**；2016 = 一轮整场数 ⇒ 整轮一个块）、`SP_WORKERS=14`（worker 线程数，取核数−2）、
`ROLLOUT_THREADS=1`（前向 intra-op 线程；cuda 下只跟树线程抢 CPU）、`DEVICE=cuda`、`EPOCHS=2`、`LR=3e-4`、
`WORKERS=32`（训练线程）、`LOSS_CHUNK=16384`、`PI_WEIGHT=advantage`、`SOFT_TARGET=1`/`SOFT_TEMP=1.0`、
`TEMP_MOVES=30`、`DIR_TOTAL=10.83`、`DIR_EPS=0.1`、`GATE_N_PER=512`、`GATE_MARGIN=0.01`、
`GATE_SEED0=100000000`（=`EVAL_SEED0`）、`NET=data/warm.pt`、`ROUNDS=0`。

- **并肩/线程形状**（实测探针，sims=200，同批 63 项 × n_per，远端 4080S + 16 核 cgroup）：
  `cuda K=2016 W=14`（每线程 144 棵并肩）**0.122 s/局** < 旧「一棵树一条线程」`W=2016` 0.145 <
  `cuda K=504 W=504` 0.379；`cpu(12 intra-op) K=504` 0.206 反而最快 ⇒ **窄批 CPU 赢、宽批 cuda 赢**
  （CPU 前向的 intra-op 线程要和树线程抢同一个 16 核 cgroup）。按 3000/200 线性外推 ⇒ 一轮 2016 场 ≈60~70 min。
- **内存**：≈7.5 MB/棵 ⇒ `GAMES=2016` ≈ 15 GB（本机 503 GB 无压力）。
- **别用「一棵树一条线程」**：实测 K=2016 那形状 CPU 12.5~15.7 核（16 核吃满）而 GPU 只有 4~11%，
  几乎全是 `notify_all` 惊群 + 调度开销；换成池之后 NLWP 1510→34、CPU 7 核、GPU 32%。
- **采集必须单进程**（多份 CUDA 上下文踩断过容器）；`NNEval` 只起 1 个服务线程。
- **`loss_chunk`**：默认 65536，`hidden=512` 下单块索要 16 GiB 当场 OOM（因子策略头中间张量
  `[chunk, 手牌槽, 目标槽, hidden]` 随 hidden 线性放大）⇒ 链条里给 16384。
- **`PI_WEIGHT`**：`none` 每行等权 / `advantage = max(0, z−V̂)` 归一。等权时「在输局里也照抄教师」是负作用
  （实测两份数据各 −0.08 / −0.30pp）⇒ 生产用 `advantage`。
- **`GATE_N_PER` 下限由裕度反推**：两臂各有噪声，32 场/项（N≈1024）≈ ±2pp > `GATE_MARGIN` ⇒ 判据落进噪声、
  接受/拒绝近乎掷硬币。
- **`DIR_EPS`**：围棋给 0.25，本引擎每帧合法动作 mean ≈4.7 ⇒ 调到 0.1（`DIR_TOTAL` 是**总浓度**，
  别按「每动作 α」读）。

### 网络与训练默认（`aznet.net_cfg()` / `train()` 签名，实跑打印）
`net_cfg() = {hidden 256, depth 4, arch factor, sel_arch factor, dropout 0.1, card_rank 32, monster_rank 16,
intent_move_rank 16, relic_rank 16, potion_rank 16, init_std 0.02}`。
`train()` 默认：`lr 1e-3, wd 1e-4, epochs 64, batch 256, z_spec hp, soft_target False, soft_temp 1.0,
pi_weight none, pi_objective off, loss_chunk 65536, threads 1, device cpu, record True`。默认值**只在签名一份**。

- 建网超参（`hidden/depth/arch/dropout`）**不进权重名**（`ckpt_params` 只收 `train()` 签名里的 keyword-only），
  但 `--init` 会 `check_init` 校验建网配置一致 ⇒ 不会静默拿错网续训。
- 容量 vs 搜索是**替代品**：`hidden` 128→256 裸网 +2.1pp、MCTS(20) 只 +0.5pp、val_R² 平；`v2`（512/4）在
  22176 场上**不值**（输给 v1 起点 1068:1247）⇒ 现役回到 `256/4` + 加宽嵌入（`CARD_RANK 8→32` 等）。
- 步速随数据集**塌缩 2.7×**（batch 256：34.7k 样本 23.8 步/s → 697k 样本 8.8 步/s，gather 大数组页表崩）
  ⇒ **别拿小数据集外推**。
- 跨机器绝对耗时**不可比**（本地单核比远端 Xeon 快 2.5~2.8×；gcc -O3+LTO vs MSVC 浮点扰动 ⇒ 教师胜率 46.2%→44.8%）。

### 前向开销（旧口径测量，用于判断"下沉到哪"）
远端单线程 MCTS(20)：零前向地板 `NullNet` = **116 µs/模拟**（clone/execute/枚举 + 树操作，占最优配置 59%）；
`torch.compile` b=8 = 196.9 µs/模拟（4.66×）；trunk+3 头纯 GEMM 只 11.5 GFLOP/s ⇒ **瓶颈是 ~200 次算子派发，不是浮点**。
⇒「同宽同网把前向搬进 C++ 进程内」上限只有 1.33~1.70×：**下沉的收益来自形状**（不跨进程 + 独立线程 +
把批喂满），不是"C++ 更快"。cuda 侧另有每次调用固定开销（H2D→kernel→D2H 串行，实测 ≈1.45 ms/次）。

### 教师（预算 ↔ 胜率/耗时，held-out 1000 场，远端）
500 sims 0.07 s/47.0%、2000 0.24 s/54.0%、8000 0.89 s/64.5%、16000 1.82 s/67.5%、32000 3.48 s/69.0%。
⚠️ 教师胜率**随预算继续涨**（"Boss/Act4 是标签天花板"已修正）；比较强弱必须**同时**限定种子段与教师预算。

## 4. 评估口径与统计

1. **门控 `eval_gate.py`**：配对评估 —— 两个策略吃**同一批 `(遭遇, 层, seed)`** ⇒ 胜率差就是策略差。
   判据只能是「候选 vs 当前 best 的**胜率差** ≥ `GATE_MARGIN`」，**不能拿绝对胜率比固定阈值**（那个数常年 0.9+，
   什么网都过）。固定 seed 段（`EVAL_SEED0`）⇒ 曲线只能看配对差，不能跳轮比绝对胜率。
2. **规格字符串**：`net:<path>` = 裸网络（逐决策点取先验 argmax，无搜索）；`net:<path>:<sims>`（`sims ≥ 2`）
   才走 MCTS。`sims == 0` 不进 `selfplay`（1 次模拟只展开根 ⇒ π 全零）。
3. **统计纪律**：胜率类结论 `n ≥ 200` 起步 + 同场景配对 + McNemar；优先找连续量（血、z）而非二值量；
   均值比较分辨率下限 ≈ 2pp。
4. **`val_ce`/`val_R²` 是误导性指标**：`val_R²` 在 seed 间 +0.272↔+0.613 而胜率只差 2pp；「拟合更好」≠「搜索更强」（5 次复现）。
5. **教师先行纪律**：教师自己只有 0~20% 的档（Boss/Act4）**不能**用来判网差（标签=必输轨迹）；缺口在非 Boss 档。
6. **DAgger 口径**：问"学生在教师状态上的 Top-1"（实测 46.5%），别用"各自 argmax 走轨迹后的 agreement"（盘面早分叉，9.9%/14.5% 是测量错）。

## 5. 明确被推翻 / 已废弃（一句话 + 现在正确的）

1. **「树内 batch MCTS」是错路**：STS 每帧合法动作 mean 4.72，`MCTS_BATCH=256` 是其 54 倍 ⇒ 从第 ~6 条路径起
   退化成纯先验采样、π 被摊平（最大分量 0.58~0.62）。**现在是**：树内恒 1 条路径，批宽来自「并肩多棵树」。
2. **「一棵树一条线程」已废**：惊群把 16 核吃满而 GPU 空转。**现在是**：W 条 worker 线程 + K 棵在飞，
   ready 队列**不绑线程**（`push` 不阻塞，服务线程跑完一批把 owner 回投）。
3. **「C++ 进程内推理更快」不成立**（同宽同网 1.00~1.20×，大批还慢 20%）⇒ 见 §3；也**不是**"慢 80×"（那是测量错误）。
4. **「软目标更差」有条件翻案**：自对弈 MCTS(500) 的 π 更尖（熵 0.99 vs 教师 1.17），软目标 T=1 胜率持平、
   战损每档更好。引用必须带 π 来源。
5. **「engine 值目标更差」= 尺度混淆**（`q=W/N` 未归一化 ⇒ 改口径=偷偷改探索强度）。**现在是**：`Z_SCALE`+
   `c_puct_for`+`q_init_for` 三者绑定口径（§2.6）。
6. **值口径 `engine`/`lossshape`/`hpw` 已删**；现役只有 `hp`(默认)/`hp_turn`/`enginenorm`。
7. **`c_puct` 是"胜率—省血"旋钮、不是胜率旋钮**（n=1000 钉死：四档胜率 64.6~65.8% 全平；战损 1.5→0.25
   −10.1pp ≈10σ，赢局剩血 +8.1）；曲线**内点最优**（c=0.1 反弹）⇒ 不是"越小越好"。
8. **回放缓冲/教师锚点已删**：训练集 = **本轮**数据。代价实测：小份 41k 行等权每轮 **−1.41pp**；挂 876k 行
   教师锚点才 +1.00pp（后被重新归类为"重新贴合祖先数据"）。要拼接多轮得单独接。
9. **`_chk_shape.py` / `devshape.py` / `azsearch.py` / `frozen` 后端 / `MCTSNode` / `eval_walk` 全已删**；
   搜索只留 C++ 一份 + 冻 golden。守卫集中在 `apps/*-test.cpp`。
10. **「快照唯一来源 = `include/sim/BattleWrapper.h`」作废** ⇒ 现在是 `bindings/slaythespire.cpp` 的 `Snap`。
11. **act4 口径已修**：`scenebuild.cpp::actOf` = `1 + (floor>=17) + (floor>=34) + (floor>=51)`（51..53 是一等公民，
    与 Python 原版 `ACT_EDGES=(17,34,51)` 一致）；`scene.act_of` 只是转发 `sts.act_of`。
    ⚠️ 绑定里的 docstring 仍写着"17/34 是幕分界"（过时文字，改的时候顺手修）。
12. **「PG/策略梯度优于模仿」= 否**（同 lr 下 −0.10/−2.08/−2.95pp）；**深度加倍无效**（D8−D4 −0.08pp p=0.57）；
   **降 lr 更差**；**1-ply 值路由更差**（−6.15pp）。
13. **「教师=预言机 ⇒ ~70% 是信息上限」被否**：缺口在**多步规划**（搜索头 +13.3pp 全集中在规划型遭遇），
    不在信息集；牌序泄漏**不是**杠杆（真序无特权）。
14. **「换机器 ⇒ torch 侧不复现」= 错**（判据错在拿 loss 探针当尺子，dropout 消耗 RNG）；正确判据 = 比存盘权重
    （跨机 26 张量 0 不同）。
15. **`jit.trace` 更慢已撤回**（两次重测都快 1.43~1.71× 且逐位一致）；**`recompile_limit` 不需要提高**。
16. **「每轮只加 2016 场数据量轴」**：数据 ×5 后每轮损失从 −1.41pp 收窄到 −0.11pp；挂锚点是唯一能动门控的轴。
17. **旧战斗网权重 load 不了**（`in_dim` 与键集都变过）；`OBS_DIM 770/997` 这类魔数**别再用**，按
    `sts.snapshot_layout()` 推导。

## 6. 已知未解 / 待办 / 上限

1. **`shard_rows=200000` ≈1.5 GB 写缓冲**（§2.14）：要降就传更小的 `shard_rows`，或让 `ShardWriter::flush` 增量追加。
2. **`CARD_SELECT` 候选块只覆盖 21 个任务的一部分**（其余落 `default:` ⇒ `candidateCount=0`）；真修法（候选
   multi-hot + `confirm`）未做。
3. **`walk/scene/rlenv` 三处难度口径不一致**（曾经 `ASCENSION=0/20/0`）⇒ 只留 `scene.ascension_of(seed)` 后仍需确认调用方是否统一。
4. **A20 牌组曲线只有终局一个锚点**，段间形状是等比缩放（非实测）。
5. **值口径/根噪声的 RNG 未换成能复现 Python 的流**（§2.12）⇒ 探索档与旧数据不可比。
6. **`selfplay-test --sweep` 的帧数与历史读数**：随 `.ts` 与代码版本变（2026-10-14 实测 12 例/250 帧）——
   它断言的是**自洽**（π 归一/动作合法/两次跑逐位同），不是某个固定帧数，别把帧数当 golden。
7. **`chain.sh`（无 `_` 前缀那个）全仓零引用**，是旧分发器的残留；`_chain.sh` 才是在役入口。
8. **未做的架构探索**：(a) 前向搬进 C++ 的收益上限 1.33~1.70×（§3）；(b) 要 4~5× 得把 PUCT 树一起搬（另一量级工作）；
   (c) `--infer server` 之类后端已删，别按旧文档找。
9. **GPU 训练支线**：跨设备数字与 CPU **不逐位可比**，必须当独立队列标注。

## 7. 环境 / 运维

- 远端（现役 2026-10-14）：`root@connect.westc.seetacloud.com:38825`（`_*.sh` 头部默认值），
  `RDIR=/root/autodl-tmp/sts_lightspeed`，`PY=/root/miniconda3/bin/python`，cgroup **16 核**（`nproc=128` 是宿主机假象），
  RTX 4080S、torch 2.12.1+cu130、cmake 3.22.1、gcc 11.4（**无 ninja**）。完整四步见 `.agent/REMOTE.md`。
  两条 `.agent/REMOTE.md` 里还没有的：
  1. `conda install libstdcxx-ng`（11.2.0 缺 `GLIBCXX_3.4.30` ⇒ `import slaythespire` 报错），升到 15.x 解决。
  2. 起步权重放 `data/warm.pt`（= `_chain.sh` 默认 `NET`，3.1 MB）；新实例上先 `collect_teacher → train` 蒸馏一个再拷成这名字。
  另：`bindings/slaythespire_pkg` 必须在推送清单里（`file(GENERATE INPUT …)` 配置阶段就要读它）。
- **产物落 `data/`，不写 `/tmp`**（远端重启即丢）；每轮的分片目录与权重都留着，磁盘不自动回收。
- **CUDA 红线**：上下文不能在 fork 的子进程里用；并行开在「任务」层、不要开在 BLAS 里；采集单进程。
- **换行符**：远端要执行的 `.sh` **必须 LF**（`.gitattributes` 已钉 `*.sh text eol=lf`）；Windows 侧写文件用
  `[IO.File]::WriteAllText(..., $s -replace "`r`n","`n")`；`Get-Content -Raw | ssh` 会把 CRLF 带过去打坏远端 bash。
- **ssh/scp 坑**：`scp` 目标目录必须先存在；`ssh -tt` 正常收尾也打 `Connection to … closed.`；杀作业按 pid
  （`pkill -f` 会自杀、先 `pgrep -af`，自匹配用 `[c]ollect_...` 断字面量）。
- **本地跑测试**：见 §2.15（PATH 里补 torch/lib 与 python DLL 目录，否则 `0xC0000135`）。

## 8. 存疑（未核实，别当结论用）

1. `train.py` 的 `record` 走 `data/runs/*.jsonl`——旧文档里 `tboard`/`points.jsonl` 那套可视化已不存在，
   谁在读 `data/runs` 未查。
2. `.agent/REMOTE.md` 仍以旧实例（westb/48068、纯 CPU 口径）为背景 ⇒ 与 §7 冲突时以 §7 + 各 `_*.sh` 头部为准。
