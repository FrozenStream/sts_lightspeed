# 根目录 `.py` —— 干什么用的、值不值得留

> 📐 代码规范化（注释简明 / 聚团）标准见 `.agent/CODE_STYLE.md`；run-family 已保留（第三波归档取消）。

> ✅ **2026-09-24 第二轮归档（10 项）+ 事实订正**
>
> **订正**：根目录文件现已**全部进 git**（09-22 警告的 8 个"裸奔"文件已被后续提交收编）⇒ 归档/删除全部可恢复。另：`sync.sh` 的 `FILES` 清单早已演进为 `managed()` 通配（下文旧措辞按此理解）。
>
> 移入 `_old/`：
> - **零碎产物 4**：`az_boss.pt`（旧产物，其生产者 `train.py all` 已删）、`dump_net_s30000.log`、`_enc_ids_out.txt`（`enc_ids.py` 可再生成）、`_q_enc.npz`（全仓零引用）
> - **已定案探针 4**：`_batch_test`（叶批验证 → 结论已固化为叶批量形参 `batch=` / `--mcts-batch`）、`_probe_hand`（手牌置换等变）、`_probe_turn`（回合灵敏度）、`_profile_state`（状态空间报告）
> - **被取代/依赖已停 2**：`_queue`（**被 `chain.sh` 完全取代** —— 同功能，chain 是现行标准）、`_merge_run`（服务 `collect_run` 并行分片，该线暂停）
> - 顺带清：`__pycache__/`（1.1MB，可再生）、`err.txt`（0B）
> - 新增保护：`collect_dagger.py` 已 `git add`（DAgger v1 采集器）
>
> **第三波提议（待拍板）**：整局 run-family 全链 13 个 —— `rlenv` `rundata` `runlayout` `runnet` `runpolicy` `collect_run` `train_run` `_chk_rundata` `_chk_runnet` `_chk_runmap` `_chk_runpolicy` `_chk_runtrain` `_probe_runrange`。自 09-23 起全部精力在战斗侧（az/walk/boss/realtest），该线**暂停**。**裁决（2026-09-24）：保留不删 —— 改为随「代码规范化」逐波整理（标准 `.agent/CODE_STYLE.md`），第三波归档取消。**（`_z_ladder` / `_scale_run.sh` / `_scale_probe.py` 已不在工作树；如需恢复 `git checkout -- <文件>`。）
>
> ⚠️ **2026-09-30 部分推翻（用户：「run 相关内容当前完全过时且搁置，其他的保护内容是冗余的」）**：
> **守卫**退役 10 个 —— `_chk_run` `_chk_runmap` `_chk_runnet` `_chk_rundata` `_chk_runtrain`
> `_chk_runpolicy` `_chk_snap` `_chk_select` `_chk_trunc` `_chk_turn`，已 `git rm`
> （这批全部**被 git 跟踪** ⇒ `git show HEAD:<名>.py` 可取回；`_chk_all.py` 清单同步摘除，
> **slow 档随之消失**）。**run 库代码本身不动**（`rlenv`/`rundata`/`runlayout`/`runnet`/`runpolicy`/
> `multi_hot`/`collect_run`/`train_run` 全保留 —— 该线只是**搁置**，不是取消）。

