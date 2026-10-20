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
| 热循环（全 C++） | `collect/selfplay.{h,cpp}` + `selfplay_{tree,forward,report}.{h,cpp}` + `selfplay_pool.h` + `collect_util.cpp` | 自对弈：**W 条 worker 线程 + K 棵在飞的树**（ready 队列，树**不绑线程**）+ 前向服务 `ForwardService`；入口/调度/单树 step/池状态/报表分文件 |
| | `collect/teacher.cpp` | 教师采集 `runTeacherAll`：W 条 worker **领号**（`j = started++`，在飞满 `S = 2W` 就等）+ 出口按场序排空；绑定在 `collect/teacher_bind.cpp`（与热循环分开编） |
| | `collect/eval.{h,cpp}` | 配对评估；`net:<path>`（不给 `:sims`）= 裸网络走 `playNetBatch` |
| | `collect/collect.h` | `Frame/Term/GameOut` + `readTerm`/`snapshotObs`（两个采集器共用；实现在 `collect_util.cpp`，谁都不 include 对方） |
| | `collect/netprobe.h` | 前向会话的建立校验（空探一次 + 头宽钉到动作空间），三个调用点共用 |
| | `collect/{encs,scenebuild,snapshot,action}.{h,cpp}` | 任务表 / 造局面 / 观测快照 / 动作层 |
| 容器 | `collect/shardio.h` | **分片容器（唯一容器）** v2，头顶 24 字节；落盘外壳 = `collect/shard_out.h` 的 `ShardSink`（目录/命名/切分/汇总，两个采集入口共用） |
| 绑定 | `bindings/slaythespire.cpp` | `snapshot_layout()` / `action_layout()`（**布局与动作宽度的唯一来源**）；各采集器的 `registerXxx` 在 `collect/*_bind.cpp`（热循环那一侧不碰 pybind11） |
| | `bindings/net_eval.h` + `onnx_eval.*` | `NetEvaluator`：**唯一调用面**，唯一引擎 = ONNX Runtime（2026-10-15 起 libtorch 已撤）。内部记 H2D/前向/D2H 分段耗时 |
| 触发/训练（Python） | `collect_selfplay.py` / `collect_teacher.py` / `eval_gate.py` | **薄触发器**：拼产物名 + 装 `*Config` + 打一行读数 |
| | `train.py` / `aznet.py`（网络定义唯一一份）/ `zspec.py`（值口径+搜索旋钮）/ `collect.py`（读侧 `CollectData`）/ `collect_io.py`（字节层） | |
| | `onnxnet.py`（导出/验收） / `netarchive.py`（唯一入口） / `ckpt.py` / `metrics.py` / `runtime.py` / `scene.py` | 权重 → `.onnx` / 拼名与挡旧扩展 / 权重指纹 / 读数 / 命名+配额 / 场景转发 |
| 循环与运维 | `chain.sh`（自迭代，**无参数**，旋钮是裸常量）；`sync.sh` + `_push/_start/_kill/_status/_log/_wait/_check/_attach.sh` | `sync.sh` 已重构为**薄分发器**，实现别往里加 |
| 守卫 | `apps/{engine,scene,selfplay,selfplay-failure}-test.cpp` | Python 侧 `_chk_*.py` **已全部删除**，守卫都在 C++；`selfplay-failure-test` 不需要存档（含落盘外壳断言） |

- 根目录 `*.py` 全表（17）：`azaction azlayout aznet check_card_data ckpt collect collect_io
  collect_selfplay collect_teacher eval_gate metrics netarchive onnxnet runtime scene train watch zspec`。
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
7. **种子纪律**：`collect.COLLECT_SEED0 = 0`（教师 / 手工固定段）、`collect.SELFPLAY_SEED0 = 1_000_000`
   （自对弈**轮换段**起点）、`collect.EVAL_SEED0 = 100_000_000`（评估，held-out；工具拒绝更低的 seed0）。
   对照必须**同 seed 配对**；换种子段 ⇒ 绝对胜率不可比。`MAX_STEPS = 1000`（单局步数上限，与
   `SelfplayConfig::max_steps` 同）。
   ⚠️ **自对弈每轮换一段**（`collect.round_seed0(轮号, n_per)`，2026-10-15）：段 = `[S, S+n_per)`，
   段号 = `((轮号-1) × stride) % n_seg`（stride 与 n_seg 互质 ⇒ 段**互不重叠**、相邻轮跨得远），
   **纯函数** ⇒ 断点续跑不必存状态（`$LOGD/seed_history.txt` 只是台账）。从前每轮都是
   `start_seed=0` ⇒ 每项恒用 seed `0..n_per-1`，**每轮都在同一批开局上自对弈**。
8. **设备与存档必须同源**（`netarchive.py` → `onnxnet.py`）：存档**按设备各一份**
   （`data/onnx/*-cpu.onnx` / `*-cuda.onnx`），因为"这一次前向跑在哪个设备"是**配方**。
   ⚠️ ONNX 是设备无关的图（设备由执行提供者定），但**保留按设备分文件**的约定：否则 cuda 那一档
   会静默回落到 CPU 图（慢且难查）。按内容指纹 + 设备命名、只读共享、原子落盘（`.tmp`→`replace`）。
   ⚠️ `netarchive.ensure_net()` 还会校验 `slaythespire.NET_BACKEND == "onnx"` —— 装到旧扩展
   （链 libtorch 的那份）时当场报错，而不是拿 `.onnx` 去喂 TorchScript。
9. **前向必须 float32**（C++ 侧用 double 会与 Python 差 1e-8 级 ⇒ 十几步后轨迹分叉）；`NNEval` 的
   **第一次前向必须在主线程跑过**再起服务线程（头宽度/CUDA 上下文/执行计划那时才建立）。
   ⚠️ 头宽度**优先从模型元数据读**，读不到（`factor` 策略头导出后那一维是**动态**的，
   实测拿到 `-1`）就在首次前向按实际输出钉住。
10. **推理引擎 = ONNX Runtime（唯一）**（`bindings/net_eval.h` 是唯一调用面，2026-10-15 定案）：
   C++ 侧**不链 torch** —— `selfplay-test.exe` 只依赖 `onnxruntime.dll`（连 Python 都不需要了），
   `.pyd` 只多一个 `python3XX.dll`。
   ⚠️ 撤掉 libtorch 之前测过两档口径（数值**不可能逐位跨实现相同**）：同权重 max|Δ| ≈ 3e-7、
   两个头的 argmax 逐行一致，且**同权重同 sims 的帧数完全一致**（K=1/4/8 = 12/66/114、
   `--sweep 24` = 256 帧）。那些数字现在只作历史参照（旧实现已删，要对账从 git 历史取）。
   ⚠️ 速度（本地 CPU、同权重、intra=1）：batch=1 ORT 快 3.8×、n=8 快 1.44×、**n≥64 打平到略慢**
   （K=256 sims=4：torch 4.6 s vs ORT 5.0 s）；远端 GPU 那一档另测（见 §3，端到端 ≈3×）。
11. **复现性边界**：⚠️ **自对弈侧没有在役的冻 golden** —— `collect/tests/golden_selfplay.txt` /
    `golden_teacher.txt` 的**读侧（`_chk_selfplay.py` / 对应的 teacher 脚本）早已删掉**，现在没人读它们
    （live 的只有 `engine-test --golden` / `scene-test --golden` 那两个）。所以自对弈的守卫是**自洽型**：
    `apps/selfplay-test.cpp` 断言「同参数两次跑逐位相同」+「π 归一 / a_idx ∈ legal」（`K=1` 与 `--sweep`）。
    K>1 的批组成由到达时序决定、而前向与批宽相关 ⇒ **不可能逐位复现**；CUDA 核非确定 ⇒ cuda 路径上
    连 K=1 的逐位复现也没有。
    池化路径（`selfplayPooled`，池宽 < 任务数）另有一条**更强的**断言：**池宽 = 1 ⇒ 批恒 1 行 ⇒
    结果必须与「逐场单跑」逐位相同**（`selfplay-test <ts> 8 4 1 --pool 1`，实测 114 帧逐位一致）。
    ⇒ 改搜索口径（如 2026-10-14 删 virtual loss）**不会打红任何在役断言**，只能靠"帧数/行为变了"看出来
    （这也是为什么口径变更必须显式记一笔，见 §5-20）。
12. **批宽 ≤ 在飞的场数**：一棵树同时只挂 1 个在飞前向请求（`TreeState::waiting`）⇒ 一次前向能凑到的
    行数上限就是"此刻有几场还没跑完"。推 corollary：**行/场 = 决策点数 × sims**（一次前向里 1 场 = 1 行），
    所以「均批 84 行」= "那一刻平均只有 84 场还活着"，而累计行数是场数的 ~2300 倍（4,668,504 ÷ 2,016）。
13. **训练线程数是配方**：换线程数 = 换轨迹（1 vs 8 线程 `val_ce` 差 0.01~0.02、权重最大差 0.6~0.9）；
    **同线程数跨进程/跨 torch 版本逐位可复现**（2.8.0+cu128 ↔ 2.12.1+cu130 逐位同）。差异随训练步数放大。
14. **RNG 不是同一条流**：`std::mt19937_64`（C++ 搜索）vs `np.random.default_rng`（旧 Python 路径）
    ⇒ 开探索（π 采样/eps/温度/根噪声）时两条路的行为序列**不可逐位比**；探索全关时都是 argmax 才可比。
15. **`edges` 与枚举序**：`root.edges` 插入序 == `enumerateActions` 枚举序；并列取「枚举序第一个」，
    而 `np.argmax` 取「下标序第一个」⇒ 低 sims 下会分叉（实测 16% 帧）。
16. **写缓冲上限**：`shard_rows = 200000` 行/分片 ≈ **1.5 GB 写缓冲**（`ShardWriter` 攒够才 flush）——
    当前内存第二大头，未改。
17. **构建**：本地用 VS 的 cmake，**必须带 `-T ClangCL`**
    （`...\CMake\bin\cmake.exe -S . -B build -T ClangCL -DCMAKE_BUILD_TYPE=Debug` 然后
    `--build build --config Debug`）：`cl.exe` 默认按系统代码页（本机 GBK）读源文件，而本仓满是
    中文注释 ⇒ 会报一堆看不懂的语法错（实测 `collect/snapshot.h` 30+ 条）。
    跑 `build/Debug/*-test.exe` **不再需要**任何 `PATH` 调整（ONNX 引擎 ⇒ 只依赖
    `onnxruntime.dll`，由 CMake 拷到产物旁边）；以前那句"把 `torch/lib` 放进 `PATH`"已作废。
    ⚠️ `include/sts_common.h` **无条件** `#define sts_asserts`（自检默认开），但块体里用的是 C 的
    `assert` ⇒ `-DNDEBUG`（Release）下**被预处理掉、静默失效**（`include/combat/ActionQueue.h:72` 那种写法）
    ⇒ 断言类守卫要用 Debug 跑；别把 `sts_asserts` 删掉当"修好"。