> ⚠️ **2026-09-22 归档（16 个）**：`_old/` 现在是本仓的归档目录（既有先例：`_old/bench.py`、
> `_old/rltrain.py`…）。归档判据 = **问题已定案 + 没人 `import`**（机械扫过一遍 `ast`）：
> - 性能基准 4：`_bench_{compile,forward,loop,mcts}`
> - 数据/缓存工具 3：`_stat_data` `_subcache` `_state_space`
> - 标定/诊断 4：`_cal_turn` `_deck_var` `_batch_sweep` `_parity_test`
> - 旧格式处理 2：`_migrate_obs`（已定为"重采而非迁移"）`_remap_space`（81→96 一次性）
> - 事件层探针 2：`_probe_event` `_evprobe`
> - 已跑不了 1：`_pile_ab`（`pile_top_k`/`pile_pool` 旋钮 09-22 删除 ⇒ 一构造就 `TypeError`）
>
> **内容不删**（只是不再进根目录、不再推送）：`sync.sh` 的 `FILES` 已同步摘除，
> 并把归档判据写在 `FILES` 上方。
> **留着不动的**：守卫（`_chk_all` 跑的那些）、流水线（collect/train/eval/prior）、
> 被 import 的库（`zspec`）、
> 还有将来还会用的（`_queue` `_cmp_cache` `_cmp_ckpt` `sp_ab` `_z_ladder` `_net_search_sweep`）。
> ⚠️ 下面第 4 节的计数与行数是**归档前**的；要刷新跑文末那条命令。
>
> ⚠️⚠️ ~~**8 个文件不在 git 里**（`_chk_res` `_chk_runnet` `_chk_rundata` `_chk_runtrain`~~
> ~~`_chk_bossrelic` `_probe_runrange` `_res_prior` `train_run`）⇒ 删掉就永久没了~~
> **已过时（2026-09-30 复核）**：这批现在**都在 git 里**（`git ls-files --error-unmatch` 逐条验过）
> ⇒ 「未跟踪 ⇒ 只能移不能删」的前提不成立，退役可直接 `git rm`（历史里可取回）。
> ➕ 2026-09-22 新增：**`_merge_run.py`** —— 合并多份 `collect_run.py` 的输出（采集器是单进程的，
> 并行多开是唯一加速手段；它会重编号 `bid` 并拒绝合并不同宽度的数据）。
> ➕ 2026-09-22 新增守卫：**`_chk_reexport.py`** —— 扫全仓 `from aznet import …` 的名字，逐个
> `hasattr` 验证。**同一个坑踩了两次**：`_unique_idx_ok` 忘再导出 ⇒ `_chk_actions` / `_chk_select`
> 自己 `ImportError`，而 `_chk_all` 只报"退出码 1"，两个守卫静默停摆。已登记进 `_chk_all`（engine 档）。
> **为什么有这份文件**：2026-09-21 被问「整理一下项目文件，这些测试脚本都有用吗」。
> 答案不该靠印象 —— 下面第 4 节是**机械生成**的（行数 + 脚本自己的 docstring 首行），
> 第 1~3 节是实测结论。重新生成第 4 节的命令见文末。

## 1. 结论：没有堆积，问题在别处

73 个根目录 `.py`、15573 行，**全部**登记在 `sync.sh` 的 `FILES` 里
⇒ 按"有没有人管"这个标准，**没有无人认领的死文件**。

> ⚠️ 2026-09-22 复核：`_probe_runrange.py` 当时**漏登记了**（表里没有、也没人发现）。
> 计数与行数已按当日重新机械生成的结果刷新（59 → 73）。

所以「删掉一批」不是这个项目真正的整理需求。真正的三条问题：

1. **守卫没人跑。** 十几个 `_chk_*.py` 写得很好，但没有任何东西会去运行它们。
   实际代价：`_chk_actions.py` 一跑就抓到「动作空间只支持 **4 只怪**」，错了很久；
   掩码口径（掩码 = **搜索树访问过的边** ≠ 真实合法集）**错了 4 天**才被发现 ——
   因为当时**根本没有**对应守卫，也没有谁会挨个去跑那十几个脚本。
   ⇒ **守卫的价值 = 被运行的概率。** 对策见第 2 节。
2. **有脚本带着已被推翻的前提活着。**（✅ 2026-09-30 已修）`walk.py` 曾因为 `_evprobe.py`（2026-09-17）那句
   「事件房会硬崩」而**跳过事件房**。那个结论的前提是"当时只能盲选选项"；
   现在有 `valid_event_options`，`_chk_run.py` 实测 **2000 局 / 48 种事件 / 0 崩溃**。
   ⇒ 已改成按 `valid_event_options` 取最低合法位；顺手把它的 CLI 删了（职责分别归
   `_chk_run.py` / `eval_walk.py`），现在它只是 `eval_walk` / `_chk_shape` 共用的 harness 库。
   （`_evprobe.py` 本身不废 —— 它的结论没错，错的是**怎么应用**它。）