18. **产物名不带配方**（2026-10-15 定案 —— 取代了"名字 = 完整配方"）：数据目录 = `<cache-dir>/<kind>`
    （`spencs` / `encdata_teacher`），权重 = `<数据目录名>.pt`（落 `data/`），**没有自定义名字**
    （`--name` 已删）。要落到别处就显式给路径：采集 `--cache <目录>`、训练 `out` 位置参数
    （链条就是这么分轮的：`data/sp_<STAMP>_r<轮>`）。
    **配方改由产物内部承载**，唯一性靠"同名 ⇒ 必须同配方，否则炸"：
    · 数据目录：`<目录>/_recipe.json`（`runtime.check_recipe`）—— 复用前**逐字比对**，不一致就
      列出不同项并报错；**有分片却没有 `_recipe.json`** 的目录也报错（旧命名时代的产物 / 手造的）
      ⇒ 换 `--cache` 或删掉重采。`shard_rows` / `cache_dir` / `cache` / `net_path` **不进**配方
      （分别是粒度、路径、语义上的"同一份权重换个路径"）；网络进配方的是**内容指纹 + 推理后端**。
    · 权重：存档里的 `recipe`（`ckpt.save(..., recipe=…)`；`config` / `z_spec` / `metrics` 本来就在）
      = 数据（含**混训各份与权重**）+ 全部训练超参。
    ⚠️ 为什么要反过来：名字 = 配方 ⇒ 每加一个旋钮就长十几字符，而单段上限 255（ext4/NTFS）——
    实测生产配方**第 2 轮就 271 字符**（`start_seed<8位>` 与 `net<上一轮权重名首段>_wfp<指纹>` 一起
    进来）⇒ `os.makedirs` 在采集第一步抛 `ENAMETOOLONG`，每加一个旋钮复现一次。
    **代价（明确接受）**：从文件名看不出配方 —— 要看 `_recipe.json` / 存档 / 运行日志
    （采集与训练都会把完整配方打一行）；`data/` 里的旧命名产物**不再被复用**（见上一条的报错）。
    **已退休的命名机具**（别再用）：`runtime.cache_name` / `_fmt_val` / `dataset_tag` / `net_tag` /
    `short_name` / `DISPLAY_DROP` / `NAME_MAX` / `_fit_name` / `ckpt_name` / `dataset_mix_tag` /
    `_pt`，以及 `train.ckpt_params` 与两个采集器的 `default_cache` / `NAME_DROP` / `NOT_IN_NAME`。
19. **`chain.sh` 的 `pick` 不能用全角括号当 `-F` 的字符类**（2026-10-15 修）：mawk 在没有 locale 的
    环境里按**字节**解释字符类，而 `（`/`）` 的 UTF-8 字节（EF BC 88 / 89）会命中标签自己的字节
    （`物` = E7 **89** A9）⇒ 标签被劈开、`pick` **恒返回空** ⇒ 链条每轮死在第一道解析
    （现场：`data/logs/chain_*/` 里**只有** `collect_1.log`，从没跑完过一轮）。现在用默认 FS（空白）
    + 从第一个非 ASCII 字节起截断。**判据**（远端 LANG 是空的，所以这条必须按空 locale 测）：
    `awk -v k=产物目录 '…' <collect 日志>` 必须回出路径。
20. **怪物索引越界 ⇒ 踩内存（2026-10-16 修）**：`arr[idx]` 只允许 `[0, monsterCount)`，
    **用 `monstersAlive` 判是不够的**。触发窗口 = 觉醒者转阶段 / 暗灵待复活那段：半死怪不算
    可选中，`monstersAlive` 掉到 **0**，而 `areMonstersBasicallyDead()` 仍是 false（半死算活着
    ⇒ 这场没结束）；此时 `getRandomMonsterIdx(aliveOnly)` / `getFirstTargetable()` 都回 **-1**
    （"没有合法目标"，是它们的契约），而 `Havoc` / `DistilledChaos` / 遗物（`Player.cpp`）与
    `CardQueueItem::randomTarget` 会把这个 **-1 直接当怪物下标**塞进排队卡的目标。
    `arr` 是 `MonsterGroup` 的**第一个数组** ⇒ `arr[-1]` 落在 `BattleContext` 前面那些字段上
    （不是数组外空洞）⇒ `isDeadOrEscaped()` / `isTargetable()` 读出"看起来活着能打" ⇒
    `AttackEnemy(-1)` → `Monster::attacked()` → Hand Drill 分支把 `this->idx`（野内存 =
    `-1509947384`）塞进 `DebuffEnemy` ⇒ 真踩内存、采集进程 SIGSEGV。
    **现场**：第 3 轮采集 1800/2016 场崩（`AWAKENED_ONE@50 seed=24370753`；`monsterCount=3
    monstersAlive=0`）；`--n 2` 就能在 ~100 s 复现，`gdb` 六次同一栈同一场。
    **修法**（口径 = 原版：没有合法目标就**打不出**需要目标的牌）：
    ① **查询侧**（`CardInstance::canUse` + `playCardQueueItem`）的目标检查先过 `monsters.isIdxValid()`
    （越界 ⇒ 这张牌不打出；`targetable` 只算一次、两处共用）；
    ② **动作侧**（"按 idx 找某只怪"的那些：`Actions::AttackEnemy` / `DamageEnemy` / `MonsterGainBlock` /
    `BuffEnemy` / `DebuffEnemy`(`BattleContext::debuffEnemy` 里的 `arr.at(idx)`) / `FeedAction` /
    `HandOfGreedAction` / `RitualDaggerAction` / `SpotWeaknessAction`）**不做静默空转**，一律
    `arr.at(idx)`：越界 ⇒ **抛 `std::out_of_range`**（响亮失败，符合"绝不静默跳过"的纪律；修好之后
    它不该再被触发，留着只为"下次真出问题别又变成野内存 SIGSEGV"）。
    **实测证据**：修后同一配置 `--n 2` 跑完 126/126、`EXIT=0`，且队列侧那一行正是
    `playCardQueueItem(排队卡的目标越界) idx=-1 monsterCount=3 monstersAlive=0 seed=24370753`
    ⇒ 说明拦在①（动作侧从没被触发，即野索引再没进过动作层）。
    守卫 `selfplay-test` 全过（K=1 逐位同 / sweep 217 帧 / 池宽 1）。
    ⚠️ **数据谱系**：这条修复只在"半死窗口"里改行为（以前那里靠**野内存**决定打不打得出去），
    而且**没有**进配方（配方记的是网络指纹/后端/采集旋钮，不是代码版本）⇒ 修前后采的数据理论上
    不同源（实测同一配置两边行数只差 **1 行/216 万**，可忽略）；`chain_1015_2332` 的 r1/r2 是修前、
    r3 起是修后，混训时不校验这一层（`CollectMix` 只比 obs 宽 / 动作空间 / 列名）。

21. **落盘外壳 / 会话建立 / 产物复用各收在一处**（2026-10-17）：① 分片外壳 = `collect/shard_out.h` 的
    `ShardSink`（建目录 + `blk%05d.bin` 命名 + 按行切分 + 路径汇总 + `shard_rows > 0` 校验），
    `selfplayRun` / `teacherRun` 都只调它 ⇒ §2.5 那条“行序 == 场序”由此**一处**保证；
    ② 前向会话的建立校验 = `collect/netprobe.h`（空探一次 + 头宽钉到 `kPolicyDim`/`kSelectDim`）；
    ③ 产物复用 + 配方闸 = `runtime.reuse_shards`（两个采集脚本共用）。
    ⚠️ 头宽**只在建立会话那一刻**查一次（`ForwardService::start` 不再自查）⇒ 直接构造
    `ForwardService` 的测试要自己先 `probeForwardChecked`。
    ⚠️ `n_per ≥ 1` 的契约在 `encJobs` 里拦（负数会在它的 uint64 游标上绕成天文数字）。
    ⚠️ 行为变化（已点名）：`teacherRun` 现在会拦 `n_per`/`shard_rows`（此前 `shard_rows=0` 静默退化成
    “一行一个文件”）、目录创建失败不再被 `error_code` 吞；`eval` 的裸网络路径现在也校验头宽
    （此前 `p_logits[下标]` 会越界读）；teacher 复用那行日志统一成“产物目录 … 复用已有 N 个分片”。

## 3. 旋钮与实测数字（为什么是这个值）

### `chain.sh`（自迭代循环，2026-10-15 在役值）
本轮产物的名字 = `data/sp_<STAMP>_r<轮>`（数据目录）与 `data/sp_<STAMP>_r<轮>.pt`（权重）——
**名字里不带配方**（§2-18），只用来分轮/分运行；配方在 `_recipe.json` 与存档的 `recipe` 里。
`N_PER=32`（每项场数，**任务表** = ENCS 表项数 63 × N_PER = 2016 场；**也是种子段宽度**）、`SIMS=3000`、
`SIMS_CHEAP_FRAC=0.83`（随机化预算的**便宜档占比**：便宜预算 = max(2, sims×它)；0 = 关）、`SIMS_FULL_FRAC=0.25`（走完整预算的决策点比例）、
`SP_POOL=512`（**池宽**= 在飞的场数，跑完一场立刻补一场；一棵树同时只挂 1 个在飞前向请求 ⇒ 它也是
**前向批宽的上限**。⚠️ 池宽**不是线程数**，也**不是任务表大小**）、`SP_WORKERS=14`（worker 线程数，
取核数−2；树不绑定线程）、
`DEVICE=cuda`、`EPOCHS=2`、`LR=3e-4`、`WORKERS=32`（训练线程）、`LOSS_CHUNK=16384`、`PI_WEIGHT=none`、
`REPLAY_ROUNDS=3`（回放：挂最近几轮的产物）、`REPLAY_DECAY=0.6`（权重按轮龄几何衰减）、
`SOFT_TARGET=1`/`SOFT_TEMP=1.0`、`TEMP_MOVES=30`、`DIR_TOTAL=10.83`、`DIR_EPS=0.1`、`GATE_GAMES=512`、
`GATE_N_PER=512`、`GATE_MARGIN=0.01`、`GATE_SEED0=100000000`（=`EVAL_SEED0`）、`NET=data/warm.pt`、`ROUNDS=0`。

- **开局种子段每轮换**（2026-10-15，`collect.round_seed0`）：段 = `[S, S+N_PER)`，第 r 轮取
  `S = SELFPLAY_SEED0 + (((r-1)·stride) % n_seg)·N_PER`（`n_seg = (EVAL_SEED0 − SELFPLAY_SEED0)/N_PER`，
  `stride` 与 `n_seg` 互质、取黄金比 ⇒ 段互不重叠且跨得远）。实测前几轮：`1e6, 62185376, 24370752,
  85556128, 47741504, …`（都 < 1e8 = 评估段）。⚠️ 代价：**绝对胜率跨轮不可比**（本来也只能看配对差）。
- **回放（混训）**（2026-10-15，`collect.CollectMix` / `train.py --mix`）：训练集 = 本轮 + 最近
  `REPLAY_ROUNDS` 轮的产物，回放权重按轮龄几何衰减（`0.6^k`）⇒ 3 轮 ≈ **50.9/30.5/18.5%**（要的
  50/30/20 口径）。⚠️ **一轮的优化步数不变**（行数锚在本轮：`len(epoch_rows) == 本轮训练行数`），
  否则"挂回放"与"多训几步"分不开。冻结/落盘口径：混训各份与权重写在**存档的
  `recipe.data`** 里（名字里不带配方，见 §2-18）；台账 `$LOGD/replay.txt`
  一行一轮（`<轮号> <目录>`）—— 续跑靠它恢复"哪轮用了哪份"；缺行/目录不见了**当场死**（少一份
  就是另一个配方）。
- **`PI_WEIGHT` 基线改 `none`**（2026-10-15）：`advantage`（`max(0, z−V̂)`）在败局上整批归零 ⇒
  难局的策略梯度直接消失；等权的代价是"输局里也照抄教师"（历史实测 −0.08/−0.30pp）。**先跑等权
  基线**，再单独试温和权重（一次只改一件事）。
- **2026-10-18 续跑结果（`chain_1015_2332`，从 r12 断点继续，复用 r1~r11 数据）**：
  r12 **69.26%** → r13 **69.67%** → r14 **70.27%**（22667/32256，`seed0=1e8`，512 场/项裸网；
  r14 采集 583 s / 44,431 行）。r14 比 r8 **+1.5pp**，门控采纳，`best` 更新为 r14；
  达到 70% 目标后手动停链（r15 只留下空壳 `_recipe.json`，未产出分片）。
  ⚠️ 这是**裸网络**绝对胜率，不是 MCTS 口径；`GATE_N_PER=512`、固定 held-out seed 段、配对判据未变。
- **2026-10-18 `N_PER=64` 复测（r15）**：每轮 4032 场 / 91,027 行 / 采集 1489 s；裸网
  **70.96%**（22888/32256）vs r14 70.27%，**+0.7pp**，未到 1% best 阈值（best 仍 r14）。
  ⇒ 数据量翻倍有小幅正收益，但不足以单轮过阈值；链继续从 r15 跑 r16。
- **2026-10-18 r16（`N_PER=64`）**：裸网 **71.04%**（22916/32256）vs r14 70.27%，+0.77pp
  （vs r15 仅 +0.08pp）；采集 88,451 行 / 1401 s。r15/r16 说明数据量翻倍的收益在 ~+0.8pp 附近
  开始走平；r16 后停链，改测 MCTS 1024（r14/r16/teacher）。
- **2026-10-18 裸动作重点转向回放深度（r16 数据，无新采集）**：
  `r16 + replay5`（r15..r12，0.6/0.36/0.216/0.1296）= **71.61%**（23098/32256），
  vs r16 71.04% **+0.56pp**、vs r14 70.27% **+1.34pp**。这是目前最高的裸动作胜率；
  同配方 512 场/项裸网门控，`seed0=1e8`，产物 `data/sp_1015_2332_r16_replay5.pt` /
  `data/logs/mcts/replay5_gate.json`。replay7（r15..r10）门控 **71.59%**（23092/32256），
  与 replay5 的 71.61% 打平/略低 ⇒ 回放深度在 5 轮附近饱和。
- **2026-10-18 裸动作单变量收口（r16 数据，512 场/项，`seed0=1e8`，32,256 场）**：
  `replay5`（soft target，2 ep，0.6 衰减）= **71.61%**（23098/32256）为本轮最佳；
  `replay7` = 71.59%（回放深度 5 轮饱和）；`replay5 + 4 ep` = 71.04%（多训无益）；
  `replay5 + hard target` = 71.31%（软目标更好）。⇒ r16→replay5 的 **+0.56pp** 是裸动作
  可复现的最好增益（vs r14 +1.34pp）；产物 `data/sp_1015_2332_r16_replay5.pt`，
  门控 `data/logs/mcts/{replay5,replay7,replay5_e4,replay5_hard}_gate.json`。
- **2026-10-18 MCTS 曲线（r14，`seed0=1e8`，同一批场景）**：
  r14 在 64/256/1024 sims = **84.9 / 86.5 / 86.9%**（252 场）；1024 sims、504 场 = **87.1%**。
  teacher 在 256/1024 sims = **82.5 / 85.7%**（252 场）；1024 sims、504 场 = **86.9%**。
  ⇒ teacher 80%+ 确认；r14 的 MCTS 与 teacher 基本同水平（+0.2~0.5pp，样本内不显著），
  裸网 70.27% 与 MCTS 86.9% 的差距来自搜索，不是“裸动作会 80%”。
  产物：`data/logs/mcts/curve4.json`、`curve1024_n8.json`（远端）。
- **2026-10-18 r16 MCTS 1024 对照（504 场/项）**：r14 **87.10%**、r16 **87.30%**、teacher **86.90%**。
  r16 的裸网 +0.77pp 到 MCTS 只剩 **+0.2pp**（样本内不显著）⇒ `N_PER=64` 的收益基本被搜索吸收；
  继续加数据量不是提高 MCTS 的有效杠杆。产物：`data/logs/mcts/r14_r16_1024_n8.json`。
- **2026-10-18 eval 提速（评估侧）**：`EvalSpec` 补 `workers`（默认按 cgroup，不再用宿主 128）+
  `model_path_cpu` / `cpu_rows` 尾段分流。1024 sims / n_per=4（252 场）实测：
  `workers=12` 181 s → **+CPU 尾段 114 s**，胜率不变（r16 86.9% / teacher 85.7%）。
  4096 sims / 252 场：无尾段 **753 s** → 尾段版 **437 s**（−42%），胜率同为
  r16 89.3% / teacher 88.1%。产物：`workers12_n4.json`、`cputail_n4.json`、`cputail_4096_n4.json`。
- **2026-10-18 裸网 eval 的真正瓶颈是 `--games`，不是 CPU 线程**：`eval_gate` 旧默认
  `--games 8` ⇒ 裸网按 8 场一个小 batch，32k 场要 4032 次调用（3 个规格 455 s）。
  改成 `--games 512` 后 3 个规格 **98 s**；单 Session 32k 单规格 **20 s**。
  试过 12 个独立 Session 分段并行：结果完全一致，但每个 Session batch 只有 ~42，
  3 个规格 **98 s**（约 32 s/规格）⇒ **并行反而更慢**。因此 `eval_gate` 默认
  `--games=512`，裸网固定单 Session（`workers=0`）；MCTS 仍用 `--workers`。
- **2026-10-18 网络 vs SimpleAgent（同场景，512 场/项，32,256 场）**：
  `net:replay5` **71.61%**、`net:r16` **71.04%**、`simple` **62.69%** ⇒ 同遭遇/同层/同 seed 下
  网络明显高于 SimpleAgent（+8.3pp）。**战损也不更高**：终局 HP 比 replay5 **33.3%** /
  r16 33.0% / simple 24.3%；仅胜场 HP 比 **46.6% / 46.6% / 38.9%**。产物
  `data/logs/mcts/net_vs_simple_hp.json`。
  整局 `fullrun-test` 500 seeds：`--battle bare` **2/500（0.4%）**、`--battle agent` **5/500（1.0%）**。
  逐 seed 配对：net 胜 `{100000065, 100000479}`，agent 胜 `{100000024, 100000065, 100000181,
  100000479, 100000482}` ⇒ **net 的两胜 agent 也全胜，agent 另多 3 胜**；McNemar 约 p=0.25，
  方向偏向 agent 但不显著。net 平均层数 23.5 > agent 22.7，但“走得更深”没有转成通关。
  ⇒ 单场战斗/战损网络更好，整局转化率没有更好；瓶颈在战斗外的状态轨迹/牌组，而非单场战斗。
  **2000 seeds 复核**：net **6/2000（0.30%）**、agent **15/2000（0.75%）**；配对 net-only 2 局、
  agent-only 11 局（McNemar p≈0.02）⇒ **整局 SimpleAgent 显著更好**，此前 500 局的“不显著”
  是小样本。net 平均层数 23.3 > agent 22.7，但通关转化更差；单场 HP 更高，所以不是战损，
  而是战斗结果改变 HP/金币/奖励后，非战斗状态轨迹与牌组分叉。
- **2026-10-18 难关专项（`--hard-only`：强怪/精英/Boss/Act4/事件，26,112 场）**：
  net replay5 **66.7%**、net r16 **66.1%**、simple **57.2%**；终局 HP 比 **30.1% / 29.7% /
  21.3%**，胜场 HP **45.2% / 45.1% / 37.4%**。⇒ 就算只算难关、跳过三幕弱怪，网络仍是
  胜率更高、战损更低；“简单关稀释”解释不了整局差距。产物 `net_vs_simple_hard.json`。
- **2026-10-18 逐难关 2048/项（104,448 场，`per_enc_hard_2048.json`）**：唯一负项是
  **The Heart** 胜率 `-0.9pp`（1.4% vs 2.2%，HP 0.5 vs 0.6，胜场 HP 反而 +7.8pp），
  其余难关网络全部 ≥ SimpleAgent；最大的正向是 Gremlin Nob **+38.6pp**、
  Centurion+Healer **+21.7pp**、Slavers **+21.4pp**、Masked Bandits **+21.1pp**。
  ⇒ 逐难关平均网络全面占优，但**平均不保证逐状态占优**。
- **2026-10-18 逐 seed 复现（seed 100001587）**：两边 **35 牌 / 10 遗物 / 25 战 / 159 非战斗** 完全
  相同；最后一战都是 **Awakened One 层=50**：net 14 决策 → 负（hp 0），SimpleAgent 56 决策 →
  胜（hp 46）。⇒ 整局差距确实可以是**关键 Boss 的单场战斗**，不是非战斗分叉。此前“整局只能
  归因战斗外”结论过宽；准确说法是：网络在 2048 场平均难关更强，但在决定通关的特定 Boss 状态
  上会输。产物 `seed1587_bare.log` / `seed1587_agent.log`。
- **2026-10-18 seed 1587 + agent-only 11 局的 MCTS 兜底**：seed1587 用 `--battle search`
  sims=64 与 256 **都过**（256 终局 hp46/80，与 agent 相同）。对 11 个“agent 赢、裸网输”
  的 seed：MCTS64 过 **7/11**，MCTS256 再翻盘 1951 ⇒ **8/11**；仍不过的是
  `100000024`（层50）、`100000482`（层16，16牌/4遗物）、`100000902`（层50）。
  ⇒ 裸网打不过是**策略/覆盖**问题，搜索能救回大部分；剩下 3 局更像早期牌组/路线或搜索价值问题。