3. **路径结构不能动。** `_*.py` 全在根目录是**故意的**：`sync.sh` 的 `FILES`
   和笔记里几十条复现命令都写的是根目录路径。塞进子目录会把那些命令**全部作废**。

## 2. 守卫 —— 唯一有统一入口的一类

```bash
python _chk_all.py                      # engine 档：不需缓存，秒级
python _chk_all.py --data data/x.npz    # 再加 data 档（需要一份缓存）
python _chk_all.py --list               # 只列清单
python _chk_all.py -k mask              # 只跑名字含 mask 的
```

⚠️ `_chk_all.py` **每个检查各起一个子进程**：守卫可能撞上 C++ 的 `assert(false)`，
那是 **SIGABRT，Python 抓不到**；同进程里跑第二个就没机会，而且只看得到"崩了"、
看不到是哪个守卫崩的（`_probe_event.py` / `_chk_run.py` 都踩过这个坑）。

| 名字 | 命令 | 档 | 说明 |
|---|---|---|---|
| 动作可编码 | `_chk_actions.py` | engine | 合法动作能唯一编码；`expand_children` / `batch_snapshot` 与旧路径逐位等价 |
| 掩码语义 | `_chk_mask.py [n=5] [sims=500]` | engine | 掩码 == 真实合法集 |
| 池化不变量 | `_chk_pool.py [遭遇名=THREE_SENTRIES] [层=12] [seed=1000]` | engine | 逐卡嵌入→mean→主网络 成立（含置换不变） |
| 推理前向 | `_chk_infer.py` | engine | `inference_net` 就是 net 本身 / 确定 / numpy 与 torch 输入一致 / 线程数落实 |
| 树复用 | `_chk_search.py` | engine | `SearchTree`：状态链逐位对齐 / nv 不泄漏 / 首步与从零建树逐 位一致 / 丢掉的子树立即回收 / 裸 torch 网可用 / 错位必须报错 |
| 缓存自检 | `_chk_cache.py selftest` | engine | 缓存守卫本身会被触发 |
| Boss 遗物候选 | `_chk_bossrelic.py` | engine | 三选一的 relic id 可读、且作为**内容**进了观测 |
| 缓存宽度 | `_chk_cache.py <npz> [n_probe]` | data | 这份 obs 是不是**当前二进制**采的 |
| 归一化体检 | `_chk_scale.py <npz> [ratio=10]` | data | 索引列整数性 + 每列**归一化后**的分布 |
| 遭遇战归因 | `_chk_enc.py <npz> [n_probe]` | data | 多怪状态为什么这么少 |
| 教师难度哨兵 | `_chk_difficulty.py [n=40] [sims=300] [asc…]` | 清单外 | 重编后的**数值中性** |
| dynamo 重编译 | `_chk_recompile.py` | 清单外 | `torch.compile` 何时放弃 |
| 多进程等价 | `_mp_test.py` | 清单外 | 并行结果与串行**逐位**相同 |

⚠️ 「清单外」= 不进 `_chk_all.py`（要手动跑：耗时/需特殊输入）。
2026-09-30 退役的 10 个（`_chk_run` `_chk_runmap` `_chk_runnet` `_chk_rundata` `_chk_runtrain`
`_chk_runpolicy` `_chk_snap` `_chk_select` `_chk_trunc` `_chk_turn`）从这两张表里一并删除。

## 3. 其余 55 个的分类（按 docstring 事实）

**库 / import 目标（10，无 `_` 前缀 —— 不是拿来跑的）**：
`azlayout.py` `azaction.py` `azsearch.py` `aznet.py` `runenv.py` `runlayout.py` `runpolicy.py` `runnet.py` `rundata.py` `check_card_data.py`