- **2026-10-18 难关定向采集（`--hard-only`，n=32 / sims=6000）**：1632 场 / 38,311 行 /
  1362 s。从 replay5 续训 2 ep：纯难关 bare **71.79%**，难关+r16 混训 **71.91%**，
  vs replay5 71.61%（+0.18~0.30pp，基本噪声）。11 个 agent-only seed 上纯/混都只过
  **2/11**（1757、1587），而 `100000482` 仍层16 死，且 644/775/902/1951 等出现回退。
  ⇒ “只采难关 + 拉高 sims”不是有效杠杆；比不过 MCTS 预算（4096 已救 10/11）。
- **2026-10-18 全遭遇野蛮加大（r17：N_PER=64，SIMS=10000，EPOCHS=4）**：
  4032 场 / **86,061 行** / 采集 **3307 s**；裸网 **71.84%**（23173/32256，HP 33.4%），
  vs r14 70.27% **+1.57pp**，best 更新为 r17。但 11 个 agent-only seed 仍只过 **2/11**
  （181、1587），`482` 仍层16 死；单场涨、整局转化没跟上。产物 `data/sp_1015_2332_r17.pt`、
  `data/logs/chain_1015_2332/gate_17.json`、`data/logs/mcts/r17_seeds.log`。
- **2026-10-18 r18（N_PER=128，SIMS=5000，EPOCHS=8，replay5）**：8064 场 / **181,171 行**；
  裸网 **73.40%**（23677/32256，HP 34.7%），vs r17 71.84% **+1.56pp**，best 更新为 r18。
  ⇒ “场数翻倍 + 5 轮回放 + 8 epochs”这次明显有效；r19 已自动继续（同配方）。产物
  `data/sp_1015_2332_r18.pt`、`data/logs/chain_1015_2332/gate_18.json`。
- **2026-10-18 r19（同配方）**：裸网 **73.71%**（23776/32256，HP 35.0%），vs r18 73.40%
  **+0.31pp**，未过 1% best 阈值（best 仍 r18）；但绝对胜率是当前最高。r20 已继续。
- **2026-10-18 r20（同配方）**：裸网 **74.46%**（24019/32256，HP 35.6%），vs r18 73.40%
  **+1.06pp**，过阈值，best 更新为 r20；r21 已继续（7657/8064）。产物
  `data/sp_1015_2332_r20.pt`、`data/logs/chain_1015_2332/gate_20.json`。
- **2026-10-18 r21（同配方）**：裸网 **75.05%**（24209/32256，HP 36.3%），vs r20 74.46%
  **+0.59pp**，未过 1% best 阈值（best 仍 r20）；但绝对胜率是当前最高。r22 已继续。
- **2026-10-18 r22（同配方）**：裸网 **75.31%**（24292/32256，HP 36.6%），vs r20 74.46%
  **+0.84pp**，仍未过 1% 阈值（best 仍 r20）；绝对胜率继续最高。r23 已继续。
- **2026-10-18 r23（同配方）**：裸网 **75.86%**（24471/32256，HP 37.2%），vs r20 74.46%
  **+1.40pp**，过阈值，best 更新为 r23；r24 已继续。产物 `data/sp_1015_2332_r23.pt`、
  `data/logs/chain_1015_2332/gate_23.json`。
- **2026-10-18 teacher:5000 baseline（n_per=8/504 场，seed0=1e8）**：teacher **89.1%**
  （449/504），同批 net r21 **80.4%**（405/504）⇒ teacher MCTS 5000 仍高 8.7pp。
  ⚠️ 504 场样本与 32k 门控不可直接比（net 在 32k 是 75.05%）；teacher 接口拿不到 HP。
- **2026-10-19 teacher:5000 全额基准（n_per=512/32,256 场，seed0=1e8）**：teacher **84.9%**
  （27372/32256，HP 43.3%，战损 20.6%，胜场战损 13.6%），耗时 18,226 s（约 5.1 h）。
  ⇒ 之前 504 场的 89.1% 是小样本高估；权威基线是 **84.9%**。r23 裸网 75.86%，仍差 **9.0pp**。
- **2026-10-19 r27 自搜索曲线（n_per=8/504 场，同批；local CPU）**：bare **81.3%**、
  MCTS64 **84.9%**、MCTS256 **87.5%**、MCTS1024 **87.7%**、teacher1024 **86.9%**。
  ⇒ 同批内自搜索 +3.6/+6.2/+6.4pp；64 已接近 teacher，256 起略超 teacher。
  ⚠️ 该 504 样本的 bare 81.3 明显高于 32k 的 76.71，绝对数不可当全量；只看同批增量。
  产物 `data/logs/r27_mcts_curve.json`、`r27_bare_teacher_n8.json`。
- **2026-10-19 shard v4（紧凑观测/π）**：`obs` float[1603] → 18 个 `obs_*` 块列，值为 int16
  原始值 + `snapshot_layout()['divisors']` 读时还原；`pi_act/pi_sel` 改 u16 未归一访问数，
  读侧再归一。分片体积 **12.12 MB → 6.26 MB（1.93×）**；v3 旧分片仍可读，v4/v3 可混训。
  浮点重建与旧数据**排序后逐位一致**；本地 63 场采集 + 1 epoch 训练通过；远端重编后
  采集/读取冒烟通过。
  产物 `data/logs/teacher5000_full.json`。
- **2026-10-18 逐怪 net r23 vs teacher:1024（n_per=32/2016 场）**：net **76.8% / 战损 27.8%**，
  teacher **83.9% / 战损 23.6%**；45/63 个遭遇 net 胜率更低、51/63 战损更高。差距最大：
  Giant Head `-31.2pp/+14.9`、Time Eater `-31.2/+12.3`、Champ `-21.9/+12.3`、
  Transient `-18.8/+10.6`、Writhing Mass `-15.6/+12.5`、Reptomancer `-15.6/+9.0`。
  ⇒ 对 SimpleAgent 的优势不适用于引擎 MCTS；真正对照是 teacher searcher。
  产物 `data/logs/net_r23_vs_teacher_perenc.json`。

- **并肩/线程形状**（实测探针，sims=200，同批 63 项 × n_per，远端 4080S + 16 核 cgroup）：
  `cuda K=2016 W=14`（每线程 144 棵并肩）**0.122 s/局** < 旧「一棵树一条线程」`W=2016` 0.145 <
  `cuda K=504 W=504` 0.379；`cpu(12 intra-op) K=504` 0.206 反而最快 ⇒ **窄批 CPU 赢、宽批 cuda 赢**
  （CPU 前向的 intra-op 线程要和树线程抢同一个 16 核 cgroup）。按 3000/200 线性外推 ⇒ 一轮 2016 场 ≈60~70 min。
- **池形状 / 批宽（2026-10-14 定案，受控探针：同任务表 2016 场 / sims=200 / workers=14 / cuda / 4080S）**：

  | 形状 | 墙钟 | 均批 | 服务调用 | 每行 | GPU 均 |
  |---|---|---|---|---|---|
  | **池宽 512 + 滑窗补场（现役）** | **202 s** | 79.3 | 190.4 s（95% 忙） | 38 µs | 17% |
  | 池宽 1024 + 滑窗 | 214 s | 83.6 | 202.8 s | 41 µs | 15% |
  | 整表全开 2016（旧形状，= 池宽 2016） | 209 s | 84.2 | 198.6 s | 40 µs | 16% |
  | 池宽 128 + 滑窗，**树槽/交付槽没分开** | 543 s | 17.2 | 523.1 s | 112 µs | 20% |

  拟合「调用耗时 = F + M×批宽」：**F ≈ 1.50 ms / M ≈ 24.7 µs/行**（B=17.2 → 1.93 ms；84.2 → 3.58 ms；
  109.8 → 4.30 ms，三点自洽）⇒ **B≈80 就已经过了拐点**（B→∞ 也只到 24.7 µs/行 = 40k 行/s，比现状
  只有 ~1.4× 余量）。而每行 25 µs 里 **~18 µs 与批宽无关**（GPU 只忙 16%、按行只需 6.6 µs）⇒ 那是
  torch/CPU 侧的按行开销（单个服务线程串行）⇒ **下一个杠杆是把「一次前向」本身变便宜**
  （`jit.freeze` / `optimize_for_inference`、减少按行算子/拷贝），**不是继续加宽批**。
  ⇒ **"批太窄"这个方向可以收工了**：结论是"在飞场数 ≥ ~100 就够"，之后加宽/攒批都是白费。
- **批宽 ≤ 在飞的场数**（一棵树同时只挂 1 个在飞请求，`TreeState::waiting`）⇒ 一次前向能凑到的行数
  上限 = "此刻有几场没跑完"。由此：**1 场 ≈ 决策点数 × sims 行**（一次前向里 1 场 = 1 行；累计行数是
  场数的 ~2300 倍：4,668,504 ÷ 2,016）。
- **两个会让池子塌到 1/3、慢 2.5× 的陷阱**（都踩过）：
  ① **树槽与交付槽必须分开**：合成一个环（槽 = 任务号 mod 池宽）时，**跑完但没交付**的场一直占着树槽
     —— 交付按任务序、队头撞上长局会落后上千场（实测落后峰值 993~1726 场）⇒ 在飞场数只剩 49（池宽
     128）。现在是 `S` 个树槽 + `D` 个交付槽（`D = n_task`）。
  ② **交付落后量要留够**：`claim()` 里一旦 `started - emitted` 顶到上限就**停开新场** ⇒ 同样塌。
     现在 `D = n_task`（落后只是内存：一场的帧 ≈ 70 KB ⇒ 整表 ~140 MB）。
- **交付落后是设计里的常态**（不是卡住）：交付**按任务序**（数据行序的契约），而在飞的场是并发跑的 ⇒
  **只要队头那场慢，游标就落后**（实测落后峰值 993~1726 场；池越深越明显）。落后只是内存（一场的帧
  ≈ 70 KB）+ 第一个分片出得晚（`shard_rows=200000` 要攒够才落盘 ⇒ 监控上会看到"完成"在动、"交付"久久
  不动）。⚠️ 别把它当成 hang：判据看 `自对弈（完成）x/y 场`。**收尾会一次性排空**（末尾都是已算完的）。
- **攒批（`batch_wait_us`）只在池子塌时有用**：那时等一等能把批从 17 拉到 60+（543→316 s）；池子正常时
  是**白等**（池宽 512 等 6 ms ⇒ 服务线程只剩 78% 忙、235 s vs 不等 202 s）⇒ 生产 0。
- **内存**：≈3~7.5 MB/棵 ⇒ 池宽 512 ≈ 1.5~4 GB（本机 503 GB 无压力）。
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

### 前向吞吐：**一次调用的耗时几乎与批宽无关** ⇒ 尾段（窄批）走 CPU（2026-10-16 实测；2026-10-20 起实现换成无旋钮版，见下）起因：第 1 轮采集 2411 s 里有 **约 1877 s** 花在最后 16 场的 ~160 万次 **1~2 行**调用上（GPU 40%、
14 条 worker 87% 在睡）。量完三段得到两条硬事实：

> **2026-10-20 现状（实现已换，下面保留的是当时的实测记录）**：`SelfplayConfig` 里
> `cpu_rows` / `cpu_sessions` / `CpuPlan` / `batch_target` / `batch_wait_us` / `threads` **全部删除**。
> 尾段**没有旋钮也没有开关**：`tail_mode_ = 已交付过至少一场 && 在飞场数 ≤ workers`，命中后 worker 用
> 自己的 CPU 会话（懒建、intra=1）就地前向；CPU 存档 = `model_path_cpu`，空则用主存档 + CPU。
> 前向服务**不攒批**（来一批发一批）。守卫：`apps/selfplay-test <net> 8 4 1 --tail-guard`
> （池宽 1 ⇒ 首次交付后由 CPU 会话接手，每场仍与逐场单跑逐位相同）。

| 批宽（行） | 1 | 4 | 16 | 64 | 256 |
|---|---|---|---|---|---|
| cuda 每次调用（ms） | **1.132** | 1.020 | 1.109 | 1.431 | 1.412 |
| cpu 每次调用（ms） | **0.403** | 0.805 | 2.705 | 11.079 | — |

- **每行成本 ∝ 1/批宽**：cuda 侧 1 行 1.13 ms vs 256 行 1.41 ms ⇒ 一次调用约 **200 次算子派发**，
  是**派发受限**（launch/kernel dispatch），不是算力受限。宽批时每行只要几 µs，窄批时每行贵 ~200×。
- **CUDA 加会话不 scale**：1/2/8 会话 ⇒ 每调用 1.132 / 0.805 / 0.934 ms（GPU 侧 launch 串行）；
  CPU 侧相反：1/2/4/8 会话 ⇒ **2201 / 3965 / 7164 / 10083 行每秒**（会话之间真并行）。
- **交叉点 ≈ 批 6**（单 CPU 会话），4 会话下 ≈ **10 行**。⇒ 门槛取 `cpu_rows=10`。
- 顺带排掉的嫌疑：**Debug/Release 不是瓶颈** —— 同任务表 63 场、同权重，`build`（Debug）25 s /
  `build-onnx`（Release）25 s，其中 ORT 前向 23.9 / 23.1 s、我们自己的 gather+scatter 只占 0.4 s。

**实现（2026-10-16 定案：只有一种方式，不留开关）**：
· **形状** = worker 在"没树可推"时**就地转前向**（用它自己的 CPU 会话，懒建）。不再保留"专职 CPU 服务
  线程"那条路 —— 实测两者等价（线程转到前向之后就与树无关了），而且专职那条要多一次"够不够格"的
  猜测。**触发** = **任务表被领干净**（`started == n_task`，不再有新场）之后才准接手：该判据**单调**
  ⇒ 宽批阶段的瞬时抖动不会误触发（"某条线程暂时没树可推"那种判据会反复跳）。
· 门槛只有一个旋钮 `cpu_rows`（待服 ≤ 它才归 CPU）；**策略只在 `SelfplayConfig::cpuPlan()` 一处解析**
  成 `CpuPlan`，热循环只问两个问题：`cpuAllowed()`（门控）× `narrowReady()`（有没有窄批）。
  ⚠️ 从前这里是"3 个布尔 + 4 个 lambda 横跨 `NNEval` 与 worker 循环"，改一次门控要动五处。
· ⚠️ 一条线程要同时等"树可推"与"有窄批可做"⇒ **两件事共用池子那把锁与 cv**（两把锁时"检查 + 睡下"
  不是原子的：worker 查完空、正要睡，`push` 插入并 notify 却没有等待者 ⇒ notify 丢掉、那条活没人接）。
· ⚠️ **判据必须每次醒来重算（lambda），不能是 `const bool`**：2026-10-16 线上死锁的真因 —— 主设备线程
  算完 `narrow=true` 才睡下，醒来谓词还读那份**过期**值 ⇒ 队列涨宽（> `rows` 行）它也不接手，而 worker
  侧又因"不是窄批"不接手 ⇒ **两侧都不够格、谁也醒不来**（33 条线程全 S、GPU 0%、13 min 零进展）。
  ⚠️ 触发条件是"**`cpu_rows` 低于同时在飞的行数**"——守卫如果一律用 `--cpu-rows 10`（> K 棵树）就
  **永远跨不过这条边界**（我最初就是这么漏掉的）。回归项：`--cpu-rows 2/3/5` 配 K=8 必须跑完。
· 守卫：`apps/selfplay-test` 的 `--cpu-rows N`（K=1 断言"与不分流读数逐位相同"）；`cpu_rows=0` = 关
  ⇒ **与改造前逐位相同**（在役基线守卫就是这么跑的）。
**整轮实测（2016 场 / sims 400+3000 / pool 512 / workers 14 / cuda / 同一起点网）**：

| | R2 分流**关** | R3 分流**开**（CPU×4 ≤10 行） |
|---|---|---|
| 行数 / 调用数 | 44.1M / **1.055M** | 42.4M / 2.090M（GPU 283k + CPU 1.807M） |
| 均批（最大） | 41.8（510） | 20.3（512） |
| ORT 前向（GPU） | 1085 s | **458 s** |
| 本层调用累计 | 1088 s | 2192 s（CPU 侧 1734 s，4 会话并行） |
| 尾段 CPU 分到 | — | 1.807M 行 / 1.807M 次（**全 1 行/次**） |
| worker 空转 | 13403 s | 8443 s |
| 宽批阶段（到 2000 场） | **364 s** | 541 s（慢 49%） |
| 尾段（最后 16 场） | ~802 s | **~497 s**（快 38%） |
| **整轮墙钟** | 1166 s | **1038 s（−11%）** |

⚠️ 关键口径：**GPU 的代价是"每次调用"而不是"每行"**（R2：1.055M 次 / 1085 s ≈ 1.03 ms 一次，与批宽
几乎无关）⇒ 分流真正省下的是**约 77.5 万次 GPU 调用**（≈800 s 的 GPU 时间），代价是 CPU 侧 ~434 s。

**2026-10-18 westd 复测（63 场 / sims=100 / pool 64 / workers 12 / cgroup 12 核 / RTX 4080S）**：

| `cpu_rows` | 墙钟 | CPU 行 | 备注 |
|---|---|---|---|
| 0（关） | 13 s | 0 | 全 GPU |
| 4 | 9 s | 27,614 | 会话不足 |
| 6 | 7 s | 37,258 | ≈ 半核，和 10 打平 |
| 10（生产） | **7 s** | 45,106 | 当前在役 |
| 12 | 8 s | 40,788 | 多会话争核，反而慢 |

- 同期 CPU 单会话（同一份 `-cpu.onnx`）batch=1：`intra=1/4/8/12` ⇒ 464/921/1067/1037 行/s；
  **12 个独立会话**（intra=1，batch=1）⇒ **5252 行/s**。batch=10 时 intra=8 只有 2659 行/s；
  batch=256 时 intra=12 才到 4968 行/s。⇒ **1 行尾段靠会话间并行，不是靠单会话多核**；
  "撤一半核给一个 CPU 会话"在这个形状下会更慢。
- ⚠️ 共享 `Ort::Env` 的坑（2026-10-18 实测）：CUDA EP 注册会取 ORT 的默认 logger，而 logger 是
  `Ort::Env` 构造时注册的 ⇒ 必须在 `AppendExecutionProvider_CUDA_V2` **之前**先构造共享 Env；
  否则报 `Attempt to use DefaultLogger but none has been registered`。修法：构造函数开头
  `Ort::Env &env = sharedOrtEnv();`，再用这个 env 建 Session。

### 交付环 / 线程收尾：7 条（2026-10-16 审出的调度与异常收尾问题，全部实测复现 + 修掉）

审出 7 条，逐条在**当前代码**上验过（都成立），其中 3 条有可复现的 D 版守卫：

1. **[P1] 交付环满 ⇒ 补场没有重试 ⇒ 池子永久停摆**（`selfplayPooled`）：`retire()` 里那次领号会因
   `started - emitted >= D` 被拒，**拒绝之后没有任何重试点**；若所有树槽都在那一刻跑空，池子就停在
   那里不动。复现：12 个任务、池宽 1（`D = 8`）、把**首次交付**放慢 1.5 s ⇒ **停在 9/12**
   （与用户报告逐字一致）。修法：交付是唯一让落后量下降的动作，而交付只有主线程做 ⇒ **主线程交付后
   补一次空槽**（语义单一，不新增唤醒源）。
   ⚠️ 生产上 **`D = min(n_task, 8S)` = 2016 = 任务数** ⇒ 这条**踩不到**（R9 的落后峰值 1676，
   差 340 才到顶）。但按"能发生就必须堵住"修，并且它是"池宽小 + 交付慢"这类测试形状的必踩项。
   ⚠️ 修的时候我自己**引入了一个新竞态**：补场有了两个触发源（`retire` 腾槽、主线程交付后），
   "看哪些槽空着"与"在哪个槽上开局"之间出现窗口 ⇒ 两个触发源抢同一个槽、**同槽开两场**，其中一场
   的结果永远不落交付槽（实测：池宽 1、63 场 ⇒ 报"交付槽被覆盖"，另有一次直接 `0xC0000005`）。
   ⇒ 领号必须**在锁内占位**（`st[i].starting`），`start_task` 里清掉。教训：**补场这种"看状态 +
   改状态"的动作，哪怕只有一个锁，也不能分两次进入临界区**。
2. **[P1] `emit()` 抛异常 ⇒ 进程 terminate**：交付循环里直接 `emit(...)`，异常往外抛时局部
   `std::vector<std::thread>` 先析构 —— 还 `joinable` 的线程析构 = `std::terminate`（实测：注入写盘
   异常后**外层连 catch 都来不及**，进程 0xC0000409）。修法：交付循环整体 try/catch ⇒ 记 `err` +
   停池子 + 等所有线程收工 + 在收尾处重抛（对外仍是响亮失败，但不是 terminate）。
3. **[P1] 前向服务线程未捕获异常**：`serve()` 里 ORT 失败 / `bad_alloc` / 回调抛都直接逃出线程入口
   ⇒ terminate，且 worker 还睡在"等批回投"上、连报告的机会都没有。修法：`serve()` 只做 try/catch，
   循环挪进 `serveLoop()`；接住后 `err_ = current_exception()` + `stop_ = true` + `notify_all` +
   `on_fail_()`（池子记 `err`、停池子、叫醒所有等待者）。
4. **[P1] 前向线程可能访问已析构对象**：`reqs`/`ready`/`st` 与那几个计数器都**声明在 `NNEval` 之后**
   ⇒ 析构序在它之前，而收尾只 `join` worker、前向线程要等 `~NNEval` 才停。修法：加
   `NNEval::shutdown()`（幂等：置 `stop_` + notify + join + 清空），**在 join worker 之后、打诊断/
   退出之前**显式调；`~NNEval` 仍调它兜底。