**基础设施 / 唯一定义点（3）**：`runtime.py`（**纯工具**：换机器只看这一个；`workers(v)` / `mp_context(mp_start)` / `cache_path` / `DATA_DIR` —— **没有任何旋钮读取点**）`_queue.py`（排队）`zspec.py`（z 口径全仓唯一定义点）。⚠️ **配方旋钮 = 各脚本自己的 CLI flag**（2026-09-30 定案），不常变动的量写成模块级常量（如 `collect.EVAL_SEED0` / `train.PAR_TRAIN`）；全仓唯一还读 env 的地方是 `train._BGLOG`（`STS_BGLOG`，作业日志路径）。

**数据侧公共层 / 采集（4）**：`collect.py`（**通用采集执行器**：造环境 / 跑教师 / 并行 / 组装 / 三道闸的**唯一**定义点，**不** import 训练代码）`collect_encs.py`（**唯一的教师任务表** `ENCS`：怪组 × 层；调度四步 = 列清单 → 剔除已有分片 → 线程池逐项拉起 `collect_part.py` → 回收判码）`collect_part.py`（**一项**的采集者：flag 全按 `collect_part()` 的签名注册，父进程用 `part_argv` 拼命令行 —— 一项一个进程 = 原生 abort 的隔离边界）`collect_selfplay.py`（自对弈，闭环的生成侧）。四个采集器都有 `--name`（自定义名字进文件名最前面），名字 = **完整配方**（含默认值）+ `ENCS` 表指纹；交叉引用只写对方的人给名字（`runtime.dataset_tag` / `net_tag`）。

**训练 / 评估（3）**：`train.py`（战斗侧唯一训练脚本，**不含采集**）`_boss.py`（**场景造法**：Act-1-end 规格 + 按层发状态 `shape_floor`）`train_run.py`（整局网络训练），另有 `collect_run.py`（整局战斗外采集）；评估 = `eval_walk.py`（漫游胜率主评）+ `probe_imit.py` / `probe_capacity.py`（离线探针）。⚠️ 原评估线 `_gen_eval.py` / `_eval_encs.py` **已于 2026-09-30 退役**（`git rm`，`git show HEAD:<名>.py` 可取回）。

**基准 / 性能（5）**：`_bench_compile.py` `_bench_forward.py` `_bench_loop.py` `_bench_mcts.py` `_scale_probe.py`

**探针 / 诊断（4，均在场）**：`dump_battle.py`（阅读版快照）`enc_ids.py` `_chk_scale.py` `_chk_enc.py`。
⚠️ 原先列在这一行的 9 个（`_probe_event` `_probe_hand` `_probe_runrange` `_probe_turn` `_diag_head` `_teacher_diag` `_deck_var` `_profile_state` `_state_space`）**已不在工作树**：前 6 个在 2026-09-22 归档到当年的 `_old/`（该目录现已不存在），后 3 个与 `_diag_head`/`_teacher_diag`/`_gen_eval` 一起在 2026-09-30 被删（`git show HEAD:<名>.py` 可取回）。

**数据 / 缓存工具（6）**：`_stat_data.py` `_subcache.py` `_cmp_cache.py` `_cmp_ckpt.py` `_migrate_obs.py` `_remap_space.py`

**实验 A/B / 标定（8）**：`_pile_ab.py` `sp_ab.py` `z_revalue.py` `_z_ladder.py` `_net_search_sweep.py` `_batch_sweep.py` `_batch_test.py` `_cal_turn.py`

**前提已过时（1）**：`_evprobe.py` —— 见第 1 节第 2 条

### 两个特别值得留的

- `_migrate_obs.py`：宽度从 `aznet.LAYOUT` 推导，**只加宽不收窄**
  （明写「❌ 宽度 > 当前布局 —— 本工具只会加宽」）⇒ 对将来任何一次布局变更都还有效。
- `_evprobe.py` 与 `_chk_cache.py` 这类"守卫的守卫"：能证明**守卫本身会拒**。别删。