5. **[P1] 诊断代码的数据竞争**：`step()` 里无锁读 `ready.size()`、`pendingSize()` 读 `pending_`
   （两者都在 `m` 下改），`retire()` 里无锁读别的 worker 正在改的 `retired`。诊断也一样是 UB。
   修法：`pending_` 加**原子镜像** `pending_n_`（所有改动点同步维护）；`ready` 直接用**已有的镜像**
   `rows_ready`；进度行改用**锁内快照** `done_n`。
6. **[P2] CPU 分流没验输出维度**：CPU 会话建好后按主会话的头宽 `assign` 它的输出 ⇒ 两份模型宽度不同
   就是**越界读**（更大）或静默截断（更小）。修法：首次前向后比 `p_act_dim`/`p_sel_dim`，不一致当场
   抛（`model_path_cpu` 是给人手改的旋钮）。
7. **[P2] `n_per=0` 越界**：空任务表仍去读 `jobs[size-2]`/`jobs.back()`（= `jobs[-2]`）。修法：入口
   `n_per <= 0` 直接抛（"一场没采"静默返回空列表比报错更难查）。实测 `n_per=0/-3` 都响亮报错。

**守卫（`apps/selfplay-test`，都要能证明有牙）**：

```bash
selfplay-test <存.onnx> --stall-guard       # 交付环满 3 相（慢首次交付 / K>D 常态 / 池宽 3）
selfplay-test <存.onnx> --emit-throw-guard  # 交付抛异常必须可捕获
```
- 两条都做过**反向验证**（把修法临时摘掉再跑）：`--stall-guard` 在旧行为下**停在 9/12 + 看门狗 60 s
  报错**；`--emit-throw-guard` 在旧行为下**直接 0xC0000409、无任何 catch 输出**。
  ⚠️ `--stall-guard` 最初只跑了"慢首次交付"那一相（`worker` 当时是睡着的，**碰不到并发补场那条
  路**）⇒ 我引入的同槽双开竞态它没抓到。补了"K > D 常态"两相才压到 —— **守卫的相要覆盖"并发"**，
  不是只覆盖"现象"。
- 端到端等价性（`selfplay_run`，同 seed、`n=2`、126 场）：`pool = 1 / 2 / 8 / 16` × 有无尾段分流，
  四个分片 **SHA256 完全相同**（`C4D3CC79B9013D20…`）⇒ 这些调度改动**一个字节都不改数据**。
- `n_per=0` 守卫：`selfplay_run(0, …)` / `(-3, …)` 都抛 `n_per 必须 ≥ 1`。
- 第 6 条（宽度校验）也做了**反向验证**（把判据临时反判 ⇒ 必抛）：确认它**不是死代码**，而且异常
  从 worker 线程一路传到 Python（`RuntimeError: CPU 会话与主会话头宽度不一致（act 96/96…）`）——
  顺带把"worker 抛异常 ⇒ 记 `err` ⇒ 主线程收尾重抛"这条通路也压了一遍（无死锁、无 terminate）。
  ⚠️ 抛给 Python 的消息**别用 `⇒` 这类字符**：本机控制台是 GBK，`print(repr(e))` 会
  `UnicodeEncodeError`（产品侧没事，但排查时会被这层噪声骗）。

### 攒批：判据改成"还能不能再推一张票"之后，**结论仍然是不等**（2026-10-16 生产形状三臂实测）
判据（在役实现 = `NNEval::wait_more_`）= **`ready` 里可推进的树 + 正握着树的 worker**：还有就等，
一条都没有就发车（"全池都停在等前向"就该发车）。`batch_wait_us` 降级成上限兜底。
三臂同种子同网（2016 场 / `--sims 100` / pool 512 / workers 14 / cuda / 尾段 CPU×4 ≤10 行）：

| 臂 | 策略 | 墙钟 | 调用数 | **GPU 调用** | **GPU 侧均批** | ORT 前向 |
|---|---|---|---|---|---|---|
| A | **不等**（在役） | **65 s** | 118,698 | 15,649 | **190 行** | 21.2 s |
| B | 等 + 上限 128 | 73 s | 80,352 | 24,957 | 121 行 | 30.2 s |
| C | 等 + 不设限（排空全池） | 102 s | 110,249 | 13,611 | 219 行 | 36.2 s |

- ⚠️ **别被"均批 23.9/25.9"骗了**：那是**全部调用**的均值，其中 83~87% 是 **CPU 会话**的调用（刻意
  1 行/次）⇒ 它把均值压下来了。**GPU 那一侧早就是 190 行/批**（≤10 行的窄批已被尾段分流吸走）
  ⇒ "窄批"在 GPU 上**已经不是问题**，攒批没有可捞的余地。
- 等的代价是**把窄批从 CPU 抢回 GPU**（B 的 GPU 调用 15.6k→25.0k）＋延迟；完全排空（C）更是把两级
  **串行**（前向时全池摸鱼）⇒ 102 s 最差。这是一次"机制想清楚了、实测否掉"的典型：
  `batch_wait_us` / `batch_target` 生产值都是 **0**，别开。
- 下一步的真正瓶颈：**GPU 侧 283k 次调用 × ~1.6 ms = 452 s**（一次调用的固定开销，批已 190 行）＋
  CPU 侧 1.4M 次 × ~0.9 ms（4 会话并行）。要再快只能动"每行摊到的调用数"或"CPU 会话数"，
  不是动发车时机。

### 推理引擎（ONNX Runtime vs libtorch）——**端到端受控 A/B + 微基准**（历史测量，libtorch 已删）
⚠️ 下表是 **2026-10-14 的过渡期对账**，那时两条路都编得出来。现在只剩 ONNX（§5-21），
`build-torch` / `build-onnx-dbg` 两个目录已删，`_bench.sh` 的臂也改成了 `rel`（`build-onnx`）/
`dbg`（`build`）。数字留着只为"为什么要换"提供依据。
**端到端**（`_bench.sh`；同一任务表 2016 场 / sims=200 / pool=512 / workers=14 / cuda、同权重；
**树分布逐项相同**：均在飞 349 / 可推 105 / 待服 112、峰值在飞 511、交付落后峰 1414）：

| 臂 | 墙钟 | `服务调用`（前向累计） | 均批 |
|---|---|---|---|
| `torch-dbg`（= **现役生产** `build/`） | 236 s | 220.4 s | 75.2 |
| `torch-rel`（`build-torch`） | 266 s | 249.0 s | 75.2 |
| **`ort-rel`（`build-onnx`）** | **76 / 79 s** | **64.7 / 66.6 s** | 75.2 |
| `ort-dbg`（`build-onnx-dbg`） | 77 / 86 s | 65.0 / 72.8 s | 75.2 |

- **端到端 ORT ≈ 3× 快**（≈250 s → ≈78 s）。⚠️ 第一次测的 `ort-rel` 是 114 s（**离群**）——
  单次测量在这个形状上噪声能到 ±40%（早先那批探针也从 202 到 316 s 都出现过）⇒ **每次臂至少 2 遍交错**。
- ⚠️ **微基准会低估 torch 的代价**：`net-test` 恒温循环里 torch-cuda 只比 ORT 慢 1.8×（2.77 vs 1.53 ms
  @n=64），但真管线里是 **3.6×**（3.83 vs 1.05 ms/次 @均 75 行）。怀疑是**批形状每次都在变**
  （均 75、最大 510）而 `setGraphExecutorOptimize(false)` 让 torch 反复重规划 —— 没证，但说明
  "微基准 → 生产"这一步必须实测端到端。
- **Debug/Release 在这一层不是杠杆**（ORT: 77 vs 76~86 s）⇒ 不必为了性能去翻生产构建类型
  （⚠️ 而 Release 会把 `sts_asserts` 那批自检预处理掉，翻它得单独权衡）。
- ORT 臂的 `worker 等活` 从 2800~3100 s 掉到 717~838 s ⇒ **C++/CPU 侧开始变成下一道瓶颈**
  （前向只剩 ~16% 墙钟）。

**微基准**（`net-test <n> 8 <dev>`，Release、`intra_op=1`、同权重）：

| n（批宽） | torch-cuda | **ORT-cuda** | ORT-cuda +TF32 | torch-cpu | ORT-cpu |
|---|---|---|---|---|---|
| 64  | 2.77 | **1.53** | 1.54 | 15.7 | 19.6 |
| 128 | 2.84 | **1.68** | 1.70 | — | — |
| 256 | 3.15 | **2.03** | 1.98 | 124.0 | 84.6 |
| 512 | 3.86 | **3.48** | 3.43 | 261.5 | 181.1 |

- **TF32 在这张网上没有收益**（1.53 vs 1.54）—— 网络**访存受限**（嵌入 + 逐槽 MLP），不是 matmul
  受限 ⇒ 没必要为它牺牲数值一致性。默认 `allow_tf32=false`（两个后端同名开关）。
- **数值一致性**（同权重、同合成输入）：CUDA 上 torch vs ORT `max|Δ| = 9.5e-7`、**argmax 128/128
  逐行一致**；`onnxnet.verify()`（Python 侧）在 cpu/cuda 都过（CUDA 档 3.8e-6）。
  ⚠️ ORT 的 CUDA EP **默认开 TF32** ⇒ 不显式关掉就与 torch(FP32) 差 **4.29e-3**（实测）。
- **两个实现细节**（都踩过）：① EP 必须走 `OrtCUDAProviderOptionsV2`（`Ort::CUDAProviderOptions` +
  `Update({{"device_id",..},{"use_tf32","0"}})` + `*opts` 解引用）—— 通用
  `AppendExecutionProvider("CUDA", {..})` 在 1.31 里**不认 "CUDA"**（只收插件式 EP，实测报
  `Unknown provider name 'CUDA'`）；② 头宽度要从**首次前向的实际输出**读（`factor` 策略头导出后
  那一维是动态的，元数据给 `-1`）。

### 网络与训练默认（`aznet.net_cfg()` / `train()` 签名，实跑打印）
`net_cfg() = {hidden 256, depth 4, arch factor, sel_arch factor, dropout 0.1, card_rank 32, monster_rank 16,
intent_move_rank 16, relic_rank 16, potion_rank 16, init_std 0.02}`。
`train()` 默认：`lr 1e-3, wd 1e-4, epochs 64, batch 256, z_spec hp, soft_target False, soft_temp 1.0,
pi_weight none, pi_objective off, loss_chunk 65536, threads 1, device cpu, record True`。默认值**只在签名一份**。

- 建网超参（`hidden/depth/arch/dropout`）**不进配方名**（名字里本来就不带配方，见 §2-18；建网配置在存档的 `config` 里），
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
   ⚠️ **2026-10-15 部分回役**：多轮**自对弈**产物现在按权重混训（`collect.CollectMix` +
   `train.py --mix` + `chain.sh REPLAY_ROUNDS/REPLAY_DECAY`，见 §3）；**教师锚点仍然没有**（要挂
   得自己在 `--mix` 里给路径）。⚠️ 收益仍未验证（要 A/B 门控），别把"接线了"当"有效"。
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
18. **「拉大批数 / 攒批 / 限批宽」都不能把批撑宽**（2026-10-14 实测，同一批任务、同 sims=200）：
    攒批（`target=1024`+5ms 超时）均批 84.2→85.9、墙钟 211→237s；限批宽（`target=256`）均批 73.6、更慢；
    拉大 `--games`（旧语义=任务数）只改块大小。**根因**：批宽 ≤ **在飞的场数**，而整表一次性全开时
    在飞场数**单调衰减**（短局几秒跑完就走）。**现在是**：`selfplayPooled` 滑窗补场（池宽 = `--pool`）。
    旧注释里"全拿走 ⇒ 批宽往下棘轮到 84"的说法**是错的**（84 是当时的在飞场数，不是取批策略的结果）。
19. **`SelfplayConfig::games` 已改名 `pool`**（`collect_selfplay --games` → `--pool`；`chain.sh`
    `GAMES=2016` → `SP_POOL=512` + 门控自己的 `GATE_GAMES=512`）：旧名字同时被当成"在飞树数"和
    "一轮块大小"两件事用，正是"批太窄"的认知陷阱。⚠️ 换名 = 改产物名（旧缓存不复用）。
    ⚠️ 2026-10-15 之后产物名固定（§2-18）⇒ **换配方不再改名**，由 `_recipe.json` 当场挡。
20. **virtual loss 已删**（2026-10-14，`collect/selfplay.cpp`）：树内恒 1 条路径 ⇒ 标记机制本身是死的，
    但它**唯一活着的副作用**污染了探索系数 —— `sq = sqrt(N + nv)` 在 d≥1 层因刚被标记而变成 `sqrt(N+1)`，
    而根从不被标记（`sqrt(N)`）⇒ **根与深层口径不一致**（"树内批 MCTS"时代的残留：那时一批 256 条路径
    真在飞，`N+nv` 才算对）。现在全深度统一为教科书 AZ 的 `u = q + c·P·sqrt(N_parent)/(1+N_child)`。
    ⚠️ 这是**搜索口径变更**（深层探索项变小：`sqrt(1+1/N)−1` ⇒ N=1 −29%、N=4 −11%、N=16 −3%）：
    实测（同权重、本地）`--sweep 24` **250 → 256 帧**、K=1/K=8/K=4 帧数不变、池宽 1 的"与逐场单跑
    逐位相同"仍成立。**生产 sims=3000 下的强度影响未测**（要 A/B 门控才知道），且它**不打红任何
    在役断言**（见 §2.10）。删掉的量：`kVirtualLoss` 常量 + `Node::nv` 字段 + 一处标记 + 两处撤销
    + `sq/tot/q` 三处的 `nv` 项。
    ⚠️ **不能靠调 `c_puct` "补回来"**：旧公式等价于深层把探索项乘 `sqrt((N+1)/N)`（N=1 +41%、N=4 +12%、
    N=16 +3%）—— **随 N 变**，任何单一 `c_puct` 都复原不了这个形状（它是形状变化，不是尺度变化）；
    而 `c_puct` 在本仓是"胜率—省血"旋钮（§5-7：四档胜率全平、战损差 10pp）⇒ 拿它补偿等于顺带改战损口径。
    **也没有别的旋钮在数学上与之耦合**：`q_init` 只在 `N=0` 时用（不变）、`z_spec`/`Z_SCALE` 是值尺度
    （不变）、Dirichlet 只作用于根（不变）。
    ⚠️ 幅度与 **sims** 强相关：`−1/(2N)` 的效应在**低预算**下最凶（`--sweep 24` 时深层 N≈1 ⇒ −29%，
    所以 250→256 帧这个数**不代表生产**）；sims=3000 时同层 N 大得多 ⇒ 每节点只有几个百分点 —— 但
    决策序列很长，误差会累积，"每节点小"**不等于**"整轮数据没差"。
21. **libtorch 已从 C++ 侧撤掉，ONNX Runtime 是唯一引擎**（2026-10-14 起过渡，2026-10-15 收工）：
   动机不是速度而是**甩掉那条依赖链** —— `.pyd` 静态链 `c10.dll`/`torch_cpu.dll` ⇒ 进程必须先
   `import torch`、torch 自带一份 pybind11 与子模块抢、构建要 `TORCH_CMAKE_PREFIX`、可执行文件还要
   `Python.h`+`python3XX.dll`。**现在是**：`CMakeLists.txt` 里只有 ONNX 一条路（`STS_ONNX` 开关已删）、
   `bindings/onnx_eval.*` 是唯一实现、`netarchive.py` 只转发 `onnxnet`。
   实测导入表（本地 MSVC 构建，`dumpbin /dependents`）：`_slaythespire.pyd` = `python314.dll` +
   `onnxruntime.dll`；**`selfplay-test.exe` / `net-test.exe` = 只有 `onnxruntime.dll`**（连 Python 都不需要了
   —— `collect/selfplay.h` 不碰 pybind11，绑定层单独一个 TU）。远端 `ldd` 同样只剩 `libonnxruntime.so.1`。
   ⚠️ 顺带删掉的：`_STS_PY_INC`/`_STS_PY_LIB`（selfplay-test 的 Python 头/库）、`_STS_NET_CRT`（死变量）、
   包 `__init__.py` 里那段"先 `import torch`"的 shim、`tsnet.py`、`data/selfplay_ts/`。
   ⚠️ 数值**不可能逐位跨实现相同**：撤之前测过同权重 max|Δ| ≈ 3e-7、两个头 argmax 逐行一致、
   同权重同 sims 的**帧数完全一致**（K=1/4/8 = 12/66/114、`--sweep 24` 256 帧）—— 旧实现已删，
   要再对账从 git 历史取。
22. **「根噪声只在新建树根注入」是 bug**（2026-10-15 修，`collect/selfplay.cpp`）：`expandBackup` 里那句
    `node == &t.root` 只在**展开**时命中，而子树提升为新根（`stepAndAdvance` 把 `edges[pick].node`
    搬成 `t.root`）时**不混** —— 它的孩子们是上一轮当**内部节点**展开的（那时不是根）⇒ 每场**只有
    第一个决策点**有根噪声，之后全靠温度/采样探索（`DIR_EPS=0.1` 生产档实际只有 1/13 的决策点带上）。
    **现在是**：抽出 `mixRootNoise(t, cfg)`，展开与提升各调一次（同一个根只混一次：提升过的根其孩子
    已非空，展开那条路的 `edges.empty()` 不再命中；`dirichlet_eps<=0` 或只有一个孩子时**不动 rng**）。
    实测（`--n 1 --sims 8 --pool 1 --workers 1`，同一 seed，`sha256(分片)` 为判据）：`--dirichlet-eps 0.1`
    **改前 f88943c0 / 改后 53a40ccf** ⇒ 行为确实变了；而 `--dirichlet-eps 0` 两侧**同为 eadff347**（关着时逐位不变）。
23. **ε 分支被后续采样/argmax 覆盖 = ε 完全无效**（2026-10-15 修）：`stepAndAdvance` 里 ε 抽中一条
    **树内**的边之后，代码继续往下走"采样 / argmax"两段 ⇒ `pick` 被覆盖。实测判据很干净：
    **`--eps 0.5` 与 `--eps 0` 的分片逐字节相同**（旧代码 eadff347 == eadff347）—— 因为
    `legal ⊆ root.edges` 恒成立（根总是展开**全部**合法动作）⇒ "抽到树外动作"那条路永远不走，
    ε 等于白抽。**现在是**：抽中就直接执行（`eps_pick` 短路），`--eps 0.5` = **bd6753d7** ≠ 关着。
    ⚠️ 生产 `eps=0` ⇒ 这条**不解释任何已有的平台期**（只是把旋钮修好）。
24. **随机化预算（KataGo 的 randomized playout cap）**（2026-10-15 接线，2026-10-20 改成比例口径）：
    `SelfplayConfig::sims_cheap_frac` / `full_frac`（`collect_selfplay --sims-cheap-frac/--sims-full-frac`、
    `chain.sh SIMS_CHEAP_FRAC/SIMS_FULL_FRAC`）：便宜档预算 = `max(2, round(sims × frac))`，
    多数决策点跑便宜档、少数（`full_frac`）跑完整档 ⇒ 同墙钟下"量大 + 少量深标签"。
    ⚠️ 用**比例**而不是绝对 sims：绝对数会在 `sims` 变小时反超（`sims_cheap > sims`），而且每个
    调用方都得重算一遍；比例只在 `[0,1)` 上校验一次。`sims=4` 时算出来 ≤ 2 ⇒ 与完整档没区别，
    此时**不抽签、不动 rng 流**（关着 = 逐位同旧行为：实测 `--sims-cheap 3 --sims-full-frac 0.5`
    = a4822011 ≠ 关着的 eadff347）。⚠️ 强度收益**未测**（要 A/B 门控）；原文 arXiv:1902.10565。
25. **读侧跟上 v3 落盘**（2026-10-15，修 §6-1 记的那个堵点）：`collect_io.VERSION` 2 → **3**，
    `zspec.Terminal`/`Z_COLS` 去掉已不落盘的 `t_escore`（⇒ **`enginenorm` 只有采集/搜索侧能用**
    （C++ 现算），读侧 `z_of("enginenorm")` **响亮拒绝**）。v2→v3 的列改型（`t_win` `i8`→`|b1`、
    `t_hp/t_max_hp/t_turn` `i8`→`i4`、删 `t_enemy`/`t_escore` 改存 `t_enemy_cur_hp`/`t_enemy_max_hp`）
    见 `collect_io.py` 文件头的表。⚠️ **代价**：`data/` 里 2026-10-14 那批 v2 产物（`it1_1014_0810_…`，
    331 MB）**读不了**了（要就写个一次性转换器）；那批本来也是旧搜索口径（没有 §5-22/23 的两条修正）。

## 6. 已知未解 / 待办 / 上限

1. **ONNX 后端：剩下"生产形状的收尾"**（2026-10-15 更新）：
   已验 —— 远端 Linux 构建、cu13 GPU wheel 路径（pip 走 aliyun 镜像 + 本地头文件，见 `_ort.sh wheel`）、
   CUDA EP 生效且 `use_tf32=0` 与 torch 数值一致（9.5e-7 / argmax 全同）、**端到端 ≈3× 快**（`_bench.sh`，
   见 §3）、全套 `selfplay-test` 守卫（本地 + 远端帧数逐项相同）、**libtorch 已从 C++ 侧撤掉**（§5-21）。
   **未做** —— ① `chain.sh` 与 `_start.sh` 里加"链哪个构建目录"的开关（现在 `build-{Debug}` /
   `build-onnx` 靠 `PYTHONPATH` 手选，而 `.pth` 钉在 `build/`）；② 真一轮（sims=3000）的墙钟确认
   （按微基准外推：5546 s → ~1800 s 量级）。
   ⚠️ 仍未定口径：`enginenorm` 在读侧没有列可算（`z_of` 响亮拒绝）；`t_enemy_cur_hp`/`t_enemy_max_hp`
   还没被读侧用起来（要拿去当值口径得先定怎么归一）。