## 4. 退役约定

⚠️ **根目录 `.py` 全部未被 git 跟踪** ⇒ 删除**不可逆**、也无法靠 git 复原。
⇒ **只移不删**，移到 `_old/`（该目录已是既有约定，里面有 11 个上游期脚本）。

已退役：
- `test.py`（2026-09-21）—— 12 行 scratch、无 docstring，走固定路线只跑 Act 1 的 16 层，
  功能被 `walk.py` **完全覆盖**。`sync.sh` 的 `managed()` 会过滤不存在的文件，移走是安全的。

## 5. 完整机械表（**2026-10-02 复核**：根目录 51 个 `.py`）

| 文件 | 行数 | 干什么用的（docstring 首行） |
|---|---:|---|
| **守卫（保留 `_` 前缀）** | | |
| `_chk_actions.py` | 205 | 动作空间完整性检查：哪些遭遇战有**无法编码**进策略头的合法动作。 |
| `_chk_all.py` | 133 | 守卫总检：一条命令跑全部「永久不变量检查」（守卫的价值 = 被运行的概率）。 |
| `_chk_onnx.py` | 65 | `inference_net` 与 torch 前向逐元素一致（≤1e-6）+ batch 维灵活（1/4/8 共一张图）。 |
| `_chk_search.py` | 153 | `SearchTree` 跨步复用的守卫：状态链逐位对齐 / nv 不泄漏 / 首步与从零建 树逐位一致 / 丢弃的子树立即回收 / 裸 torch 网可用。 |
| `_chk_bossrelic.py` | 146 | Boss 遗物三选一：**候选内容必须可读**，且必须进观测。 |
| `_chk_cache.py` | 155 | 缓存有效性检查：这份 obs 是不是**当前二进制**采的。 |
| `_chk_enc.py` | 102 | 把缓存的决策点按**遭遇战**归因 —— 回答「多怪状态为什么这么少」。 |
| `_chk_encs.py` | 165 | 守卫：按怪组×分布地点采集（`collect_encs.py`）—— 列 / 地点 / 续跑 / **失败不静默**。 |
| `_chk_mask.py` | 94 | 掩码语义检查：`m_act` 到底是「真实合法动作」还是「搜索树访问过的边」？ |
| `_chk_pool.py` | 117 | 池化不变量检查：确认「逐卡嵌入 → 取 mean → 进主网络」真的成立。 |
| `_chk_res.py` | 202 | 资源块守卫：遗物=多热池化、药水=**逐槽 id 列表**，以及药水**动作**的因子化网格。 |
| `_chk_revive.py` | 300 | 守卫：**「死了」和「要复活」必须分得开**，以及 `monstersAlive` / `deckSize`。 |
| `_chk_scale.py` | 147 | 归一化体检：索引列必须无小数部分 + 每个标量列**归一化后**的分布。 |
| `_chk_shape.py` | 245 | `shape_floor`（**层数 → 发育状态**）的守卫 —— 「直接指定怪物」训练流程的地基。 |
| `_chk_threads.py` | 157 | 训练线程数守卫 —— 「同线程数跨进程**逐位相同**」，并把「换线程数 = 换轨迹」量出来。 |
| **主代码（无 `_` 前缀）** | | |
| `audit_data.py` | 115 | `data/` 体检：逐个文件读出**自描述字段**，判它是现役还是过期 —— **不靠文件名**。 |
| `azaction.py` | 150 | 动作空间 + 编码约定 + 终局读数 + 策略函数。 |
| `azlayout.py` | 244 | 观测布局 → 下标 / 名义类别（归一化在 C++ 快照生产者） |
| `aznet.py` | 378 | 网络 + 全量重导出 —— RL 侧的**唯一入口**。 |
| `azsearch.py` | 223 | PUCT 搜索（网络引导）—— 不做随机 roll-out。 |
| `check_card_data.py` | 227 | 检查 include/constants/Cards.h 与 Card_data.csv 中卡牌数据的差异。 |
| `ckpt.py` | 189 | 权重存档的**唯一读写入口** —— 单文件自描述（`fmt=1`，2026-09-28 起）。 |
| `collect.py` | 352 | 采集侧公共层：通用执行器 + 缓存两道闸。 |
| `collect_encs.py` | 402 | 按怪组 × 分布地点采集教师数据：每项固定场数。 |
| `collect_part.py` | 98 | 采一项（怪组 × 分布地点）写一个分片 —— 每项一个进程的隔离边界。 |
| `collect_run.py` | 226 | 采集**整局**（战斗外）决策数据 —— 全局网络的第一块地基。 |
| `collect_selfplay.py` | 227 | 自对弈采集：用网络自己的 PUCT 产训练数据（闭环的生成侧）。 |
| `devshape.py` | 177 | `scene.shape_floor` 的发牌表（**v7 口径**）—— **单套**牌组，6 段阶梯、没有逐层表；末段对齐 A20 胜利局实测。 |
| `dump_battle.py` | 326 | 打印一场战斗的**全流程快照**：每步按块解码 + 原始向量 + 逐步摘要表。 |
| `enc_ids.py` | 86 | 枚举 `sts.MonsterEncounter` → 快照怪物 id 组合（**反向索引**，含变体）。 |
| `eval_walk.py` | 335 | 漫游场景的**同分布胜率评测** —— 检验训练成果的第一站。 |
| `multi_hot.py` | 234 | 资源块（多热 / 逐槽 id）的消费者描述符 —— **整局网 `runnet` 专用**。 |
| `probe_capacity.py` | 232 | 容量探针（战斗线 · 上限测试的**容量轴**）：固定数据 / 配方，只扫主干宽度。 |
| `probe_imit.py` | 409 | 策略头模仿质量探针 —— **不重训**，先量出「离天花板还有多远」。 |
| `runenv.py` | 515 | 整局（run-level）环境层：地图遍历 + 房间处理 + episode/env 封装。 |
| `rundata.py` | 213 | 整局轨迹 → 回报 / TD 目标 —— **纯数据层**（无 torch、无模型），同 `runlayout` 的纪律。 |
| `runlayout.py` | 348 | 战斗外（整局）观测与候选动作 —— **纯数据层**，与 `azlayout` 同构但对象是 `GameContext`。 |
| `runnet.py` | 463 | 整局（战斗外）网络 —— 全局网络**自己维护**嵌入表，不复用战斗网。 |
| `runpolicy.py` | 334 | 整局（战斗外）策略 —— 把上游 `ScumSearchAgent2` 的硬编码启发式搬进 Python。 |
| `runtime.py` | 211 | 运行环境相关的**单一定义点** —— 迁移到别的机器时只需要看这一个文件。 |
| `scene.py` | 158 | 按层造发育状态：`shape_floor` 一族，按 `devshape` 的 6 段阶梯缩放。 |
| `sp_ab.py` | 109 | 闭环自对弈的**覆盖度**对照：确定性 argmax vs 按 π 采样。 |
| `metrics.py` | 170 | 训练读数落盘（**纯 JSONL，不依赖 tensorboard**）：逐点标量 + 配方 + 故障。 |
| `train.py` | 639 | 训练脚本：训练 AZNet。**不含采集、不含评估** —— 采集全是独立脚本： |
| `train_run.py` | 425 | 整局网络的训练：**行为克隆（策略头）+ 值回归（值头）**。 |
| `walk.py` | 294 | 整局漫游 harness（**库**，无 CLI）：从涅奥开始随机沿地图往下走， |
| `watch.py` | 123 | 作业看门狗：采样日志状态与进程占用，写出 JSON 状态文件（不依赖任何本地会话）。 |
| `z_revalue.py` | 228 | 战损（HP 消耗）度量 + **零搜索**的重放核对。 |
| `zspec.py` | 87 | 值目标 `z` 的**各种口径** —— 全仓唯一的定义点。 |