2. **`shard_rows=200000` ≈1.5 GB 写缓冲**（§2.16）：要降就传更小的 `shard_rows`，或让 `ShardWriter::flush` 增量追加。
2. **`CARD_SELECT` 候选块只覆盖 21 个任务的一部分**（其余落 `default:` ⇒ `candidateCount=0`）；真修法（候选
   multi-hot + `confirm`）未做。
3. **`walk/scene/rlenv` 三处难度口径不一致**（曾经 `ASCENSION=0/20/0`）⇒ 只留 `scene.ascension_of(seed)` 后仍需确认调用方是否统一。
4. **A20 牌组曲线只有终局一个锚点**，段间形状是等比缩放（非实测）。
5. **值口径/根噪声的 RNG 未换成能复现 Python 的流**（§2.12）⇒ 探索档与旧数据不可比。
6. **`selfplay-test --sweep` 的帧数与历史读数**：随网络存档与代码版本变（2026-10-14：删 virtual loss 前
   12 例/250 帧、删后 **256 帧**）—— 它断言的是**自洽**（π 归一/动作合法/两次跑逐位同），不是某个固定
   帧数，别把帧数当 golden。
7. **接力脚本 `chain.sh` 已删、名字被自迭代循环接管**（2026-10-16）：从前 `chain.sh` 是旧分发器的
   "接力器"（`./chain.sh <等谁完成> <下一棒>`，全仓零引用），自迭代循环叫 `chain.sh`；现在**只留一个**
   `chain.sh` = 自迭代循环（**无参数**，旋钮是裸常量）。旧接力用法不再存在（`_sync.sh` /
   `_wait_logs.sh` 那套见 §7 的历史笔记）。
8. **两个孤儿 golden**：`collect/tests/golden_selfplay.txt` / `golden_teacher.txt` 没有读侧（生成它们的
   `_chk_selfplay.py` 等已删）⇒ 留着只会让人以为"还在对拍"。要么删掉，要么把读侧补回到 C++ 测试里
   （`apps/selfplay-test.cpp` 目前**没有** `--golden` 模式）。
9. **未做的架构探索**：(a) 前向搬进 C++ 的收益上限 1.33~1.70×（§3）；(b) 要 4~5× 得把 PUCT 树一起搬（另一量级工作）；
   (c) `--infer server` 之类后端已删，别按旧文档找。
10. **GPU 训练支线**：跨设备数字与 CPU **不逐位可比**，必须当独立队列标注。

11. **`teacher.cpp` 里两处死代码已清**（2026-10-17）：① `pickTeacherAction` 的 `exploring` 形参（与
    `cfg.eps > 0` 同义）+ `runTeacherGame` 里那个同名局部量；② `runTeacherGame` 的 `dim`（算完没人用）。
    净效果**逐字节等价**，只多消耗一个 rng 抽样（实测 `sims=2 eps=0.25` 的 63 场分片 sha256 改前改后相同）。
12. **`teacher.cpp` 的 `++emitted` 在锁外**（写入器取走结果后 `emit(...)` / `++emitted` 都在 `mutex` 外，
    而 worker 在锁里读它做背压判定）⇒ 严格按内存模型是数据竞争（读代码推的，**没实测、没跑 TSan**）。
13. **teacher 侧仍无端到端守卫**：`teacherRun` 的交付环 + 落盘没人跑（§6.8 的孤儿 golden 仍在）；本次
    只把 `ShardSink` 本身与 `eval` 的 `n_per` 断言落进 `apps/selfplay-failure-test.cpp`。
    ⚠️ 该目标现在**也编 `collect/teacher.cpp`**（只为拿到它的编译器检查，日志里已注销这点）——
    教训：`collect/teacher.cpp` 原先只被 `slaythespire` 目标编（全仓唯一），它自己的告警只在全量构建
    时才看得见，漏一次就是**整个 teacher 侧没有编译器检查**（实测逮到的就是这类：`exploring` 形参）。

## 7. 环境 / 运维

- **venv 加载的是 `build/Debug` 那一份模块**（`.venv/Lib/site-packages/sts_lightspeed_build.pth` 里写着
  `../../../build/Debug`）⇒ 只重建 Release（跑 `apps/*-test.cpp` 用）**不会**影响 `import slaythespire`；
  要 Python 侧见到 C++ 改动得 `cmake --build build --config Debug --target slaythespire`。
- 远端（现役 2026-10-14）：`root@connect.westc.seetacloud.com:38825`（`_*.sh` 头部默认值），
  `RDIR=/root/autodl-tmp/sts_lightspeed`，`PY=/root/miniconda3/bin/python`，cgroup **16 核**（`nproc=128` 是宿主机假象），
  RTX 4080S、torch 2.12.1+cu130、cmake 3.22.1、gcc 11.4（**无 ninja**）。完整四步见 `.agent/REMOTE.md`。
  两条 `.agent/REMOTE.md` 里还没有的：
  1. `conda install libstdcxx-ng`（11.2.0 缺 `GLIBCXX_3.4.30` ⇒ `import slaythespire` 报错），升到 15.x 解决。
  2. 起步权重放 `data/warm.pt`（= `chain.sh` 默认 `NET`，3.1 MB）；新实例上先 `collect_teacher → train` 蒸馏一个再拷成这名字。
  另：`bindings/slaythespire_pkg` 必须在推送清单里（`file(GENERATE INPUT …)` 配置阶段就要读它）。
- **产物落 `data/`，不写 `/tmp`**（远端重启即丢）；每轮的分片目录与权重都留着，磁盘不自动回收。
- **CUDA 红线**：上下文不能在 fork 的子进程里用；并行开在「任务」层、不要开在 BLAS 里；采集单进程。
- **换行符**：远端要执行的 `.sh` **必须 LF**（`.gitattributes` 已钉 `*.sh text eol=lf`）；Windows 侧写文件用
  `[IO.File]::WriteAllText(..., $s -replace "`r`n","`n")`；`Get-Content -Raw | ssh` 会把 CRLF 带过去打坏远端 bash。
- **ssh/scp 坑**：`scp` 目标目录必须先存在；`ssh -tt` 正常收尾也打 `Connection to … closed.`；杀作业按 pid
  （`pkill -f` 会自杀、先 `pgrep -af`，自匹配用 `[c]ollect_...` 断字面量）。
- **本地跑测试**：见 §2.15（PATH 里补 torch/lib 与 python DLL 目录，否则 `0xC0000135`）。
- **`bash _push.sh` 报 `Host key verification failed`** 而 PowerShell 里 `ssh` 正常：git-bash 的 `HOME=/home/bob`
  ⇒ 它读的是**另一份** `~/.ssh/known_hosts`（`/home/bob/.ssh/known_hosts`），新端口只是**没有条目**（不是 key 变了）。
  补法：`bash -c 'ssh -o StrictHostKeyChecking=accept-new -p <port> root@<host> true'`。
- **调试打印必须 `fflush(stderr)`**：stderr 在**管道**里是全缓冲 ⇒ 进程崩掉时最近几行会整段丢失，
  据此判断崩溃点会判错（今天就错了一次）。要么 `fflush`，要么别用打印而用 `gdb`/dump。
- **崩溃点"会移动"≠ 竞态**：先怀疑**越界把 garbage 当数据用**。2026-10-14 那次是我的 `selfplayPooled` 里
  多留了一个开局循环 ⇒ `start_task` 被调 2S 次 ⇒ `make_task(j ≥ n_task)` 越过任务表（`std::vector` 越界读一个
  `BattleContext`）⇒ 几十毫秒后 0xC0000005，且崩在哪一段随内存内容变（Debug/Release 都崩、换池宽也崩）。
  ⇒ `start_task` 现在有硬守卫（`started >= n_task` 当场抛），这类"越界 + 迟到崩溃"别再靠猜。
- **别拿 `git stash` 当基线**：HEAD 的 `collect/selfplay.cpp` 引用了 `shardAddColumns`（不在 HEAD 树里）⇒
  回退后**编不过**，基线没法跑。要 A/B 就把改动做成旋钮（`--pool` 就是这么来的）。

## 8. 存疑（未核实，别当结论用）

1. `train.py` 的 `record` 走 `data/runs/*.jsonl`——旧文档里 `tboard`/`points.jsonl` 那套可视化已不存在，
   谁在读 `data/runs` 未查。
2. `.agent/REMOTE.md` 仍以旧实例（westb/48068、纯 CPU 口径）为背景 ⇒ 与 §7 冲突时以 §7 + 各 `_*.sh` 头部为准。

## 9. MCTS vs Beam 对照（2026-10-20）

- 代码：`collect/beam.{h,cpp}`（定宽 beam + 可选 rollout 评估）、`apps/beam-test.cpp`（同批局面 / 同一
  `evalUtility` / 多线程）、CMake target `beam-test`（纯 C++，不链 ONNX/pybind）；`BattleScumSearcher2`
  增加 `transitionCount`（树推进 + playout 各算 1），`GameOut` 增加 `transitions` / `rollouts` / `decisions`。
- 协议：`encJobs(n_per, 1e8)`（63 遭遇 × n_per，held-out 段），MCTS = UCT + 均匀随机 playout
  （argmax 访问数），Beam = 每决策点 `max_nodes` 个孩子 × `rollouts` 次 rollout；两边同一效用：
  胜 = 2 + 终局 HP 比例、负 = −1、未决 = 怪物进度 + 0.5·HP 比例。
- 1,008 场（n_per=16）：teacher 64/256/1024 = **73.12 / 77.88 / 80.36%**；`mctsq:64`（按 Q 选动作）
  = **75.89%**；beam `32:64:4` = **72.42%**（2142 trans/dec，242 roll/dec）、`32:1536:1` = **74.21%**
  （6805 / 927）。配对差：beam `32:1536:1` 比 teacher:256 低 **3.67pp**（p<0.001）。
- 504 场高预算：beam `128:4096:1` = **77.38%**（21034 trans/dec、2981 roll/dec、7.31 回合），
  仍低于 teacher:256 的 77.78%（2743 / 256）⇒ **beam 胜率饱和 ~74–77%**，加预算主要换成战损
  （胜场 HP 60.2% vs 46.8%）；纯启发式 beam（`32:2000`）= **37.9%**，比 simple 59.5% 还差
  ⇒ rollout/价值评估是 beam 的命门。
- win-only 消融（`--win-only`，n_per=8）：teacher:256 仍 **77.78%**；beam `32:64:4` **72.42%**、
  `32:1536:1` **73.61%**、`128:4096:1` **77.58%**（damage 51%、10.5 回合）⇒ beam 的胜率上限
  ~77.6% 不是 HP 权重造成的；去掉 HP 项只把战损打崩（胜场 HP 19%）。
- 结论：胜率与样本效率 MCTS 明显更强；beam 在高预算下更保血但回合更长。逐遭遇看 beam 最差的是
  Hexaghost（`32:64:4` −43.8pp）与 Nemesis（−37.5pp）这类多阶段/长战。
