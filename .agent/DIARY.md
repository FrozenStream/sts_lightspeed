<!-- 本文件 = 本项目的**记忆正文**（唯一权威副本）。
     2026-10-01 由三份合并而成：`.agent/memory/sts-lightspeed-rl.md`（1795 行）、
     `.agent/memory/experiment-pitfalls.md`（651 行）、VS Code 仓库记忆侧那份摘要（215 行）。
     **权威副本在仓库里**：随 git 跟踪、随工作区迁移、撤回在 `git diff` / `git log` 里看得见。
     VS Code 记忆侧只留指针（`/memories/repo/README.md`）。
     ⚠️ **改动只写这里**。任何「导出 / 副本」都会当天过期
     （2026-09-20 实测：导出当天 `sts-lightspeed-rl` 就差 29 行）。 -->

> ⚠️ **历史名字怎么读**（2026-10-01 核实）：②③④⑤⑥ 里出现的 `_gen_eval.py` / `_cmp_ckpt.py` /
> `_bench_loop.py` / `_z_ladder.py` / `_deck_var.py` / `_chk_trunc.py` / `_chk_snap.py` /
> `_chk_select.py` / `_chk_turn.py` / `_diag_head.py` / `_teacher_diag.py` / `_flags.py` /
> `_prog.py` / `_collect_walk.py` / `_collect_encs.py` / `_collect_dagger.py` / `_boss.py`
> **都已在 2026-10-01 前删掉或改名**；`STS_*` 旋钮已全部改成显式 CLI 参数（唯一例外 `STS_BGLOG`）。
> 另有 `sync.sh` 的 `run` / `bg` **两个子命令已删** —— 现行 = `check` / `push` / `start` / `attach|kill` / `status` / `log` / `wait`。
> 那些名字是**当时产生这些数字的工具**，数字本身仍有效 —— 读到按历史理解，**当前事实只看 ①**。
# sts_lightspeed — 实验日记

结构按**阅读频率**排（不是按重要度）：

| 段 | 内容 | 什么时候读 |
|---|---|---|
| ① 常青 | 环境 / 远端 / 脚本 / 状态空间 / 教师 / 影响面 —— 只写**当前事实** | 动手前 |
| ② 日记 | 按日期倒序的逐次改动（2026-09-23 → 10-01），含记忆侧摘要 | 想知道「为什么现在长这样」 |
| ③ 已验证结论 | 跑出来的数字与结论 | 设计实验前 |
| ④ 坑与教训 | 方法论坑 —— **本项目最贵的资产** | 设计实验前 |
| ⑤ 计划 | 任务清单 / 优先级 | 排下一步 |
| ⑥ 附录 | 已作废方案 / 旧数字作废清单 —— 只在追溯时读 | 追溯 |

复核入口：`.agent/notes.md` 开头（逐会话记录）；文件分工表在 `.agent/README.md`。

---

## 目录

- ① 常青：当前事实（环境 / 远端 / 脚本 / 状态空间 / 教师 / 影响面）
- ② 日记（按日期倒序；含记忆侧摘要）
- ③ 已验证结论（跑出来的数字与结论）
- ④ 坑与教训（方法论 —— 最贵的资产）
- ⑤ 计划（任务清单 / 优先级）
- ⑥ 附录（已作废方案 / 旧数字作废清单 —— 只在追溯时读）

# ① 常青：当前事实

> 本节**只写当前事实**。各自有家的内容别在这里重复（重复 = 下次只会有一半被更新）：
> 远端与换实例 → `REMOTE.md`｜脚本清单与守卫 → `SCRIPTS.md`｜换机器 → `MIGRATION.md`｜
> 动作空间缺口全表 → `ACTION_GAPS.md`｜逐会话记录 → `notes.md`。

### 环境（本地）
- Python 由 uv 管理，**venv 内没有 pip**；跑脚本一律 `.venv/Scripts/python.exe`
- 构建**只能**走 VS Code 的 CMake Tools（终端里没有 `cmake`/`MSBuild` 可用）；
  等价命令行：`"$CMAKE" --build build --config Debug --target slaythespire`
- ⚠️ **必须 Debug**：`include/sts_common.h` 无条件定义 `sts_asserts`，而那些块体里写的是 C 的 `assert`
  ⇒ `-DNDEBUG`（Release）下全仓自检**静默失效**（后果不是慢，是 UB 与假通过）
- `.pyd` 会被 notebook 内核锁住 ⇒ 先杀内核，否则 lld-link `permission denied`
- 构建**之后**再刷桩：`.venv/Scripts/python.exe -m pybind11_stubgen slaythespire -o build/Debug`
  ⚠️ Pylance 只在语言服务器启动时读一次桩 ⇒ 报「不是已知属性」这类**假错**时重启它
- git-bash 的 `/tmp` 重定向到 `%TEMP%`，而 Python 看到的 `/tmp` 是 `D:\tmp` —— 两个不同的地方

### 状态空间（快照）
- 快照 = **固定布局 int 向量**，当前 **1603** 维。唯一事实来源 = `bindings/slaythespire.cpp` 的
  `struct Snap`；Python 侧读 `sts.snapshot_layout()` —— **别硬编码偏移**
- 复核一条：
  ```
  .venv/Scripts/python.exe -c "import slaythespire as s; L=s.snapshot_layout(); print(L['len'], L['counts_len'], L['pile_slots'], L['card_select_slots'], L['deck_slots'], L['monster_fields'])"
  ```
  应为 `1603 7 128 128 96 9`
- 计数块 = **7** 项：`monsterCount`（含尸体、只增不减）/ `monstersAlive`（真活怪数 = 胜负判据）/
  手牌 / 抽牌堆 / 弃牌堆 / 消耗堆 / **`deckSize`**（牌组块只走均值池化 ⇒ 张数只能单独给）
- 卡牌 id = `raw*2 + upgraded`；0 = INVALID = 空槽
- 三个牌堆 = **纯集合 + 计数**（不截断、不做堆顶优先；网络侧只做集合均值池化，跳空槽）——
  这是**部署口径**，不是消融结论
- ⚠️ **块宽**漂移是**静默**的（多一列少一列网络照跑）；**取值域**漂移会吵（`IndexError`），
  但报错点离病根很远
- ⚠️ 改布局 ⇒ 旧缓存 / 旧 ckpt 全作废。宽度变了靠 `obs_dim` 闸当场挡；**只换列序、宽度不变**的
  靠 `collect.check_cache_obs` 的重放对账挡
- ⚠️ **行 0 约定 =「空槽、可学」**：战斗网的 `card_embed` / `emb_monster_id` / `potion_embed`
  一律**不加** `padding_idx` ⇒ 空槽要从池化里排除**只能靠掩码**。
  ⚠️ 整局网（`runnet` / `multi_hot`）相反：那边行 0 是 padding、恒 0 不学 —— 别搬错网

### 动作空间
- `POLICY_DIM = 96`（手牌 10 × 目标 6 + 药水 5 × 7 + END_TURN）、`SELECT_DIM = 128`
- 默认值目标口径 = `zspec.DEFAULT_SPEC` = **`enginenorm`**（赢局照抄引擎整式、败局近 0；
  与树内 `Q` **同尺度**）。⚠️ 换口径 ⇒ 历史 `val_ce` / 胜率不可比
- `_chk_actions.py` 是这一层的守卫：**任何合法动作都必须能编进唯一统一下标**

### ⚠️ 多选类选牌：`MULTI_CARD_SELECT` 编不进统一下标（**触发者已全部排除**，2026-09-30）

> 📌 本节的**触发者全表 / 被连带无视的游戏内容 / 复核命令**都在 `ACTION_GAPS.md`；
> 改抽取范围（加 RARE、加药水、加遗物）之前先读那份。

`CardSelectTask` 共 **21** 个，而快照的候选块（`appendCardSelectCandidates`，`bindings/`）
只覆盖 **13** 个。剩下的落进 `default:` ⇒ `candidateCount = 0`、**一个槽都不铺**。

真正致命的是：这些状态里**唯一的合法动作是 `MULTI_CARD_SELECT`**，而 `actionToIdx`
**没有这个分支**：

- 2026-09-29 之前：返回 `-1` ⇒ Python 侧 `ActionEncodingError`（那层兜底**已删**）。
- 2026-09-29 之后：`default:` **当场抛**
  `RuntimeError("动作无法编码进统一下标：不认识的动作类型")`（`bindings-util.cpp::unencodable`）。

两条路都是**响亮失败**，但采集中途抛 = **整项作废**（不是这一场）。

铁甲够得着的触发者**只有三处，且全部已从场景发放池里排除**：

| 触发者 | 机制 | 排除点 |
|---|---|---|
| 药水 `ELIXIR_POTION` | `Actions::ExhaustMany(10)` → `EXHAUST_MANY` | `scene.UNSUPPORTED_POTIONS` |
| 药水 `GAMBLERS_BREW` | `Actions::GambleAction()` → `GAMBLE` | 同上 |
| 遗物 `GAMBLING_CHIP`（RARE） | **回合开始**强制进弃牌多选屏 → `GAMBLE` | `scene.BANNED_RELICS`（2026-09-30 加） |

⚠️ 连带排除的两类：`ENTROPIC_BREW`（随机生成药水 ⇒ 可能生出上面两瓶）、
`COLORLESS_POTION`（生成无色牌 ⇒ 可能生成 `PURITY`，它也走 `EXHAUST_MANY`）。
⚠️ 铁甲的 `PURITY` 本身**不在红卡池**里 ⇒ 够不着。
⚠️ 其余 5 个（`MEDIATE` / `NIGHTMARE` / `RECYCLE` / `SEEK` / `SETUP`）是观者/静默/机器人专属。

**代价明确**：铁甲少两张实用药水 + 一件遗物（真机里有 ⇒ 对外是分布外）。
**真正修法**：候选 multi-hot + 一个 `confirm` 动作（独立工作；上游源码在枚举处写着
`// just dont deal with this right now`）。

📌 教训：**「未绑定 / 未使用」的代码路径 = 未验证的路径**。这条坑藏到 2026-09-30 才炸：
① 早期场景药水恒 0，够不着；② 遗物侧原本是手写配比，09-29 改走引擎档位表后才抽得到
RARE 的赌博筹码。实测一次跑掉 **61/63 项**（`collect_encs.py 1000 5000 --eps 0.1`）。

### 教师（BattleScumSearcher2 = UCT + 随机 playout）
- 契约：`teacher_action(b, sims, rng, tau=0.0, eps=0.0, exploration_parameter=None)`
  —— `rng` **必填**（`random.Random`），随机性一律走它；`sims ≥ 1`
- 可插拔：`EvalFnc evalFnc`（默认 `evaluateEndState`）、`explorationParameter` 默认 `3√2`
  （`sts.DEFAULT_EXPLORATION_PARAMETER`）、`playoutRandom` 是**均匀随机**
- 确定性：`randGen(bc.seed + bc.floorNum)` ⇒ 同状态 + 同参数必给同一答案（标签噪声也是确定性的）
- 覆盖：`b.search_stats(sims, c)` / `teacher_action(..., exploration_parameter=c)`
- ⚠️ 只有**未终局**的战斗能跑搜索（终局状态 `enumerate_actions()` 在 C++ 里直接 abort）
- 实测结论（`c` 扫描、`sims` 的真实效果、标签质量…）都在 ③

### 🖥️ 远端 / ssh 实操坑

（原「笔记在哪」只剩下面这些**实操坑**；「正文的家」那一段已由本文件开头取代。）

⚠️ **SSH 会断连**（`Connection reset by peer`）：远端进程**没死**，只是本地失去信息。
**无感知的根因** = ssh 默认无连接超时 ⇒ 网络一断就挂着等 TCP 超时（数分钟），
表现是「终端卡住不动、不报错也不退出」。

- 对策：ssh **一律带** `ConnectTimeout=10`（`sync.sh` 已内置，`STS_SSH_TIMEOUT` 可调）。
- ⚠️ `ssh -tt` 正常收尾也会打印 `Connection to … closed.` —— **不是**断线信号。
- 判定运行状态只看**日志一个事实来源**：没日志 = 没跑过 / 有 `COMMAND_EXIT_CODE` = 已结束 /
  没有 = 仍在跑；并看**日志多久没动**（>5 min 警告 —— 光看进度条分不清「在算」和「死了」）。

⚠️ 实现坑：**`set -e` 是全局的** —— 函数内 `set +e` 后必须还原，否则调用方保护被覆盖、
返回非零时被 errexit 打死（症状 = 断网时静默退出）；调用方写 `cmd || rc=$?`。
⚠️ 实现坑：**远端脚本别用反斜杠转义**（`$((now-mt))` 会在本地被算术展开）——
用 `<<'REMOTE'` 引号定界符 + `__PLACEHOLDER__`。
⚠️ 实现坑：**解析远端输出别按行号**（无匹配时 grep 一行都不输出、行号前移 ⇒ 误判状态），按内容过滤。
⚠️ `pkill -f <pattern>` 会杀掉**你自己**（远程 shell 命令行本身就含那个 pattern）——
实测 `pkill -f -- "--help"` 自杀，exit 255、输出截断。先 `pgrep -af` 看清楚或按 pid 杀。

### 评估设置
- **种子纪律**：采集段起点 `collect.COLLECT_SEED0`、评估段起点 `collect.EVAL_SEED0`（`int`，= 1e8）；
  `eval_walk.py` 拒绝 `seed0 < EVAL_SEED0`（held-out 必须没被训练见过）。训练/评估段不得重叠
- ⚠️ **换种子段 ⇒ 绝对胜率不可比**（同 seed 配对才有意义）
- 对照必须**同 seed 配对**；跨进程统计要按**训练种子**配对，不是按评估局

> 🏷️ **2026-10-01 脚本改名**：主代码去掉 `_` 前缀并顺带改名（**守卫 `_chk_*.py` 不动**）——
> `_audit_data.py`→`audit_data.py`、`_az_train.py`→`train.py`、`_ckpt.py`→`ckpt.py`、`_coverage_walk.py`→`coverage_walk.py`、`_dump_battle.py`→`dump_battle.py`、`_enc_ids.py`→`enc_ids.py`、`_env.py`→`runtime.py`、`_eval_walk.py`→`eval_walk.py`、`_imit_probe.py`→`probe_imit.py`、`_probe_capacity.py`→`probe_capacity.py`、`_run_train.py`→`train_run.py`、`_sp_ab.py`→`sp_ab.py`、`_tb.py`→`tboard.py`、`_walk.py`→`walk.py`、`_watch.py`→`watch.py`、`_z_revalue.py`→`z_revalue.py`、`_zspec.py`→`zspec.py`；
> `_chain.sh`→`chain.sh`、`_sync.sh`→`sync.sh`、`_wait_logs.sh`→`wait_logs.sh`。
> **② 及其后的历史段（含 `.agent/notes.md`）保留当时的名字** —— 那是当时的工具名，按上表对照。

# ② 日记（按日期倒序；含记忆侧摘要）

### 2026-10-04 · 归一化除数**搬到 C++ 快照生产者**：赋值处自带除数，快照只剩一个出口（归一化）

- **起因（用户的审计）**：`azlayout._SCALE_SPEC` 里 `card_select` 行只给了 `[1, 5, 999]` 三个值，
  而该块每条目 **4 个字段** ⇒ `_build_scale()` 的 `np.tile(val, n//len(val)+1)[:n]` 变成**逐槽轮转**：
  slot0 `[1,5,999,1]`、slot1 `[5,999,1,5]`、slot2 `[999,1,5,999]`… ⇒ `cardId` 在一个槽被 ÷5、
  `costForTurn` 在一个槽被 ÷999。实测复现（`floor=9/10` 的选牌帧）：`slot1 field0` 的 `cardId=386` 被除了 5、
  `slot0 field2` 的 `costForTurn=1` 被除了 999。**11 个块里唯一长度不一致的就是它。**
  同时发现 `player` 行尾的 `6 / 21`（`inputState` / `cardSelectTask`）是**死值**：
  `_build_scale()` 末尾 `s[CAT_IDX] = 1.0` 覆盖了它们，且这两列不进 `SCALAR_IDX`
  ⇒ 唯一的除法点（`index_select(obs,1,SCALAR_IDX)/scale[...]`）根本不碰它们；那条「名义类别不该被归一化」
  的断言断的是上一行刚设的值 ⇒ **恒真**（守不住它宣称要守的东西）。
- **定案（用户选 **A**：只留归一化出口）**：除数**写在 C++ 快照的赋值处**。
  `SnapWriter` 要求每个数值列写除数（`s.push<200>(bc.player.block)`，**无默认值，漏写编译不过**），
  索引 / 名义列走 `s.push_index(...)`（**根本没有除数参数** ⇒ 「除掉一个查表下标」在类型上写不出来）。
  快照**只有一个出口**：`get_snapshot()` / `get_snapshot_np()` / `batch_snapshot()`
  都是同一份**已归一化 float32**（= 网络输入、落盘缓存的唯一口径）。
- **代价（已接受）**：快照**不再是引擎状态的忠实转储** ⇒ 要真值必须走活对象访问器。
  为此补了两个**计数类**访问器：`BattleContext.monsters_alive` / `master_deck_size`
  （其它值 `player_hp` / `monster_count` / `monsters[i].cur_hp` 本来就有）。
- **顺带修掉的语义错 + 一个既存守卫 bug**：
  - `card_select` 的额外标量列改为 `[cost ÷5, costForTurn ÷5, specialData ÷999]`（与手牌块同形）。
    **这是本次唯一的行为变更** —— 旧口径下它们逐槽轮转。
  - `_chk_revive.py` 里 `mx == 320` 是**硬编码**，而引擎是 `maxHp = asc9 ? 320 : 300`
    （`MonsterSpecific.cpp`），`asc(30000)=6 < 9` ⇒ 它一直在失败（与本次改动无关，顺手按进阶取期望修好）。
- **Python 侧删掉**：`azlayout._SCALE_SPEC` / `_build_scale` / `SCALE`（及两条**恒真**断言）、
  `aznet` 的 `scale` buffer + 四处除法、`rlenv` 的 re-export。
  `aznet` 的 `_inputs` / `policy_keys` / `select_keys` 现在只 `index_select` + `reshape`。
- **连带的守卫改动**（快照归一化 ⇒ 读它的人要分清「帧内比较」与「跟引擎常量对齐」）：
  - `_chk_res.py` / `_chk_shape.py` / `_chk_revive.py` 的 `np.asarray(..., int64)` → `float64`
    （`int64` 会把归一化值**截断成 0**）。
  - 只在与引擎常量对齐处乘回除数（÷30 counts / ÷500 怪物血量 / ÷50 miscInfo /
    ÷60 floor / ÷999 gold），每处一行注释指向 `SnapWriter` 的赋值处。
  - **帧内比较一律直接用原值**（比 0 / 比大小 / 单调 / 比值 —— 与缩放无关）。
  - `_chk_actions.py`（怪物数）与 `_chk_shape.py`（总牌组）改用引擎访问器，不再从快照反推。
  - 🔎 顺手抓到一个**假通过**：`alive_by_slots()` 拿 `int(counts[0])` 当循环上界，
    归一化后 `int(0.03…) = 0` ⇒ 循环体一次都不进，函数恒返回 0、那条 check 永远"通过"。
  - `coverage_walk.py` 的「从 runCtx 推层号」回退**删掉改为报错**（缓存里是归一化值，推不出来）。
- **`_chk_scale.py` 重写**：Python 侧没有除数表可查 ⇒ 只看产出。
  ① 索引 / 名义列**必须无小数部分**（这条直接能抓上面那个 `cardId÷5`）；
  ② 每个标量列的 p50/p99/p99.9/max（`p99.9 ≥ 3` = 量程不够；`max ≥ 10×p99.9` = 少量行离群）。
  旧版的 `fields()` 假设「除数表长 == 块内字段数」，**与缺陷同源** ⇒ 它对轮转完全瞎（守卫和缺陷共享前提）。
- **验证**：新写入器与「旧 `SCALE` 逐列相除」在 **254 帧真实战斗**上**逐位相同**
  （唯一差异就是上面那个选牌帧，且全部落在 `SEL_EXTRA_IDX`）；索引列整数；
  `batch_snapshot == get_snapshot_np()`；守卫 `_chk_res` 29/0、`_chk_shape` 全通过、
  `_chk_revive` 22/0、`_chk_actions` 63 遭遇 0 编码失败；端到端冒烟（采 6 场 → `_chk_cache` 6/6 逐位一致
  → 训 1 epoch → `eval_walk` 自对弈 26 场，两侧同权重结果逐位相同）。
- **`potionCount` / `potionCapacity` 也已归一化（用户定，÷`Snap::potionSlots` = 5）**：
  它们与 `counts` 块同一性质（"几个"），÷1 会让这两列比邻居大 30 倍（体检当场标"量程不够"）。
  唯一按整数读它的地方是 `_chk_res.py`（非零槽数 == potionCount）⇒ 改为乘回
  `LAYOUT["potion_slots"]`（除数与 C++ 的 `Snap::potionSlots` **同源**，不写第二个字面量）。
  改后：体检 20/20 正常 / 0 量程不够，`_chk_res` 29/0。
- **橙丸位域改名义类别（用户定）**：`bitset<3>`（三牌型各是否已用）过去当 `0..7` 的数值列 ÷50。
  单标量下三个位**共用同一个权重**（只差 1:2:4 缩放）⇒ 秩 1 编码，学不出独立的位权重；而那个
  ÷50 是从 `playerCounter` 那块\"次数量纲\"抄来的（它不是次数，是位域）。⇒ 改 `push_index`
  原样 0..7 + 加进 `CAT_SPEC`（`(\"orange_pellets\", [P_ORANGE_PELLETS], 8)`）走 8 维 one-hot。
  宽度不变（仍 1 列），`cat_dim` +8 ⇒ `in_dim` 2106 → 2114（缓存/权重作废，已接受）。
- **⚠️ 教训：A 方案下「在 Python 里乘回写死的除数」不可靠 —— 已全部删掉。**
  实测：`counts` 的除数从 ÷30 调成 ÷5/÷10 之后，`_chk_revive.py` 乘回 30 得到 `6`，
  拿它当循环上界就**读到怪物块外面**（读成第一个玩家属性列）⇒ 那一帧\"多出\"一个活怪。
  **它给的是一个错的数字、不是一次失败** —— 比\"假通过\"更坏。
  ⇒ 帧模型改成 `(快照, 逐槽真值, 计数)`：数值列一律读引擎真值（`m.cur_hp` / `m.max_hp` /
  `m.misc_info` / `b.monsters_alive` / `b.master_deck_size`），快照只用来读**索引/名义列**（÷1，原样）。
  为此给 `Monster` 补三个只读真值访问器：`id` / `misc_info` / `is_targetable`。
  遗留唯一一处 Python 侧除数：`_chk_res.py` 的 floor÷60 / gold÷999（那两条天生是**跨口径**比较：
  验的是\"布局把哪一列映射到 floor/gold\"，引擎侧没有对应访问器可比）。
- **⚠️ 两个新发现（都源自上面那条守卫的本地/远端不一致）**：
  1. **教师（C++ MCTS）的动作序列跨机器不保证一致**。同一状态、同一 `SIMS=50`、同一 `random.Random(0)`，
     本地第一步是 `CARD src=0 tgt=0`、远端是 `CARD src=1 tgt=1`（随后还抽了药水）⇒ 轨迹迅速分叉。
     实测用的是同一份 `src/` 源码、同一份探针（逐帧引擎状态两侧**完全相同**）⇒ 分叉只在搜索里。
     ⇒ **不要拿教师轨迹当期待值**（守卫/测试都会因此变成机器相关）；也意味着
     「跨机器逐位对拍」这种验证**不成立**（只能同机复现）。
  2. **觉醒者半死窗口曾能被"打光小怪"判胜 —— 与原版不符，已修**。
     端口把"全灭"简化成计数器 `monstersAlive`，而一阶段觉醒者的 `die()` **先** `--monstersAlive`
     ⇒ 同一回合内再把其余小怪打死就 `monstersAlive == 0` ⇒ 当场判胜、二阶段永不出现。
     **原版（`_decomp/src/`，你自己 CFR 出来的那份）不是这样**：`AwakenedOne` init 里
     `room.cannotLose = true`，它的 `die()` 在 `cannotLose` 期间是**空操作** ⇒ `isDying` 从不置位；
     而 `MonsterGroup.areMonstersBasicallyDead()` 只跳 `isDying || isEscaping` ⇒ 半死的它**保住这一场**。
     复活在它自己回合的 `changeState("REBIRTH")`（`maxHp = asc9?320:300` + `HealAction` + `CanLoseAction`）。
     ⇒ 修法（**别动计数器**）：`MonsterGroup::areMonstersBasicallyDead()` 改成原版的**状态扫描**
     `!isDeadOrEscaped() || isHalfDead()`（"只要还有一只不是真死的，这场就还没完，半死算活着"），
     `Monster::die()` / `suicideAction()` 的判胜都改用它（`suicideAction` 里还得把 `curHp = 0`
     挪到判胜之前，因为判据看状态不看计数器）。
     端口的 `isDying()` 是 `curHp <= 0` 这个**状态**（原版是只在 `die()` 里置位的**标志位**）⇒
     写判据时必须像 `doMonsterTurn` 那样把 `isHalfDead()` 补回来。
2b. **`monstersAlive` 的语义一行未动**（= **可选中**的怪数），它同时是
`MonsterGroup::getRandomMonsterIdx` 的游走判据 ⇒ 那条不变量（`monstersAlive` == 逐槽
`!isDeadOrEscaped()` 数）本来成立，**不要**去重写那个消耗 RNG 的函数（先前一版把它改成
"先过滤成列表再抽"是多余的，已回滚）。
2c. **暗灵要补的是原版 `Darkling.damage()` 那一步**（端口原本靠计数器归零冒充它）：
`Monster::die()` 的 REGROW 分支里"先把自己标成半死 → 数同种里还有谁没半死 → 一只不剩时
清掉全员 `halfDead`（= 真死）并判胜"，否则半死按原版口径"算活着" ⇒ 三只永远在半死/复活
之间循环、**数学上打不死**。那里**不能**再 `--monstersAlive`（转半死时已经减过，再减会负数）。
⚠️ 反过来说：通配写法「有 halfDead 就不判胜」单独用也是错的，两处必须一起有。
  2d. `_chk_revive.py` 的边界断言跟着改（**28/0**，本地 + 远端）：一阶段死后 `monstersAlive`
     减 1（半死不算可选中）但**不许**判胜 / 命中计数器归零也不判胜 / 三小黑同回合清空**必须**判胜
     （防"打不死"回归）。
  ⇒ 守卫改成 `killer.execute(b)` 之后只 `b.end_turn()`（玩家不出牌 ⇒ 不会误杀小怪 ⇒ 复活路径必然走到），
  **不再调 `teacher_action`**。边界断言本地 + 远端都是 **28/0**。
- **旧权重**：它们的 `state_dict` 里有 `scale`（旧口径唯一 persistent buffer）⇒ `strict=True` 会报
  unexpected key；更根本的是旧 ckpt 里那份 `scale` 与代码表本就不同（`curHp/maxHp 200 vs 100`、
  `block 999 vs 200`、`energy 10 vs 20`）⇒ 旧权重在新口径下**不可复现**（已接受）。

### 2026-10-01 · 闭环第一轮：**零增益**（机制=自我确认）+ **软目标在当前条件下翻案**（胜率平、战损更好）

- **数据**：自对弈 MCTS(500) + π 采样 63,000 场（`seed0=1000000`，胜率 55.2%）掺进教师缓存 ⇒ `data/encdata_sp_s500_sample.npz`（2,183,699 行 / 126,000 场 / 16.6 GB；⚠️ `src` 是**整份一个标量**、合并后 = `mcts` ⇒ 要按来源切只能按 `seed` 段：教师 `0..999` / 自对弈 `1e6..`）。
- **第一轮训练**：16 轮 / 1 h 15 m ⇒ `n1_spmix_e16.pt`（`train ce=0.810 R2=+0.740 | val ce=0.811 R2=+0.704`）。
- **评估 = 零增益**（同 seed 批 100 局，`eval_n1.json`，8 臂）：N1 vs N0 三个预算档胜率差 −0.2 / +0.1 / +0.1pp，配对 McNemar p = 0.690 / 0.442 / 1.000，交集局剩血差 +0.44 / +0.46 / +0.25pp（≈1σ）；战损、终局分、场/局、终局层全在噪声里。
- ⭐ **机制坐实（自我确认）**：按 `seed` 切段量 N0 的 policy top1 —— 教师段 **60.5%(act) / 51.3%(sel)**，自对弈段 **83.6% / 81.6%** ⇒ 自对弈行里约 **84%** 本来就是网络自己会下的动作（教师段 ~40%）⇒ 学习信号密度只有教师段的 1/2.5，「掺自对弈」基本等于重复标注。（读法：`z_of` 那类"重放核对"不需要；这个检验只要 obs + π + 一个前向。）
- ⭐ **软目标翻案**（`n1_soft_e16.pt`，`--soft-target` T=1，其余与 N1 逐项相同，单变量）：胜率 88.0 / 90.7 / 91.8 vs N1 的 88.3 / 90.8 / 91.9（配对 p = 0.286 / 1.000 / 0.720）⇒ **胜率持平**；**战损每档都更好** —— 裸 47.3 vs 48.0、M100 46.3 vs 47.8、M500 **44.9 vs 46.4**；配对"都赢局"剩血差 **−1.29±0.53pp（2.4σ）** / **−1.26±0.41pp（3.1σ）**；`N1s·M500` 的 44.9% 甚至低于教师100 的 45.8%。终局分、z 均值同向。
- ⚠️ **与历史「软目标 = 更差（四次复现、结案）」矛盾 ⇒ 条件是关键**：那四次的 π 来自教师/漫游（5000 sims、分布更平），本次是自对弈 MCTS(500) 的 π（更尖）⇒ 软目标 ≈ 轻度平滑，正好落在那条轴上。**引用这两个结论都必须带条件**。
- 口径纪律：软/硬目标之间 **`ce` 不可比**（目标分布不同）；可比的只有胜率、战损、终局分、z。
- **训练侧三个旋钮的判决（2026-10-01 晚，`n1_soft_e32.pt` + π 自熵测量）**：
  ① **轮数死**：16 → 32 轮（步数 136k → 273k，ce 从 ep12 起不再动）胜率 裸 88.0→88.1
     （McNemar p=0.678）、M500 91.8→91.7（p=0.678）、战损 44.9→45.1、终局分 −6、z −0.004、
     终局层 21.4→20.9 —— **全在噪声里且方向一致地略差**。
  ② **容量不是瓶颈**：train ce 1.215 / val 1.217，没有过拟合缺口。
  ③ **数据已无余量**：量出标签自熵 —— `pi_act` 全体 **1.076 nats**（教师段 1.171 / 自对弈段
     **0.993**，max π 0.545 / 0.481 / 0.601）；`pi_sel` 只在 72,492 行（3.3%）有定义。
     两类帧**严格互斥**（2,111,207 + 72,492 = 2,183,699）。按同口径拆 `ce`：`1.215 ≈
     0.967·CE_act + 0.033·CE_sel` ⇒ `CE_act ≈ 1.22`，即网络**已把标签分布复现到只差
     ~0.14 nats**，而 32 轮把这 0.14 挪了 **0.000**。
  ⇒ 结论：**轮数/容量/同类数据量都不是杠杆**；自对弈段 π 更尖（熵 0.99 vs 1.17）却零增益，
  与「84% top1 一致」互证 = 自我复读。剩下唯一没动过的杠杆是**让标签分布本身变强**（提 sims）。

- **命名收口（2026-10-02）：权重名 = `<自定义名字>-<数据集名>-<全部配方参数>.pt`。**
  以前是我手写 `n1_soft_e16.pt` / `encs_n1000_s5000_eps0.1.pt` —— 看不出第几轮迭代、看不出是
  模仿还是自对弈、看不出轮数与目标，而且每次显式给 `--out` 就把仓内的命名点 `runtime.ckpt_name`
  整个绕过去了。现在：
  - **数据集名照旧**（它自己的名字够用，不改），权重名里**原样嵌进去** ⇒ 可反查。
  - **自定义名字**由 `train.py --name` 给（`imit` = 模仿 / `sp1`、`sp2` = 第几轮自对弈）；不给就只剩数据集名。
  - **参数全塞**（含默认值），按 `train()` 签名的顺序；只有轮数 `e<N>` 单独排在数据集名后面。
    两个例外：`epochs`（已单列）、`tb`/`tb_dir`（打点，改不了权重，且 `tb_dir` 是路径）。
  - 段间用**连字符**：数据集名自己用下划线（`encdata_n1000_s5000_eps0.1`），两个分隔符不打架。
  - ⚠️ **默认值也要写**：软/硬目标只差 `soft_target0/1` —— 只写"非默认"的话，"硬目标"在名字里
    毫无痕迹，两条 `-e16` 的权重就分不出来。
  - 实测全长 **195~200 字符**；**显示与 TB 运行名用 `runtime.short_name`**：丢掉结构/优化超参段
    （`DISPLAY_DROP` 那 12 个），**保留 `z_spec`/`soft_target`/`soft_temp`** —— 丢了它们软/硬目标
    在评测表里会同名（实测撞过一次）。于是显示成
    `sp1-encdata_sp_s500_sample-e16-z_specenginenorm-soft_target1-soft_temp2`（71 字符）；文件上仍是全名。
  - `--out` 仍在，但降为逃生口（要写绝对路径时才用）。

  完整名示例：
  `sp1-encdata_sp_s500_sample-e16-hidden128-depth2-archflat-sel_archflat-pairnone-dropout0.2-lr0.001-wd0.0001-batch256-z_specenginenorm-soft_target1-soft_temp1-loss_chunk65536-threads32-devicecpu.pt`

  | 旧名 | 新名的识别段 | 尾巴差异（其余同上例） |
  |---|---|---|
  | `encs_n1000_s5000_eps0.1.pt` | `imit-encdata_n1000_s5000_eps0.1-e64` | `soft_target0` |
  | `n1_spmix_e16.pt` | `sp1-encdata_sp_s500_sample-e16` | `soft_target0` |
  | `n1_soft_e16.pt` | `sp1-encdata_sp_s500_sample-e16` | `soft_target1-soft_temp1` |
  | `n1_softT2_e16.pt` | `sp1-encdata_sp_s500_sample-e16` | `soft_target1-soft_temp2` |
  | `n1_soft_e32.pt` | `sp1-encdata_sp_s500_sample-e32` | `soft_target1-soft_temp1-threads1`（原命令没给 `--threads`）|

  本机已改；远端等 `sp2` 收工再改（它的 argv 里写的就是旧路径）。
  `sp2` 那批数据训出来的会是 `sp2-encdata_sp_s2000_sample-e16-…`（`--name sp2`）。
  【未落地】中途我曾做过一版"从数据名自动推代数（`g<N>`）"的方案，因过度设计**已全部删除**。

- **删掉 `pair` 开关，交叉矩阵常开（2026-10-02）**：用户定。`AZNet` / `train()` / `net_cfg` /
  `dump_battle` 里的 `pair` 参数与 CLI flag 全部移除；`FactorPolicyHead` 不再收 `bilinear`，
  `key ⊙ target` 那一项**无条件加**。`--pair` 现在会被 CLI 报 `unrecognized arguments`（已验）。
  - **注意交叉矩阵只在 `arch="factor"` 的头部里**：现役 5 个网络都是 `arch="flat"` ⇒ 这次改动
    对它们**功能上是零**（它们的 `config` 里留着 `pair="none"`，`net_kwargs` 会打一行警告并忽略，
    实测旧权重仍能加载 + 前向）。要用上它就训 `--arch factor`，名字里的 `archfactor` 会自动带上。
  - `runtime.DISPLAY_DROP` **保留 `pair`**：现役那 5 个名字里就有 `pairnone` 段，删了会显示不全。
  - 验证：`py_compile`；flat/factor 两架构构造 + 前向（输出 96/128/1）→ **factor 下无「梯度为 None」
    的参数**（改之前 `n_tar` 恒 None，现在它进前向了）；旧存档加载 + 前向 OK；`--pair` 被拒。
  - 参数量（当前布局，与本条上方旧记录的数字不同源）：flat **327673** / factor **340937**。

- **`collect_part.py`：逐项采集从 `collect_encs` 里拆出来（2026-10-02，用户：「encs 不要 `_collect_single_enc` 这种复用自己的写法」）。**
  以前 `collect_encs.py` 靠 `sys.executable __file__ … --one <id> --floor <层>` **把自己再拉起一个进程**
  当 worker（`main()` 里那个 `--one/--floor` 分支只为这条路存在）。现在：
  - `collect_part.py`（新）= **一项** `(怪组, 层)` 的采集者。CLI 七个 flag 全按 `collect_part()` 的
    签名注册（flag 名 / 类型 / 默认值都从签名来）⇒ **没有第二张 flag 表**；`main()` 直接
    `collect_part(**vars(parse_args()))`，名字对不上就 TypeError。
  - `collect_encs.py` 只做调度：`collect_part.part_argv(sys.executable, enc=…, floor=…, n_per=…,
    sims=…, out=…, eps=…, start_seed=…)` 拼子进程命令行。**漏给或拼错一个名字当场 `TypeError`**
    —— 旧写法漏一个 flag 会静默用子进程默认值（= 采错数据，看不出来）。
  - `ENCS` 表仍归 `collect_encs`；`TASK` / `generate_single_enc_jobs` 挪到执行者那边，
    `collect_encs` 重导出一次 ⇒ `eval_encs.py`（还在按老路径取 `generate_single_enc_jobs`）不受影响。
  - 隔离边界不变（仍是一项一个真子进程）；并发仍是线程池。
  - ⚠️ **旧复核命令作废**（写的是内部 flag）：`collect_encs.py 1 25 /tmp/x.npz --one 9 --floor 12`
    → `.venv/Scripts/python.exe collect_part.py --enc 9 --floor 12 --n-per 1 --sims 25 --out /tmp/x.npz`
    （`--eps` 同名同义；`--seed0` 改叫 `--start-seed`）。同类还有本文两处 `--one 46 --floor 45` /
    `--one 4 --floor 4`，以及 `notes.md` 的 `--one 9 --floor 12`。
  - 验证：`_chk_encs.py` 全通过（真子进程那条路照旧被走到）；`collect_part.py --help` 七 flag；
    `part_argv` 漏参数 → `TypeError: … 缺 ['start_seed']`；`collect_encs.TASK` 与三个下游
    import（`eval_encs` / `collect_walk` / `collect_selfplay`）冒烟通过。
  - 【未落地】`sync.sh` 的 `_JOBS_RE='[p]ython .*(collect_[a-z]+|…)'` 现在也匹配 `collect_part.py`
    ⇒ `status` 的进程列表里会多出短命 worker（在 `head -12` 内、父进程按 etimes 排在前）。未改。

- **采集器缓存名补全参数 + 表指纹（2026-10-02，用户：「把所有参数都写上去」）。**
  口径照 `train.ckpt_params`：名字本身就是一份完整配方，**含默认值**、按调用方签名顺序。
  - `runtime.cache_name` 的 `**extra` **不再排序**（顺序 = 配方签名顺序），值走 `_fmt_val`
    （布尔 → `1`/`0`、浮点 → `%g`）；`runtime.eps_key` **已删** —— 它那条「0 不进名」的规则正是
    本次要取消的。新增 `runtime.recipe_kv(fn, kw, drop)`：按签名挑参数，不做手工白名单，
    漏一个签名参数就 `KeyError`。
  - `collect_encs.default_cache(n_per, sims, eps, start_seed, cache_dir)` = 本表**唯一**拼名点
    （`collect_walk` 与远端流水线都调它）：`encdata_n1000_s5000_eps0.1_start_seed0_fpafc96aec.npz`。
    `fp` = `ENCS` 表的 8 位指纹（`table_fp`）⇒ **改表自动换名**（以前改了表还复用旧缓存）。
  - `collect_selfplay.default_cache`：补上 起点权重 / `eps` / `sample` / `batch` / `start_seed` /
    `rollout_threads` / 掺入数据集。⚠️ 旧名只有 `n`/`sims`，而 `main()` 先拼名、`load_cache`
    见文件存在就直接返回 ⇒ 换起点网络或 `--eps` 会**静默复用别的配方采的数据**（真 bug）。
    权重段 = `<短名>-<权重内容 sha1[:8]>`：`short_name` 丢掉 lr/hidden/depth，只差 lr 的两个网
    短名相同 ⇒ 靠内容指纹区分。
  - `collect_run.default_cache`：以前写死 `data/run_smoke.npz` ⇒ 现在 `run_n5_s50_seed00.npz`。
  - 派生名实测：encs 53 字符（含 `.npz`）/ 自对弈 95 字符（拿 `_smoke1.pt` 当网）。
  - ⚠️ **长度撞墙 ⇒ 交叉引用只留「人给的名字」（2026-10-02 用户定）**：`ckpt_name` 原先把数据集名
    **原样**嵌进去，而权重名本体（全部训练超参）已 ~180 字符 ⇒ 拿自对弈那支（数据名 156 字符）
    派生到 **320** ⇒ 超 255，且是在**训练跑完、存盘那一步**才 `ENAMETOOLONG`。两条新规则：
    · `runtime.dataset_tag(npz)` = 数据名里 **`_n<数字>` 之前的一切**（`[<名字>_]<kind>`）——
      权重名里的数据引用只写这个：`imit2-imit_encdata-e64-…`（实测 178 字符）。
    · `runtime.net_tag(ckpt)` = 权重名的**第一段**（`train.py --name` 给的名字）—— 数据名里的
      权重引用只写这个：`…_netsp1.npz`（原来那 79 字符的 `short_name` 整段没了）。
    · 四个采集器因此都加了 `--name`（同 `train.py`）：`imit_encdata_n1000_…npz`、
      `spd2_spencs_n1000_s500_…_netsp1_miximit_encdata.npz`。
    ⚠️ 代价：引用短了 ⇒ **只有自定义名字能区分**，重名会静默指向错的那份（`train.py` 存盘是覆盖
    语义）。自对弈那支的权重内容哈希（`weights_fp`）随之删除 —— 区分度现在全靠 `--name` 起得准。
  - ⚠️ `train_run.py` 的默认输入仍是 `data/run_smoke.npz`（整局线搁置，未改）⇒ 它与
    `collect_run.py` 的默认输出不再自动接上（已改用显式参数或走 `--out`）。
  - 验证：四个采集器打印派生名与长度；`collect_encs` 走 `cache=None` 真跑一次（分片目录 +
    合并 65 行，完了删掉产物）；`_chk_encs.py` 全通过；`compileall` 干净；`grep eps_key` 无引用。
  - ⚠️ `collect_train_off.sh` 的名字推导同步改成 `from collect_encs import default_cache`
    （不然它算出的路径与采集器不一致，训练会去找不存在的文件）。顺带发现：那条流水线的
    `SEED0` 只进了 `STS_SEED0` 环境变量，**没有**传给 `collect_encs.py --seed0`（`STS_*` 已不是
    读取点）⇒ 采集实际永远用 seed0=0，而日志里打的是 `seed0=${SEED0}`。未改。

- **MCTS：展开挪到前向之后 + 引擎侧一次调用展开（2026-10-02）**。起因是问「azsearch 降到
  onnx+cpp 以绕开 batch 参数是否可行」——先把两个便宜的先手做掉再谈搬家。
  - 新增 C++ 绑定 `BattleContext.expand_children()`（`bindings/slaythespire.h` + `bindings-util.cpp`
    + `slaythespire.cpp`）：一次跨语言调用完成「枚举合法动作 + 逐个 `clone`+`execute`+`action_to_idx`」，
    返回 `([(下标, Action), ...], [孩子 BattleContext, ...])`（两列同序）。
  - `azsearch.mcts_search` 改成**先降 k 条路径、一次前向、再建孩子**（原来是先给**均匀占位先验**
    建好孩子再前向）。同批内的选择不再可能拿到占位先验；`batch=1` 两种写法逐位相同。
  - ✅ **实测：batch=1 的 π 逐位不变**（三个遭遇 × 300 sims，`sha1(pi)` 三个全同：
    `23a8a1137ea4` / `feb11453a020` / `93759bc6ac1d`，与改前基线一致）。
  - ✅ **新的守卫**：`_chk_actions.py` 扫 63 个遭遇，比 `expand_children` 与旧三步路径的
    下标序列 / 孩子快照 / 终局标记——0 个不一致。
  - 代价：`batch>1` 时**首批 k 次评估会全落在根上**（旧写法会把其中 k−1 次摊到占位先验的子节点）。
    实测 `sum(pi)` 296 vs 299（300 sims / batch=4）：丢的就是这 3 次。占比 (k−1)/sims，高 sims 可忽。
  - ⚠️ **时间只快了 ~5%**（300 sims：0.181→0.172 / 0.228→0.216 / 0.232→0.220 s）。
    归因（cProfile，2000 sims 空网/真网）：
    · 前向 batch=1 ≈ 350–400 µs/次，batch=8 摊到 75 µs/样本 ⇒ 前向里大部分是**每次调用的固定开销**；
    · 引擎本身 ≈ 95 µs/模拟：`clone` 5.7 次（~6 µs/次，整上下文深拷贝）+ `execute` 5.7 次（~4.3 µs）
      + `enumerate_actions` 1 次（**~28 µs**）+ 快照 1 次（~10 µs）；
    · Python 胶水 ≈ 40 µs/模拟（建节点 / PUCT 循环 / mask+softmax）。
  - ⇒ **推翻我之前的判断**：那 17 次跨语言往返只值 ~20 µs/模拟，**不是**大头。
    非前向的地板主要是「每个动作整拷贝一份战斗上下文」这个算法选择（C++ 内部，换语言也省不了）。
    真要提速度，杠杆是：① 提高到 batch（已知语义代价）② 让子节点**惰性**materialize
    （每模拟 1 次 clone 而不是 5.7 次）③ 把 `enumerate_actions` 那 28 µs 查清（它每次都新建一个
    `BattleScumSearcher2`）。ONNX/整树搬 C++ 的收益因此比原先估的小得多。

- **`torch.compile` 当推理前向（2026-10-02，两地实测；结论按机器分）**。动机：把「每次调用的固定
  开销」干掉，让 batch=1（原教旨语义）跑得动。测法：`AZNet()` 随机初始化（计时只看架构与算子，
  且本地现存 ckpt 全是旧布局载不进），`torch.set_num_threads(1)` + `torch.inference_mode()`。
  - **前向单次**（µs）：本地 eager b1 **234** / b8 369；compiled b1 **90**（2.6×）/ b8 216。
    远端（Xeon 8352V，单核比本地慢 ~2.1×）eager b1 **496~592** / b8 757~813；
    compiled b1 **319~325**（1.5×）/ b8 359~382（2×，每样本 45~48 vs eager 95）。
  - **整次搜索 300 sims（每次模拟，远端）**：eager b8 ~205 µs ⇒ **compiled b8 ~155 µs（1.3×）**；
    **compiled b1 ~440 µs**。
  - ⇒ ⚠️ **「compile 让 batch=1 变便宜」只在本地成立，远端不成立**：远端删 batch 是**净亏 ~3×**
    （compiled b1 440 vs compiled b8 155）。折成采集一轮（63k 局 × ~17 决策 × MCTS(500) / 32 worker）
    约 +30 min（44 → 74 min，粗算）。**要不要删 batch 得按这条定。**
  - 数值：eager vs compiled 差 4~7e-9（float32 末位；5 个遭遇 × 3 个头最大 7.45e-09）⇒ **不是逐位
    相同**，按本仓口径（一个 ULP 都算行为变更）等于**换了一条轨迹**，与 compile 前采的数据别混训。
  - **`-march=native` 没用**：疑心 inductor 默认只生成 SSE2 ⇒ 用包装脚本
    `exec /usr/bin/g++ -march=native "$@"` 喂给 `torch._inductor.config.cpp.cxx=(None, path)`
    （注意：`cxx` 只能给**可执行文件路径**，写成 `"g++ -march=native"` 会 `InvalidCxxCompiler`），
    结果 325 → **319 µs**，无变化 ⇒ 瓶颈不是向量化宽度。
  - ⚠️ **形状坑**：给 dynamo 扔 >8 种 batch 形状会撞 `recompile_limit (8)`，之后**静默回落 eager**
    （日志里才看得到）。固定 batch + 固定 sims ⇒ 只有 1~2 种形状 ✓；新形状首调本地 6~8 s、
    远端 4~6 s（inductor 磁盘缓存跨进程复用；`%TEMP%/torchinductor_<user>`）。
  - ⚠️ **Windows 要先 `call vcvars64.bat`**（inductor 要 `cl.exe`；`vcvars64.bat` 在
    `...\VC\Auxiliary\Build\`，`cl.exe` 在 `...\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\`）。
    跑 `python <脚本路径>` 时还要给 `PYTHONPATH=<repo>`（`sys.path[0]` 是脚本目录，不是 cwd）。
  - 远端状态（2026-10-02 21:1x 复核）：5 个 tmux 会话全 `dead`、无作业；引擎已按新绑定重编
    （`expand_children: True`）；`nvidia-smi` 不存在（westb 是纯 CPU 机）。

- **推理前向换成 ONNX Runtime（2026-10-02 落地）**：`aznet.inference_net(net)` = 拿权重现导一份 ONNX
  → 返回 `OrtRunner`（接口与 `AZNet` 一致：吃 torch 张量、吐 torch 张量）⇒ `mcts_search` / `net_action`
  一行没改。接手点：`collect_selfplay._init_worker` / `walk._init_worker`（`eval_walk` 走它）/ `dump_battle._load_net`。
  - 实测（远端 32 核 Xeon，单样本）：eager 711 → jit 382 → **ORT 186 µs**（1 线程；32 线程 122）；
    batch=8 每样本 61。整次搜索 300 sims：b=1 **2.1~2.3×**、b=8 **1.25~1.30×**。
  - ⭐ **采集级实测（8 局 @ MCTS(500) batch=8，串行）：14.82 s → 9.28 s = 1.60×**；
    前向从 53%（1135 µs/次）降到 38%（**510 µs/次**）。前向占 54% ⇒ 换成整轮采集就是这 1.6×。
  - ⚠️ **数值：不是逐位相同**（ort vs torch 最大差 **2.98e-08**；ORT 的 kernel 与图融合不同）。
    但 12 个对照（4 遭遇 × 3 batch）里 **π 的 L1 = 0.000、argmax 全同** ⇒ 实际轨迹几乎不动，
    但按本仓口径（一个 ULP 都算行为变更）**它仍是一次轨迹变更**，别把 torch 前向采的数据与 ORT 前向
    采的混在一起解释。
  - 为什么不是 jit / compile：jit 逐位不变但只 1.2×（batch=8）；compile 要 C++ 工具链 + 形状坑
    (`recompile_limit`) + 7.45e-09 漂移；ORT **batch 维天然动态**（一种形状一张图，不重导）、
    只依赖预编译轮子（Windows 不用 vcvars）。
  - 实现要点：⚠️ **每次调用都重新导出**到进程私有临时文件（`aznet_<pid>.onnx`）—— ONNX 的内容同时
    取决于权重与 `aznet` 的代码，按 mtime 缓存会静默用旧图；写权重旁边则 32 个 worker 抢同一个文件。
    `intra_threads` 按调用方并行度给（worker 里 = `rollout_threads`，默认 1）。
  - 新守卫 `_chk_onnx.py`（进 `_chk_all` 的 engine 档）：导出能跑 + 三头逐元素 ≤1e-6 + batch 1/4/8 不重导。
  - 【未接】`realtest/comm_battle.py` 与 `probe_imit` 仍用裸 torch 网（它们不走 MCTS，暂时不必改）。

- **MCTS 的 obs 与先验后处理搬出 torch（2026-10-03）**。接上条：ORT 落地后 b=1 的账是
  「前向 61% / Python 胶水 35% / 引擎 4%」，先抠胶水里的两块。
  - ① **掩码 + softmax 从 torch 换 numpy**。原写法每叶 `torch.zeros(dim, bool)` + 逐项
    `mask[i] = True`（9 次 tensor setitem）+ `masked_fill` + 标量 `softmax` + 5 个 `float()`
    ⇒ 实测 **103.1 µs/叶**；numpy 同活 **12.9 µs**。新写法**只对候选位置归一化**：非法位置的
    `exp` 恒为 0、不进分母 ⇒ 与全量 softmax 等价（观测实测：`sha1(π)` 逐位不变）。
  - ② **新绑定 `sts.batch_snapshot(ctxs)`**（`bindings/slaythespire.cpp`）：一次跨语言调用把 k 个
    BattleContext 的快照按行写成 `(k, snapshot_len)` **float32**。原来逐叶 `get_snapshot_np()`
    是 k 次 int32 分配 + Python 侧 `np.stack` + 再 cast 一次（合计 18.5 µs/叶）。
  - ✅ **π 逐位相同**：三个变体（v0 = 旧 / v1 = 只换后处理 / v2 = 现状）在 CULTIST + MAW
    （b=1 / 500 sims / ORT）上 `sha1(π)` 全同（`dd4d723748d4` / `122ce11448c2`）、L1 = 0、
    argmax 全同；**本地与远端一致**。
  - 实测（远端，300 sims，5 次 min，ms）：

    | case | b | v0 | v1（①） | v2（①+②） | v0/v2 |
    |---|---|---|---|---|---|
    | CULTIST | 1 | 112.6 | 93.9 | **85.6** | 1.32× |
    | MAW | 1 | 113.9 | 97.2 | **90.9** | 1.25× |
    | CULTIST | 8 | 45.3 | 32.6 | **30.9** | 1.46× |
    | MAW | 8 | 50.1 | 34.4 | **32.1** | 1.56× |

    ⇒ b=1 从 ~400 µs/模拟 降到 **~285**，其中 ORT 前向 ~245 µs 已占 **86%** ⇒ Python 侧没有可摘的了。
  - ⚠️ **ONNX 锁死 batch 维没用**（四变体，µs）：dynamic 241.1 / 静态 `(1,1603)` 249.5 /
    静态+复用输入缓冲 242.3 / 静态+IOBinding 预分配输出 239.1。batch=1 本来只有一个形状 ⇒ ORT 的
    kernel 计划与内存早已缓存好，那 241 µs 是**真 kernel 时间**，不是计划/分配开销。
  - 守卫：`_chk_actions.py` 新增 `_batch_snapshot_selftest()`（扫 63 个遭遇，`batch_snapshot` vs
    逐叶 `get_snapshot_np` + stack + astype **逐位一致**）—— 本地与远端均 0 不一致。
  - `OrtRunner.__call__` 现在接受 numpy 或 torch 张量（`batch_snapshot` 给的是前者）。

- **MCTS 树跨步复用（2026-10-03）**：`SearchTree` 走完一步就把那个孩子变成新根、子树连统计一起保留
  （AGZ 原文的 tree reuse）；`mcts_search` 保持「每次从零建树」的一次性语义不变。
  - 地基：复用要求「孩子里的 BattleContext 与真实走完那步的状态**逐位相同**」。新守卫
    `_chk_search.py`（进 `_chk_all` 的 engine 档）钉死三件事：advance 后树根与真实状态逐位一致 /
    `run` 结束全树 `nv == 0` / **步 0 与从零建树逐位相同**；另测「越界 idx」与「状态链断裂」
    **必须报错**（不静默重建）。3 遭遇 × 5 步，本地与远端均 0 异常。
  - ⚠️ **实测：速度上没有收益**（我事前那句「比 batch 的损耗大得多」是错的，收回）：
    `sims=2000 × 6 步` 复用 5313 ms / 前向 **12000** 行 vs 从零 5202 ms / 12000 行（0.98×）；
    `sims=500` 是 1414 vs 1283 ms（0.91×）。**前向次数严格相等** ⇒ 每步仍跑 `sims` 次新增模拟，
    复用不省计算；真实采集吞吐 17.5 → 18.0 场/s（噪声内）。
  - 它确实改了三件事：
    ① **等效算力 +12~34%**：继承的 `root.N_VISIT / sims` 实测 12~34%（战斗后期出现 116%/212%，
       那是跨多步累积的）；
    ② **消掉「首批 k 条路径全卡在根」**：继承的根已有孩子 ⇒ 第一批就能下探（`ΣN` 不再少 k）；
    ③ **战斗后期大幅变快**：继承的子树里已有终局节点 ⇒ 不必重新发现（本地 CULTIST 后期单步
       531 → 36 ms）。
  - 代价：树在一局内累积（**见下条的修正**）；
    **π 变了**（复用 vs 从零 L1 = 23~60 / 200 sims）⇒ 与旧数据不可比，要用就得重采。
  - ⇒ **留不留由胜率定**：它现在不是优化、而是「换成 AGZ 的做法」；唯一可能变成真加速的路径是
    「复用后把 `sims` 降下来而质量不变」，那必须做配对胜率测试。

- **权重内容指纹（2026-10-03）**：AZ 那条借鉴的第一条 —— 名字（`--name`）是**人给的标签**，同一个
  标签训两遍是两份完全不同的网，而光看名字分不出（`runtime.net_tag` 的注释自己写着「重名会
  **静默指向错的那份**」）。
  - `ckpt.weights_fp(weights)` = `sha1[:8]`（键名 + dtype + 形状 + 字节，按键名排序）。`save()`
    **自动**算并写进存档；`read()` 对**老档**（加指纹之前）现算 ⇒ 读侧不必分两种存档
    （实测老档现算值 == 存档时算的值）。
  - `collect_selfplay.default_cache` 的数据名加 `wfp<8>`（紧跟 `net<tag>`）⇒ 起点权重换了内容，
    数据名自动变。远端实测：`spencs_n63_s500_batch8_..._netsp1_wfpb8ffbcca.npz`。
  - ⚠️ **没有**把它写进 npz 自描述：`mix_caches` 有两道会直接炸的检查 —— 「B 多出 A 没有的键」
    直接报「不是同一套 schema」，非 `_SCALARS` 的键还会被 `np.concatenate` ⇒ 「只有一侧有且是
    标量」的新字段两边都过不去。要让**内容**带身份得先给混训加规则（**未做**）。
  - 验证：同权重存两次同值 / 改一个参数（1e-6）即变 / 老档现算一致 / 本地与远端名字一致。

- **`AZNet.forward` 接受 numpy（2026-10-03，修行回归）**：`SearchTree.run` 的 obs 改由
  `sts.batch_snapshot` 产出（numpy）后，我只教了 `OrtRunner` 吃 numpy ⇒ **裸 `AZNet`（torch 路径）
  接不住**（`_gate_select_region` 里 `is_sel.unsqueeze` 当场 `AttributeError`）。
  - 它藏了一整天，因为**所有验证都只走 ORT**（采集/评测/守卫全是 `inference_net`）。触发它的是
    「重采期间量 torch vs ORT 的差距」—— 也就是问「改回 torch 损失多少」时。
  - 修：`forward` 入口 `if not isinstance(obs, torch.Tensor): obs = torch.as_tensor(obs)`
    （torch 张量零开销）。守卫：`_chk_search.py` 加 **`_torch_path()`**（裸 torch 网也必须能跑
    MCTS），本地与远端均通过。
  - 顺带量到（本地，比例与远端一致）：改回 torch 的损失 = **b=1 2×** / **b=8 1.34×** /
    **b=32 1.08×**（单次前向本身差 3.6×，摊到整次搜索只剩 2×）；远端 `torch.compile` b=1 仍比
    ORT 慢 1.7× ⇒ **不改回**（它换来的「权重可变」只需 0.3% 代价就能模拟）。

- **`MCTSNode` 删掉 `parent`（2026-10-03）**：接上条 —— 树复用暴露出一个一直存在但以前无害的内存
  问题：节点回指父节点 ⇒ `parent ↔ children` **循环引用** ⇒ 被 `advance` 丢掉的子树**不能靠引用
  计数回收**，只能等分代 GC；而 `parent` **从未被读过**（搜索只向下走，回传靠 `path` 列表）。
  - 实测（用 `gc.get_objects()` 数 `MCTSNode` 实例）：改前 6 步活节点 240→477，而 gc 里总数
    1959→**10053** ⇒ **垃圾 9576 = 活节点的 20 倍**，每个还持着一个 `BattleContext` 深拷贝；
    `gc.collect()` 后立刻回落到 477（确认就是循环引用垃圾）。改后**垃圾恒为 0**。
  - 内存（远端 RSS，sims=500）：改前 6 步 603→654 MB（+51）；改后 8 步 602→610 MB（**+7**，
    活节点峰值 1042 时只 +3.5）⇒ 真实树内存 ≈ **7 KB/节点**。上一轮报的「33 KB/节点 / +100 MB /
    32 worker 要盯内存」是被垃圾撑大的，**全部撤回**（一场 30 步 ~3000 活节点 ≈ 20 MB）。
  - 行为**不变**：同权重同参数的 630 场自对弈给出**完全相同**的 10089 决策点 / 胜率 288/630；
    吞吐 18.0 → 18.5 场/s（噪声内）。
  - ⇒ 纯修复。以前不暴是因为**一次性搜索的树活不过一次调用**，不存在「丢子树」这件事；
    跨步复用一上线，它就从「无害」变成「每步丢掉上千个节点」。
  - 守卫里的第 ④ 条（`_chk_search.py`）就守着这个：`gc.get_objects()` 里的 `MCTSNode` 必须
    全部从 `tree.root` 可达。有效性已反向验证：给一个将被丢弃的节点**注入自环** ⇒ 判为
    「未回收」 83 个（干净时 0）。
  - ⚠️ **写这条时先吃了个假阳性**：第一版把「越界/错位测试」和回收检查放在同一个循环里，
    于是上一轮的 `t2`/`t3` 两棵树（还绑在局部变量上）被算进「未回收」（MAW 107、
    THREE_SENTRIES 128；CULTIST 因为是第一轮、还没有 `t2`/`t3` ⇒ 0）。同理，用列表收集
    `gc.get_objects()` 的诊断脚本**自己也持有**它统计的那批节点。⇒ 这类「已回收」检查必须
    让被测对象是**进程里唯一活着的那个**（现在拆成了 `_reuse_chain` / `_must_raise` 两个函数）。

- **`ckpt.save` 改成原子替换（2026-10-03）**：为「采集侧热重载最新权重」铺路 —— 旧的
  `torch.save(payload, path)` 是直写，并发的读侧会读到**写了一半**的文件。
  - 改法：先写同目录的 `<path>.<pid>.tmp`，再 `os.replace`；异常时清掉 tmp（旧档保持完好）。
  - ✅ 实测（远端 Linux/overlayfs，1.3 MB 权重、写 400 次、读侧狂读 8 s）：
    静态对照（无写侧）**0 坏档**；旧实现 **4934/11858 = 41%** 读到坏档（坏档当时的文件大小从
    `0` 到完整 `1313247` 的各种中间值 —— 典型的「读到写了一半」）；新实现 **0 坏档**
    （写 400 次产生 60 种 inode ⇒ rename 确实在换 inode）。
  - ⚠️ **Windows 上 `os.replace` 会抛 `PermissionError [WinError 5]`**：目标文件正被 reader 打开，
    而 Windows 的 `open()` 不带 `FILE_SHARE_DELETE`（POSIX 的 rename 没有这个问题，本机已复现）。
    部署在 Linux ⇒ **未加重试**；将来本地并发读写同一份 ckpt 时会需要。
  - ⚠️ **流程教训**：第一次在远端跑这个对照时**忘了 `sync.sh push`** ⇒ 「新实现」那一臂跑的
    还是旧 `ckpt.py`，于是得到「新旧都是 ~5000 坏档、**inode 只有 1 种**」的假结果，白查一轮。
    改完代码在远端验证前必须先同步；**「写 N 次只产生 1 种 inode」就是没换成新实现的判据**（本次
    正是靠它定位的）。

- **新增脚本 `eval_encs.py`（2026-10-01）：按怪组 × 出现层分别评测。**
  构造照 `collect_encs.py`：直接复用它的 `ENCS` 表（测试覆盖 == 采集覆盖）+ **逐项一个子进程**
  （隔离边界，也是「每项固定场数」的来源）+ 分片复用 + 逐项战报；差别只在**不产数据、只产读数**。
  造环境走 `collect.make_stage_battle`（与采集同一份实现），策略装配走 `walk._init_worker`
  （与 `eval_walk` 同一份），所有策略共用同一段 seed ⇒ **同 seed 配对**。
  与 `eval_walk.py` 的分工：walk 答「整局能走多远」（漫游、终局层/z；怪群那格的分母由走图运气
  决定，act3 的怪要等整局活到 act3 才有样本）；encs 答「某个怪组打得怎么样」（场是造出来的，
  层固定、牌组由 `shape_floor` 按档发，格与格可比）。
  - **口径修正（写这个脚本时发现）**：战损必须**只算胜场**。败场 `t_hp = 0` 恒贡献 100% 损失，
    混进来会让这一列变成胜率的复述，且一列两个含义（实测冒烟里 89.4%/81.2% 就是在复述 33%/52%）。
    输出改标「战损（仅胜场）」，无胜场时打 `—`。
  - **分片参数闸**：分片里记 `n_per`/`seed0`/`specs`，任一不同即**报错**而不是静默复用
    —— 否则换策略续跑会得到半新半旧的混合读数，而控制台上看不出来（`--force` 强制重跑）。
  - 冒烟（本机，`data/_smoke1.pt` + `teacher:200`，每组 3 场）：63 项 / 378 场 / **0 撞限** / 43 s；
    同参数重跑 0 s 全复用、换 `n_per` 立刻报错退出。

- **`sync.sh start` 起远端作业的两个操作坑（2026-10-01 各踩一次）**：
  ① 评估臂的**真语法**只有 `net:<path>[:<sims>]`（逗号分隔），对话里常用的 `net:x(裸/M500)`
     简写**不是**语法 —— 抄进命令会被远端 shell 当括号报 `syntax error`，作业 0.3 秒即死；
  ② `start` 必须**显式**给 `STS_BGLOG=...`，否则写默认 `run.bg.log`（共用日志会被 `script(1)`
     截断），而 `wait` 盯的若是另一个名字就会**静默空转**（实测白等 25 分钟）。
  ⇒ 规矩：起完作业先 `head` 真日志确认"参数解析正确 + 已开跑"，别只信 `start` 的"已启动"回显。

- 新增旋钮 **`--soft-temp`**（目标 = `π**(1/T)` 再归一化；T=1 与旧实现逐位相同，已用数值对照验证；只对 `--soft-target` 生效）。TB 的 CE 记录门槛改成 `min(10, max(1, epochs // 4))`（16 轮 ⇒ ep4/8/12/16 四个点，长训练仍是每 10 轮）。
- **温度方向（`n1_softT2_e16.pt`，T=2）= 与 T=1 打平**：胜率 裸 87.2 vs 88.0（−0.8pp，McNemar p=0.061）、M100 91.1 vs 90.7、M500 **92.1 vs 91.8**；战损 裸 46.7 vs 47.3、M100 46.6 vs 46.3、M500 **44.4 vs 44.9**（配对"都赢局" Δ剩血 −0.73±0.40pp，1.8σ）；终局分 / z 均值 / 场每局 / **终局层** 在 M500 上全部同向更好。⇒ 没达到事先定的"再好 ≥1pp"门槛 ⇒ 读作「T=1 ≈ T=2，更平对**裸网略有害**、对**加搜索略有益**」；⚠️ 这一批做了 5 次比较，1.8σ 不算证据，别过度解读。锚点 `N1·M500` 与上一轮逐位相同（91.9%）⇒ 跨 JSON 比较成立。

### 2026-10-01 · `train.py` 把「未知参数」两个方向的静默都堵上

- **问题**：① `argparse` 的 `allow_abbrev` 默认开 ⇒ `--hidde 256` **被当 `--hidden` 的唯一前缀缩写吃掉**（拼错 = 静默改义，不是报错）；② `train()` 新加一个 keyword-only 配方参数而忘了登记 `RECIPE_FLAGS` ⇒ 该参数**永远只能用签名默认值、CLI 一点提示都没有**（反方向已经会响：`recipe()` 的 `sig[k]` 当场 `KeyError`）。
- **改法**：`allow_abbrev=False`（主 parser + 两个子 parser）；新增 **`check_recipe()`**（`main()` 里、`_start_watchdog` 之前）= 断言 `train()` 的 **keyword-only 形参集 == `RECIPE_KEYS`**，不等就列出「签名多 X（CLI 调不到）/表多 Y（`train()` 收不到）」并 `sys.exit(2)`。判据只能是 keyword-only：`data/seed/val_frac/verbose/tag` 是调用方给的，不是配方。
- **验证**：`train.py train x.npz --hidde 256` ⇒ `error: unrecognized arguments`（rc=2）；两方向的闸各注入一次（改 `RECIPE_KEYS` 后调 `check_recipe`）都 rc=2 且报对方向；`_chk_encs.py` **全部通过**（它真的 spawn `train.py train`）；仓库内所有 `train.py` 调用都用全名（`probe_capacity.py:131` 逐项钉死 flag）⇒ 关缩写零影响。
- ⚠️ **仍未堵的第三类**（当时问的是"未知参数"，这类是"未知取值"）：`z_spec`/`arch`/`sel_arch`/`pair`/`device` 是自由字符串，拼错会在**读缓存之后**才炸（`z_from` 的 `KeyError` / `AZNet` 的构造）。要做早必须是 `choices=`，而口径清单现在只隐含在 `zspec.z_from` 的分支里 ⇒ 得先在 `zspec` 立一个 `SPECS` 常量（单一取值点），属于设计决定，未动。

- 🔧 **同日稍后推翻**（用户：「那你写那么多有的没的干什么，脚本参数直接读取，然后直接塞进 `train` 函数，不匹配直接爆炸不行吗」）：本条上面的 `check_recipe()` 及其依赖 —— **`RECIPE_FLAGS` / `RECIPE_KEYS` / `check_recipe()` 全部删除**，`recipe()` 改名 `train_kwargs()`。新形状：
  · **flag 从签名派生**：`recipe_flags(sp)` 遍历 `train()` 的 keyword-only 形参 —— 名字派生 flag 名、`type` 取默认值类型、`default` 取签名默认值、help 取一张只影响 `--help` 的小表 `HELP`。两个例外单列：`tb`（历史上写 `--tb 0/1`，用 `type=int, choices=(0,1)`）与 `epochs`（`train` 里是位置参数，不参与派生）。
  · **参数直传**：`train_kwargs(cli)` = `vars(cli)` 滤掉 9 个非配方名（`JOB_KEYS`），补 `epochs` 默认值、`tb` 转 bool，然后 `train(**kw)`。⇒ 新加 flag 忘登记、名字拼错，都在这一步当场 `TypeError`，不再需要"表 ⇄ 签名"断言。
  · 副产品：flag 带上签名默认值 + `ArgumentDefaultsHelpFormatter` ⇒ `--help` 显示 `(default: 128)`。⚠️ 坑：argparse **只在 help 文字非空时**才追加默认值 ⇒ `HELP` 每项都必须有字。
  · 验证：`py_compile`；`train --help`（16 个配方 flag 全带默认值）；`--foo`/`--hidde` 仍报错；`_chk_encs.py` 全过（真 spawn `train.py train … --tb 0`）；`seeds` 用 1 场小缓存真跑通（两条子命令路径都覆盖）。605 → 582 行。

### 2026-10-01 · `eval_walk.py` 逐场落终局列 ⇒ 战损 / 终局得分可逐怪读

- **动机**：逐怪只有**胜率**一把尺；要看**战损**（1 − 胜局平均剩血比，历史口径）与**终局得分**（引擎 `evaluateEndState` = `zspec.Terminal.t_escore`），必须逐场留终局读数。
- **做法（不新增汇总口径）**：`_eval_battle` 用 `azaction.terminal_stats(b)` 取六列（必须在 `exit_battle` **之前**），返回槽 3 由 `None` 换成 `term`、`info` 补 `floor`；`_evaluate` 逐场落一行 **`battle_rows`**（`seed/idx/floor/act/room/enc` + `zspec.Z_COLS`）—— 胜率、战损、终局得分、按 **(怪群, 层)** 拆表、跨策略**逐场配对**全从它现算。原 `per_act`/`per_room`/`per_encounter` 保留（速览视图，形状不变）。顺带把 `z` 的内联式 `hp/max_hp if win else −1` 换成 `zspec.z_from(term, "hp")`（历史 `hp` 口径，逐位等价）。
- **验证（本地 A/B**：`eval_walk.py 20 "net:data/_smoke1.pt" 100000000 … --workers serial`，旧版 = `git show HEAD:eval_walk.py`）：旧键 16 个**逐位相同**（含 `per_act`/`per_room`/`per_encounter`/`z_mean`/`z_var`/`win_rate`）；新增 `battle_rows`/`hp_left`/`hp_loss`/`escore`/`zeng`/`turns`；`len(battle_rows) == battles`；从 `battle_rows` 自算的战损/终局分/均回合与 harness 内算一致。含 MCTS 与 `random:` 的多策略冒烟亦通过。
- ⚠️ **战损只统计胜局** ⇒ 选择效应：胜率更高时「边际胜局」是更难的场次，会把战损**拉高**（不是打法变费血）。跨策略比战损必须胜率接近，否则拿 `battle_rows` 取两策略**都赢**的交集局做配对。「终局得分」含败局低分 ⇒ 同样与胜率耦合。
- 复核：`python eval_walk.py 20 "net:data/_smoke1.pt" 100000000 /tmp/x.json --workers serial` ⇒ 汇总表多出「战损/终局分/回合」三列，JSON 出现 `battle_rows`。

### 2026-10-01 · `ActionQueue<50>` 溢出：Maw 的 `NOM` 段数随回合涨 ⇒ 改压回合上限（`MAX_TURNS = 96`）

- **现场**：远端 `collect_encs.py 1000 5000 --eps 0.1 --workers 32`（63 项）跑完 62 项，唯一失败 `MAW@45`（分片 `46@45.npz`）退出码 **-6**（SIGABRT）：`include/combat/ActionQueue.h:88 pushBack [capacity=50]: Assertion 'false' failed`。「一项一个子进程」是隔离边界 ⇒ 只丢这一项，其余照跑。
- **定位（两步，都可复用）**：① **种子可隔离** —— `generate_single_enc_jobs` = `[(start_seed+i, enc, floor) for i in range(n_per)]` 且每场 rng 独立（`random.Random(seed*1000003+11)`）⇒ **`--seed0 S --n_per 1` 精确等于批量的第 S 场**；② 16 路并行扫 seed 0..999 ⇒ **seed 711** 崩，`gdb -q -batch -ex run -ex bt --args $PY -u … --seed0 711` 一次拿到完整栈。
- **栈与机理**：`Monster::attackPlayerHelper(times=53)`（`Monster.cpp:601`）← `Monster::takeTurn`（`MonsterSpecific.cpp:1419` = `THE_MAW_NOM`）← `BattleContext::executeActions` ← `search::Action::execute` ← `BattleScumSearcher2::playoutRandom`。`attackPlayerHelper` 是**一次性批量**入队 `for (i < times) addToBot(AttackPlayer(...))`，而 Maw 的 `NOM` 传 `t = (getMonsterTurnNumber()+1)/2 = (turn+2)/2` ⇒ **段数随回合增长**；崩时 `times=53` ⇔ `turn=104`。**崩点在搜索的随机 playout**（playout 策略打不死 Maw ⇒ 拖到 100+ 回合），因此与 `eps` **无关**（seed 711 在 `eps=0.0`/`0.1` 下都 abort）。全仓 28 处多段攻击调用里**只有 Maw 的 NOM 段数非常量**（其余 ≤ 15）。
- **修法（用户定调「和最大步数一样，按住最大回合数，超过直接判负」）**：`BattleContext.cpp` 里既有的 `turn > 500 ⇒ outcome = Outcome::PLAYER_LOSS` 保留语义、阈值降到新加的文件内 `constexpr int MAX_TURNS = 96`。取 96 的依据：`96 ⇒` NOM 最多 **49** 段（入队时队列必为空 —— 主循环先排空 `actionQueue`/`cardQueue` 才 `doMonsterTurn`），留 1 格余量。**不动 `ActionQueue<50>`**：它在搜索里被大量拷贝，扩容等于给搜索加内存带宽。
- **为什么没改成「自链式动作」**（评估后否掉，留个记录）：① `Actions::AttackPlayer` 是用 `clearOnCombatVictory=false` 构造的（怪死后剩余段仍要打），自链必须对齐这一位；② 更关键 —— **续段只能推队尾**，会落到本段 `addToBot` 的后插项**之后**（唯一的后插点是 `Player::attacked` 里攻击者带 `PAINFUL_STABS` 时的 `addToBot(WOUND)`；`hpWasLost` 的遗物触发全是 `addToTop`、`cards.onTookDamage()` 不入队），而原实现里后续段**本来就排在它们前面** ⇒ 顺序不再一致。要「构造上严格一致」只有扩容队列这一条路。
- **影响面 = 0（已验）**：本批 62,000 场里动作最多的一场是 **118 动作 / 49 回合**（`GIANT_HEAD@45` seed 711；用**修复前**的远端引擎重放读出 `t_turn=49`）⇒ 全部 < 97 回合；控制流上阈值下调只在 `turn > 96` 时才分岔。`_chk_all.py` 改动后仍 **10/10**。
- ⚠️ **改的是 C++**：本地必须走 CMake Tools 重编（Debug）；远端同步后必须 `cmake --build build -j32`（换了 `.so` 才算生效）。
- 复核（一条）：`collect_encs.py 1 5000 /tmp/x.npz --one 46 --floor 45 --seed0 711 --eps 0.1`（修前 SIGABRT/rc=-6，修后 rc=0）。
- 回归检查就用上面那条命令。中途写过 `_chk_queue.py`（手推 Maw 长局：每回合血两边拉满 + `end_turn()`，子进程捕 abort；反向敏感性已验证：把 `MAX_TURNS` 改回 500 则它必红），用户要求撤掉 ⇒ **已删、不进总检**，别再加回来。
- 同日 `train.py` 清理（用户：「太啰嗦，代码逻辑混乱」+「毫无解释性的大 `over` 参数」）：636 → 557 行。
  · **配方管线合并**：`recipe(**over)` + `recipe_from_cli(a)` ⇒ 一个 **`recipe(cli: argparse.Namespace)`**（显式、有类型、无哑参数；`**over` 那种写法会**静默吞掉拼错的键**）；`RECIPE_FLAGS` 从「(flag 名, kwargs) 元组」改成**「配方键 → argparse 参数」一张字典**（flag 名由键派生：`sel_arch` → `--sel-arch`），`RECIPE_KEYS = (*RECIPE_FLAGS, "epochs")`；`NET_KEYS` 改由 **`inspect.signature(net_cfg)` 派生**。⇒ 同一件事的清单从 4 份（`RECIPE_KEYS`/`NET_KEYS`/`RECIPE_FLAGS`/`train()` 签名）降到 **2 份**。
  · ⚠️ **上面摘要条里的 `recipe(**over)` / `recipe_from_cli(a)` 是旧形状**（那两条名字已不存在）。`--job-timeout` 明确**不进配方表**（它是作业环境），在 `build_parser()` 里与 `--epochs` 并排挂。
  · `if __name__ == "__main__":` 里那坨（嵌套 def + 两个子命令内联分支）拆成 `build_parser()` / `load_cache()` / `run_train()` / `run_seeds()` / `main()`；`np.load` 由「每键重开一次」改一次；`seeds` 的 fork/`--save` 两个参数闸挪到读缓存之前；删死 import（`slaythespire`/`runtime`/模块级 `time`）。
  · 注释去叙事留契约（日期/事故复盘/「见 notes 续四十」全删，只留判据与不变量）；`_chk_encs.py` 全通过（它真的 spawn `train.py train`）。

### 2026-09-24 → 10-01 · 规则与不变量摘要（原 VS Code 记忆侧正文，2026-10-01 并入；**摘要层**，细节看下面各条）

- 快照布局当前 **1603**（网络输入 **2115**；参数量 h=128/d=2/flat = **327673**）（2026-09-30 复核：`python -c "import slaythespire as s, aznet as A; print(A.OBS_DIM, s.snapshot_layout()['len'], A.AZNet().trunk[0].in_features)"`）。构成：**pile_slots 60→128**（引擎牌堆是 `std::vector`、无编译期上限 ⇒ 容量只能实测；`_chk_trunc.py 10 200 1000` 实测最大单堆 **23**、余量 105）+ **deck_slots 64→96** + `card_select_slots` 跟着 pileSlots ⇒ **SELECT_DIM 60→128** + **怪物块 12→9 列**（删 halfDead/escapeNext/isEscaping）。改布局 ⇒ 旧缓存/旧 ckpt 全作废（宽度变的靠 `obs_dim` 闸当场挡；**只换列序、宽度不变**的靠 `collect.check_cache_obs` 的重放对账挡）。

- **牌堆写盘 = 容器序、不截断（2026-09-30，用户：「牌堆不需要堆顶优先」）**：`appendPile` 不再 `pile[n-1-j]`（原堆顶优先 + 丢堆底），改成按容器序铺 + 补零；网络对这三块只做**集合均值池化**（`aznet.card_feats`）⇒ 槽序不可见、不构成序泄漏。判据已改：`_chk_snap.py` 现在比**非零槽的多重集 == 引擎那堆**（不再断言 slot0 == 堆顶）。

- **行 0 约定 =「空槽、可学」（2026-09-30 定案，用户当天先要 padding、随后回退说「算了 保留对 0 学习」）**：`card_embed` / `emb_monster_id` / `potion_embed` 一律**不加** `padding_idx` —— 占位符是**可学的表示**；因此空槽要从池化的「和」里排除**只能靠掩码**（`aznet.card_feats` 的 `blk * mask`，分母数真实张数）。⚠️ 整局网（`runnet` / `multi_hot`）相反：那边行 0 是 padding、恒 0 不学 —— 别把行约定搬错网。复核：`python -c "import aznet;n=aznet.AZNet();print(n.card_embed.padding_idx, n.potion_embed.padding_idx, float(n.card_embed.weight[0].abs().max()))"`（应为 `None None` + 非零）。

- **`moveHistory` 语义 + `escapeNext` 是死列（2026-09-30 查证）**：`Monster::moveHistory[2]` 唯一写入口是 `setMove()`（`moveHistory[1] = moveHistory[0]; moveHistory[0] = moveId;`）。**`[0]` = 待执行（下一次轮到它要做的）**：开局 `MonsterGroup::init` 逐怪 `rollMove`；`takeTurn` 直接 `switch (moveHistory[0])` 执行并在分支末尾 `addToBot(Actions::RollMove(idx))` ⇒ 行动后立刻 roll 下一条；玩家侧 `intentMoveId`/`intentDamage`（`getMoveBaseDamage` 也 switch `[0]`）都是"将要做的" ✓。**`[1]` = 刚执行过的**（快照**没导出**）。引擎自己在 `rollMove` 里把 `[0]` 当"刚做完的那条"读（`lastMove`/`lastTwoMoves`/`firstTurn()`）—— 同一槽位两种读法，靠"行动后立刻 roll"接上。佐证"待执行"性质的写法：`onHpLost` 把大史莱姆掉半血时**当场改写** `moveHistory[0] = ..._SPLIT`（直接赋值不 shift）；`die()` 里觉醒者/暗灵 `setMove(AWAKENED_ONE_REBIRTH/DARKLING_REGROW)`。⚠️ **`Monster::escapeNext` 全仓无任何赋值**（只有 `Monster.h` 声明 `= false`）⇒ 快照 `escapeNext` 列**恒 0**（= 零信息列，与已删的 `outcome` 同类），`MonsterSpecific.cpp:650/660/670` 的 `doesEscapeNext()` → `GENERIC_ESCAPE_MOVE` 分支是**死代码**；真正会逃跑的怪走 `LOOTER_ESCAPE`/`MUGGER_ESCAPE`（`setMove` 进去，`isEscapingB` 是独立字段）。复核：`grep -rn "escapeNext" src include bindings`（除声明外无赋值）。

- **总牌组块 64→96（2026-09-30，用户：「管理器本来就存不下大于 64」→ 其实两张表**：`Deck::MAX_SIZE = 96`（run 牌组）vs `CardManager::MAX_GROUP_SIZE = 64`（战斗侧三个堆））**：`BattleContext::masterDeck` 抄的是 **run 牌组**却按 64 截断（`std::min(nDeck, masterDeckCap)`）⇒ 实测 70 张的牌组会**静默丢 6 张**且 `counts.deckSize` 只报 64。现态：`masterDeckCap` 别名删除、`std::array<int, Deck::MAX_SIZE>` 由容器自身定容、`masterDeckSize = gc.deck.size()` **不截断**，`Snap::deckSlots = Deck::MAX_SIZE`。复核：`python -c "import slaythespire as s; L=s.snapshot_layout(); print(L['deck_slots'], L['deck_off'])"`（`deck_slots` 应为 96）。

- 远端（2026-09-30 现役）：`ssh -p 48068 root@connect.westb.seetacloud.com`，**纯 CPU / cgroup 32 核 / 251 GB / `autodl-tmp` 50 GB** / py3.12.3 + torch2.12.1（`cuda.is_available()==False`）；RDIR=`/root/autodl-tmp/sts_lightspeed`；PY=`/root/miniconda3/bin/python`（远端 PATH 无 `python`）。`_sync.sh` 的 HOST/PORT 默认值已改到这台；`.agent/REMOTE.md` 有完整换实例清单。

- **旋钮取值点 = 全显式参数（2026-09-30 定案）**：每个脚本自带 argparse，库函数收形参（`collect_jobs(workers=…)`、`train(threads=…, device=…, tb=…)`），`_env.py` 只剩纯工具（`workers(v)` / `mp_context(mp_start)` / `cache_path(name, dir)` / `ckpt_name` / `DATA_DIR`；`default_workers` 已于 2026-09-30 并入 `workers`）。**唯一还读 env 的 = `STS_BGLOG`**（作业日志路径，shell 注入，不是旋钮）。迁移**已完成（2026-09-30）**：`collect` / `collect_encs` / `collect_walk` / `collect_selfplay` / `_walk` / `_eval_walk` / `_az_train` / `_tb` / `_chk_threads` / `_chk_encs` / `_chk_pool` / `_imit_probe` / `_dump_battle` / `_probe_capacity` / `_audit_data` 全部改成形参 + CLI flag；`_gen_eval` / `_eval_encs` 已退役。**唯一例外 = `_run_train.py`（整局线，搁置）**：仍用它自己那套 `STS_RUN_*` env + 自己的 `TRAIN_THREADS` 常量，待迁。复核：`grep -rn "_env\.\(train_threads\|rollout_threads\|device\|tb\|tb_dir\|cache_dir\)" *.py` 应为空。

- **`train()` 的配方 = 扁平 keyword-only 形参（2026-09-30）**：`train(data, seed, val_frac, verbose, tag, *, hidden=128, depth=2, arch="flat", sel_arch="flat", pair="none", dropout=0.2, lr=1e-3, wd=1e-4, epochs=64, batch=256, z_spec=DEFAULT_SPEC, soft_target=False, loss_chunk=65536, threads=1, device="cpu", tb=True, tb_dir=None)`。**默认值只在签名里一份**；`RECIPE_KEYS` = 清单、`recipe(**over)` 按签名补齐（CLI flag 默认全 `None` ⇒ "不写 flag" 与 "写默认值" 同一条路径）、`recipe_from_cli(a)` 映射 argparse。**建网 kwargs 的唯一组装点 = `_az_train.net_cfg(...)`**（别与 `aznet.net_kwargs(cfg)` 混：后者是把**存档里**的配置规范化 / 容忍旧副档）。`save_ckpt(net, path, cfg=…, z_spec=…)` 显式收这两项 ⇒ 存档自描述仍成立（实测 `--hidden 32 --z-spec hp` 落进存档）。`_tb.writer(tag, sub, on, tb_dir)` / `fault(..., tb_dir)`（记住最近 writer 的目录写 `faults.jsonl`）。**`EVAL_SEED0` = `collect.py` 的模块级常量（1e8）**，`_eval_walk.py` 的种子闸读它、不在任何脚本里重定义（`_probe_capacity` 原来那个 `= 20000` 已改成 import）。`_chk_threads.py` 直接 `train(..., threads=n, tb=False)`。复核：`grep -n "RECIPE_KEYS\|def net_cfg\|def recipe" _az_train.py`。

- **`_walk.py` 库化 + 事件房照进（2026-09-30）**：删 CLI（`main`/`summarize`/`MIN_PAR`），只剩 harness + 策略装配（`parse_spec` / `_init_worker` / `make_choose` / `run_one` / `play_battle`）；事件分支改用 `gc.valid_event_options` 取最低合法位（掩码 0 才离开）。`_init_worker(cfg)` 的 `cfg = (spec, batch, threads)`，**建网配置读存档**（`_ckpt.config_of` + `aznet.net_kwargs`），不再依赖 `apply_recipe()`。消费者：`_eval_walk.py`（主评）/ `_chk_shape.py`（守卫）。⚠️ 副作用：`_chk_shape.py` 原先靠 `_walk` 顺带 `import _env` 得到 UTF-8 `reconfigure`，现已把那段 preamble 显式写进自己。证据：`_chk_shape.py` 全通过；`_eval_walk.py 4 "random,teacher:50" 100000000` 跑通（池 16 workers）、`--workers serial` 也跑通。

- **远端构建配置 = `Debug`（2026-09-30 定案，别再改回 Release）**：`include/sts_common.h` **无条件** `#define sts_asserts`，但那些 `#ifdef` 块体里写的是 **C 的 `assert`** ⇒ `-DNDEBUG`（Release）下全仓自检**静默失效**。可见后果：`_chk_encs.py` 的前提检查「`--one 0`（INVALID）必须非零退出」在 Release 不成立 ⇒ `_chk_all` 恒 **14/15**；危险后果：`enumerateActionsForNode` 的 `default:` 与 `playoutRandom` 的 `edges.empty()` 之后都紧跟 `uniform_int_distribution(0, size-1)` ⇒ 断 assert 后是 **UB**。换 Debug 后远端 **15/15**。⭐ **实测「不付代价、不作废数据」**：同参数采同一批（`collect_encs.py 5 500`，315 场/4405 帧）两种构建的缓存 **17 列全部 0 差异**（含 double 列 `t_escore`）、胜率都 67.0%、耗时都 8 s；自对弈 32 worker 0.21 s/局（Release）vs 0.19 s/局（Debug）⇒ `-O0` 与 `-O3+LTO` 在这类负载上无浮点分歧，**不用重采**。⚠️ 跨**编译器**仍不可比（MSVC 44.8% vs gcc 46.2%）。

- **自对弈吞吐实测（2026-09-30，sims=200 / batch=1 / 每 worker 单线程）**：本地 16 worker **0.44~0.75 s/局（4800~8100 局/h）**；远端 westb 16 worker **0.28 s/局（12780 局/h）**、32 worker **0.21 s/局（17375 局/h）** ⇒ **远端总吞吐是本地 2~3.6×**（虽然远端单核慢 2.5~2.8× —— 本地并行效率只有 ~25%：单决策串行 106 ms，16 worker 实测只降到 ~73 ms/决策）。⚠️ **探针必须与 `_init_worker` 同口径**：远端 `torch.get_num_threads()` 默认 **32**，32 个 worker 各开 32 线程 = 1024 线程 ⇒ 实测 **46 s/局**（假读数，差 100×）。**任何自对弈基准都要先 `torch.set_num_threads(1)`**。

- **怪物块 = 9 列（2026-09-30 定案）**：`[0] monsterId  [1] intentMoveId  [2] curHp  [3] maxHp  [4] block  [5] targetable  [6] intentDamage  [7] intentHits  [8] miscInfo`。用户先要求把 moveId 提到第二列，随后定调「halfDead / escapeNext / isEscaping 回到简单的 `isDeadOrEscaped`」⇒ **这三列删掉**（12→9 字段）：生死/去留**只有 `targetable`（= `!isDeadOrEscaped()`）一列**，「为什么不能打」写在**意图 id** 里（实测：觉醒者一阶段 / 暗灵倒下当帧意图就是 `AWAKENED_ONE_REBIRTH` / `DARKLING_REGROW`，逃跑的 Looter/Mugger 停在 `LOOTER_ESCAPE`/`MUGGER_ESCAPE`，永久尸体停在最后一次滚出的招式）。⚠️ `Monster::escapeNext` 全仓**无任何写入**（只在 `Monster.h:47` 声明为 false）⇒ 它那列本来就恒 0，`GENERIC_ESCAPE_MOVE` 那三条 `if (doesEscapeNext())` 分支是死代码。顺带：**`MonsterMoveId` 已绑成真枚举**（197 项，`monsterMoveStrings` 名字表循环填，全是合法标识符、无重名）；`azlayout.REVIVE_MOVES` = {AWAKENED_ONE_REBIRTH, DARKLING_REGROW, DARKLING_REINCARNATE} 是"待复活"的语义依据。`azlayout.MONSTER_EXTRA_OFF` 仍从 `CAT_IDX` 派生（现在 = (2..8)，7 项）。复核（一条）：`.venv/Scripts/python.exe -c "import slaythespire as s,azlayout as a;L=s.snapshot_layout();print(L['len'],L['monster_fields'],a.MONSTER_EXTRA_N,s.MonsterMoveId(13).name)"`（应为 `1603 9 7 AWAKENED_ONE_REBIRTH`）。手写字段号的消费者：`_chk_snap.py`(+2/+5/+8)、`_chk_revive.py`(`_monster_field(1|2|3|5|8)` + `move_name()`)、`_dump_battle.py`(M_NAMES / r[2] / 意图按名显示)、`_gen_eval.py`(`_MON_HP_IDX` +2)、`_chk_scale.py`(`_NAMES`)、`realtest/comm_map.py`。⚠️ 快照列数变了 ⇒ 旧缓存/ckpt 全废（`obs_dim` 闸会当场挡）。

- **怪物读数只有两条路（2026-09-30，用户：「没有存在的必要，全部删除」）**：绑定里那 6 个 `get_monster_hp|get_monster_max_hp|get_monster_block|get_monster_move_id|is_monster_targetable|get_monster_id` **已全部删除** —— 查证结果：只有前两个有调用者（`collect.terminal_stats` 的 `t_enemy`、`rlenv.RunEnv._sig` + reward），后四个零使用。连带删掉只服务它们的 `monsterIdx()`（那唯一的越界校验）。现在读怪 = ① **快照怪物块**（唯一带 id / 意图 / targetable 的地方）② **活对象** `b.monsters[i].cur_hp|max_hp|block`（`def_readwrite`，可写 —— `_chk_revive.py` 就是靠它改血调场景）。⚠️ `b.monsters[i]` 走 `MonsterGroup.__getitem__`（`bindings-util.cpp::getMonster`）= **带界检查、越界抛 `IndexError`（2026-09-30 加，见文末）**⇒ 枚举一律用 `for m in b.monsters`（== `arr[0..monsterCount)`，含尸体）或 `b.monsters.count`；**`Monster` 绑定没暴露 `id` / `moveHistory`**，要那两项只能看快照。复核：`.venv/Scripts/python.exe -c "import slaythespire as s; print([n for n in dir(s.BattleContext) if 'monster' in n])"`（应为 `['monster_count', 'monsters']`）。

- **缓存不存 `z`（2026-09-30 定案）** —— 用户：「干嘛要存old口径，改名，并且使用一个数据结构替代这个dict」：
  - `_zspec.Terminal` = **一场**战斗的终局记录：字段名 == 缓存列名、`Z_COLS = Terminal._fields`、**类型按字段本义**（`t_win/t_hp/t_max_hp/t_turn: int`、`t_enemy/t_escore: float`），缓存列 dtype 由 `Z_DTYPES` **从 `Terminal.__annotations__` 派生**（`int` 字段不存 float64 —— 与整局侧 `collect_run` 本来就用 int64 一致）。**只有一种粒度**（逐场；缓存里逐行存只是广播），不要引入 `int|float|ndarray` 那种并集。`z_from(t, spec) -> float` 吃单场；`z_of(d, spec) -> ndarray` 逐场算一次再按 `bid` 广播（**前提自检**：`bid` 升序且每场连续 + 终局列同场内逐行相同，不成立当场抛）。
  - 口径：`DEFAULT_SPEC = "enginenorm"`（`_az_train.Z_SPEC`、`collect_selfplay` 的打印、`_flags --z-spec` 帮助文本都引用它）；**`old` → `hp`**（赢 = `t_hp/max(1,t_max_hp)`、负 = −1）。实测 `hp` 口径与**旧缓存里的 z 逐位相同** ⇒ 历史数字可复原。换口径 = 训练侧旋钮，**不必重采也不必重放**。
  - `collect.BattleRecord` = `(seed, traj, term, enc, floor)` —— **删了 `z`，字段位置全变**：别再按位置取（`collect_encs.py` 的胜率打印写 `t[1] > 0` 就因此炸过），用 `r.term.t_win > 0`。`collect_jobs` / `_drain` 的胜率判据同样改成 `r.term.t_win > 0`（不再用 `z > 0` 反推胜负）。`assemble` **不写 `z` 列**。
  - `_z_revalue.py`：**`rew` 子命令已删**（只留 `stats` / `zsplit`，且必须显式给缓存路径 —— 原来那条无参分支引用的 `DEFAULT_HALF` / `seed_of` 全仓根本没定义过，一直是 NameError）；重放自检改为**终局列逐位比对**（比原来的 `|Δz| < 1e-6` 更强、且不再依赖任何口径）。
  - 顺带删掉两处重复：`_az_train` 的「旧口径重算 == 缓存 z」自检（它检的就是「z 是终局列的第二份拷贝」）、以及 `_z_revalue.rew` 写的 `zspec` npz 列（**全仓无读者**）。
  - ⚠️ 旧缓存/旧 ckpt 全废（快照布局 + 缓存 schema 都变了）；`mix_caches` 会因键集不同响亮报错。
  - 复核：`python -c "import numpy as np,collect as C;from _zspec import z_of,Terminal;print(Terminal._fields)"` + 造一份小缓存看 `'z' in d`（应 False）。

- **坑⑨（`x is 哨兵` 不收窄 Pylance 联合类型）**：哨兵若是**普类实例**（`STOP_BATTLE = _StopBattle()`），`if got is STOP_BATTLE: break` 之后 Pyright **仍认为 `got: Got | _StopBattle`** ⇒ `got[0]` 报「未在类型"_StopBattle"上定义"__getitem__"方法」。实测（临时探针，后删）四种写法：裸模块单例 ✗、`STOP: Final = _Stop()` ✗、**Enum 成员 ✓**、**`isinstance(got, _StopBattle)` ✓**。`collect.run_battle` 现在用 `isinstance`（`_StopBattle` 是私有标记类、唯一实例就是这个哨兵 ⇒ 与 `is` 语义等价；`rlenv._apply_action` 也是 `isinstance` 风格）。**2026-09-30 已整层删除哨兵**（`_StopBattle`/`STOP_BATTLE` 与那三处返回全删，`Label = Callable[[sts.BattleContext], Got]`）⇒ 收窄问题随之消失；这条坑只在「以后再引入哨兵/联合返回」时有用。（同类坑：`get_errors` 工具对新文件可能返回**陈旧/空**结果 —— 判定类型错误要用 Pylance LSP 的 `textDocument/diagnostic`。）

- **树的值只有一个尺度 + 终局读数下沉（2026-09-30）**：`terminal_stats(b) -> Terminal` 从 `collect.py` 移到 **`azaction.py`**（`collect` / `_z_revalue` 改 `from aznet import terminal_stats` —— `azsearch` 不能 import `collect`，会成环）。`azaction.terminal_value(b, spec=DEFAULT_SPEC)` 旧义（胜 = 留血比、败 = −1，即 `hp` 口径）**改成 `z_from(terminal_stats(b), spec)`**，即**与训练口径同尺度**；`azsearch.MCTSNode.__init__(..., zspec=DEFAULT_SPEC)` **在构造时**就用它给 `tv`（`mcts_search` / `mcts_action` 多一个 `zspec` 形参透传，默认 `_zspec.DEFAULT_SPEC`，语义上必须等于 `_az_train.Z_SPEC`）。原状态是终局叶 `hp` 尺度（败 = −1.0）vs 头值 `enginenorm`（败 ≈ 0~0.2）**混进同一个 `W/N`** = PUCT 静默错权。⚠️ **副作用**：Q 的赢/输落差从 1.0+ 缩到 **0.477** ⇒ `c_puct=1.5` 是给旧尺度调的，**该按 ~0.48 重扫**（`.agent/MIGRATION.md` 有同向提示）。复核（一条）：`.venv/Scripts/python.exe -c "import collect,aznet as A;from collect_encs import jobs;b=collect.make_stage_battle(*jobs(1,0)[0])[1];print(A.terminal_value(b))"`（初始态应 ≈ `+0.0004`，不再是 −1.0）。

- **`sims ≥ 2` 是 Python 侧 PUCT 的下界（2026-09-30）**：第 1 次迭代只展开根、不访问任何根边 ⇒ `sims=1` 时根边 N 全 0 ⇒ π 全零（实测根边总票 `sims=1 → 0`、`sims=2 → 1`（= 网络先验 argmax）、`sims=3 → 2`（第 2 票才由 PUCT 用「走一步后的叶价值」选出）。`collect_selfplay.collect_selfplay()` 入口 `sys.exit` 挡 `sims < 2`。对照 C++ `BattleScumSearcher2`：一次模拟 = 下降 + 展开 + **rollout 到终局** + `evaluateEndState` 回传 ⇒ `sims=1` 就访问一条根边（`teacher_action(b,1)` → π = one-hot），且 Q 靠 `best−min` 自归一化、无先验、无批量；两侧 `sims` 语义不同，**数字不可互换比较**。

- **`_StopBattle` 已整层删除（2026-09-30，用户：「反正我不想要 _StopBattle」）**：`collect.py` 的 `_StopBattle`/`STOP_BATTLE`、`run_battle` 的 `isinstance` 分支、`collect_selfplay.sp_battle.label` 的三处返回全删；`azsearch.mcts_search` 的 `if not cand`（非终局必有合法动作 ⇒ 不可达）与 `mcts_action` 的 `if not counts: random_action(b)` 一并删（`counts` 空只可能在 sims=0，而调用方都以 `SIMS == 0 → net_action` 分流）。实证不可达/只在 sims=1 发生：`not cand` 从未出现、`pi.sum() <= 0` 只在 sims=1。它原来会**静默记一场假终局**（实测 sims=1：`traj=0`、`t_hp` = 开局 HP、`t_turn=0`）并进胜率分母。复核：`grep -rn "STOP_BATTLE\|_StopBattle" --include=*.py .`（应空）+ `_chk_all.py` **15/16**（唯一红项 = 已知真机桥接）。

- **引擎越界 bug 已修（2026-09-30）：`for m in b.monsters` 在 `monsterCount == 5` 时 abort（`rc=3`、零 traceback）** —— `bindings/slaythespire.cpp` 的 `__iter__` 原来写 `make_iterator(&self.arr[0], &self.arr[self.monsterCount])`，`monsterCount == arr.size()` 时 `&arr[5]` 是**越界元素访问**（MSVC Debug 的 `std::array::operator[]` 检查 ⇒ 当场 abort；Release 不检查 ⇒ 只在 Debug 暴露）。现改为 `self.arr.begin() + self.monsterCount`。影响面：`azaction.terminal_stats` 的 `[m.cur_hp for m in b.monsters]` ⇒ **每场 5 怪战斗（`LOTS_OF_SLIMES` 等）的终局读数都会崩**，采集/自对弈/评估全线静默丢数据。同批 `getMonster` 加界（负数/越界 ⇒ `IndexError`）。复核（一条）：`.venv/Scripts/python.exe -c "import slaythespire as s;print(s.snapshot_layout()['len'])"` 只能验布局；崩溃复现要跑 `collect_encs.py 1 25 /tmp/x.npz --one 9 --floor 12`（应 rc=0 并打出一行 `LOTS_OF_SLIMES@12`）。⚠️ 排查同类 `rc≠0` 必须 `> log 2>&1` 落盘：`| tail` 会吞掉 abort 进程的全部输出。⚠️ **远端 `.so` 当时仍是旧绑定** ⇒ 同步后必须 `cmake --build build -j 32 --target slaythespire`（Debug）重编。

- **采集调度 = 四步 + 线程池（2026-09-30）**：`collect_encs.collect_items` 现在 = ① 列清单 ② **剔除已有分片（在进池之前）** `todo = [it for it in items if force or not os.path.exists(it[2])]`（`items` = `[(enc, floor, 分片路径)]`） ③ `ThreadPoolExecutor` 逐项拉起 `_launch_single_enc(enc, floor, n_per, sims, eps, start_seed, part)`（参数表与子进程入口 `_collect_single_enc` **同名同序**，只差末位 `part`/`out`；每项一个 `subprocess` = 原生 abort 的隔离边界；**载荷只装逐项变的 `(enc, floor, part)`**，`n_per/sims/eps/start_seed` 全表共用、不进载荷）④ `_classify` 判 rc/分 ok+failed，返回值 = **本次采成功的 + `skip`（已有分片）那批**。⚠️ 坑：**全复用时可不能只回本次实采的**（`todo == []` ⇒ `_classify([])` 给空 ok）⇒ 抛「所有项都失败」而其实全都成功；**现行做法**（2026-09-30 晚，用户改过一轮）= 不早返回，而是 `return _classify(res, len(items)) + [(int(e), int(f)) for e, f, _part in skip]`（`skip` 无条件拼在后面）+ 循环后打一行 `复用已有分片 N 项，本次实采 M 项` —— ⚠️ **该字面量是 `_chk_encs.py` 的判据**（字符串匹配），改措辞必须同步改守卫。（早返回那版已不存在；`todo`/`skip` 变量名与逐项 `-todo:`/`-skip:` 打印是用户选的。）`multiprocessing.Pool` 与 `mp_start`/`--mp-start` 在 `collect_encs`/`collect_walk` **已删**（`collect.py`/`collect_selfplay.py`/`_eval_walk.py`/`_az_train.py` 的还在，那边是真进程池）。复核（两条）：`collect_encs.py 1 25 /tmp/x.npz --workers 16`（首采 63 项 / 1061 行）→ 同命令再跑（应打印「复用已有分片 63 项，本次实采 0 项」、0 项实采、缓存逐列无差异）。

- **类型注解（2026-09-30）：`collect_encs.py` 全部 42 个参数已写明类型**（`Job` 从 `collect` 引入复用；路径类 `str | None`、`workers: str | int | None`、`ok/pairs: Sequence[tuple[…]]`、内部分片目录用 `Sequence[Mapping[str, np.ndarray]]`）。复核：`python -c "import typing,collect_encs as C; print(typing.get_type_hints(C.collect_items))"`（`get_type_hints` 能解析 = 无未定义名/前向引用问题）。⚠️ **坑：`np.savez(f, **data)` 与 `dict[str, np.ndarray]` 冲突** —— numpy 桩里 `savez` 有具名参数 `allow_pickle: bool`，把 `data` 标成 `dict[str, np.ndarray]` 后 Pylance 报「无法把 ndarray 分配给 allow_pickle」（实测 `_concat_caches` 收紧返回值就撞上）⇒ 这种缓存 dict **保持 `dict[str, Any]`**。⚠️ `listMissingAnnotations` 会把 `dict[str, Any]` 报成 `reason=explicitAny`，那是**故意的**，别为了消它去收紧。⚠️ `_chk_encs.py::check(name, cond, extra="")` 的 `extra: str` **注解过窄**（5 个调用点传 int/dict/list）⇒ 报 5 个 `reportArgumentType`，正解是 `extra: object`（**未改**）。

- **失败现场 = 原文全打（2026-09-30）**：`_launch_single_enc` 不再嗅探异常行（旧法只认 `Error`/开头 `✗`，漏 `Traceback`/`Assertion failed`/中文）、不再 `note[:240]` 截断、删掉那条**会撒谎**的兜底（写死「原生 abort（退出码 3）」，与旁边的真实 rc 冲突）；现只剔 tqdm 进度条行、**保留行内缩进**，原文整体回传，`_classify` 里**成功打一行摘要 / 失败打完整原文**。代价：全失败时日志长（可接受）。复核：造一项 `INVALID@4` 跑 `collect_items`（应打印 `失败（退出码 3）：` + `（子进程无任何输出 —— 原生终止？）`）。

- **`_concat_caches` 检查全前移（2026-09-30）**：非空 / 键集 / 四个标量（`space`/`obs_dim`/`task`/`src`）/ **每片 `bid` 取值恰为 `0..k-1`**（k = 该片**场数** = `len(np.unique(bid))`）四条**全在动手之前一次查完**，之后只搬运；错开量 = 前面各片场数之和。⚠️ **`bid` 不是逐行连续**（一场多行共用同一个值，1 场 11 行 ⇒ `bid=[0]*11`），别写成 `bid == arange(len(bid))`。⚠️ 原来的收尾 `assert got == want` 是**空检查**（错开量 = 累计 `max+1` ⇒ 位移后必不重叠，构造保证），它漏过 `bid=[0,5]` 这种带空洞的片（那会让 `merge_and_save` 的 `bincount` 静默错位）⇒ 已删，换成上面那条真正的前置检查。复核：重合并 `/tmp/o1.npz` 的 63 片，缓存 sha 应仍为 `e577caa3f67947a6`。⚠️ 标量比较已从 `str(a) != str(b)` 换成 **`not np.array_equal(a, b)`**（判定更严：`1603` vs `'1603'` 判不等；同值不同 dtype 宽度放行）。⚠️ **别把 `task`/`src` 改成 int 列**：数字比较确实不再需要 `str()`（`array(1) != 1` → `np.False_`，进 `if` 没问题），但这两列是**分类标签**（名字才是含义；`space`/`obs_dim` 是宽度标记、数字即含义），换 int 后裸 `np.load` 看不懂，而缓存真会被裸看；账：省 ~43 KB vs `obs` 列 6.5 MB，且 dtype 一变旧缓存的 src 校验会**响亮报错**（= 作废）。

- **`task` / `src` 取值域 = `StrEnum`（2026-09-30）**：`collect.py` 定义 `class Task(StrEnum)`（`ENCS`）/ `class Src(StrEnum)`（`TEACHER`/`MCTS`）—— 缓存 schema 件定义点就在那儿（`assemble` / `load_cache` 同文件）。`assemble(trajs, task: Task, src: Src)`、`load_cache(cache, src: Src)`。`StrEnum` 成员**就是 `str`**（`hash` 同、`np.array(Src.MCTS)` → `array('mcts')`、f-string 直接拼）⇒ **落盘字节不变、缓存/ckpt 不作废**。⚠️ **取值域枚举不能当 dict 键**：`d[Src.TEACHER]` 是查键 `'teacher'` ⇒ `KeyError`；正确用法 = 比值 `str(d["src"]) == Src.TEACHER` 或 `Src(str(d["src"]))`（顺带做取值域校验）。⚠️ **缓存列名仍是裸字符串** —— 十几个消费点，且 `_zspec.Z_COLS = Terminal._fields` 靠“NamedTuple 字段名即列名”自动同步，别改。⚠️ `_dump_battle`/`_eval_walk`/`_walk` 里的 `"teacher"` 是 **policy spec**（`teacher:50`），与 `src` 列不同空间。复核：`grep -rn "assemble(.*\"teacher\|load_cache(.*\"" --include=*.py .` 应为空；自对弈写新缓存读数应仍为 63 场 / 1339 决策点 / 胜率 38.1%。

- **教师参数 = 闭包，不是模块级全局（2026-09-30）**：`collect.py` 的 `_T` / `_init_teacher` / `teacher_battle` **已删**，`make_teacher(sims, eps)` 返回带参闭包。原来用全局是因为 `Pool` worker 必须可 pickle（参数只能挂模块级状态、靠 `initializer` 装）—— 但那条路**已经没人走**：`make_teacher` 只有 `collect_encs.py` 子进程内一个调用者，`teacher_battle` 作为 Pool worker 零调用者。⚠️ **判据 = "pickle 那条路是否活着"**：`collect_selfplay._init_worker` + 它的全局**要留**（worker `sp_battle` 真被 pickle 进池）。⚠️ 代价：丢掉「用 Pool 跑教师」的现成入口（远端 fork 池比 spawn 63 个子进程便宜），现在没人用。复核：`grep -rn "_init_teacher\|teacher_battle" --include=*.py .` 应为空；同参数重采 `CULTIST@4` 应与改前分片**逐位相同**（实测无差异）。

- **真机桥接守卫已退役（2026-09-30，用户：「让那个一直报错的东西滚出去！」）**：`realtest/_chk_comm.py` 从 `_chk_all.py` 的 `CHECKS` 摘掉，连带 `LOCAL_ONLY` 与「⊘ 跳过」机制全删（只服务这一个条目）⇒ **engine 档 11 → 10 项**。理由：它**长期红**（`comm_map.py` 的 `_FALLBACK` 停在旧布局 ⇒ `KeyError: 'potion_bag_off'`），而修它要先定「真机 JSON 拿不到 `halfDead/escapeNext/isEscaping/miscInfo` 这 4 列怎么填」。⚠️ **桥接代码保留**在 `realtest/`，且它是**真坏**（`comm_map.py` 写快照那段也用旧键，真机跑会崩）—— 要单独跑守卫：`python realtest/_chk_comm.py`。复核：`python _chk_all.py --list` 里不应再有「真机桥接」。

- **✅ `teacher_action` 的 `rng` 契约已收口（2026-09-30，走 B）**：`rng: random.Random` **必填**且函数体**不再容忍 `None`**（`(random if rng is None else rng)` 已删）。11 个调用点全部传 `random.Random(0)` 或 `_rng`（`collect.py` / `collect_run.py` / `_chk_mask` / `_chk_pool` / `_chk_res` / `_chk_revive`×3 / `_dump_battle` / `_walk`）。复核：`grep -rn "teacher_action(" --include=*.py .`（不该出现无 rng 的调用）。

- **⭐ `eps > 0` 分支曾直接崩：拿随机动作去访问表里反查（2026-09-30 晚）** —— `azaction.teacher_action` 原来写 `chosen = int(np.random.choice(dim, p=mask/mask.sum()))` 然后 `action = next(a for a, i in visited if i == chosen)`，而 **`visited` 只装被搜索访问过的动作**（`stats.policy` 里 `n > 0`）⇒ 随机抽中的冷门动作不在里面 ⇒ **`StopIteration`**。`eps=0` 时 `chosen` 来自 `argmax(pi)`/π 抽签、必然在访问集合内 ⇒ **这条路径从没被跑过**。
  · 实测（远端 63 项 × 2 场 @ 5000 sims `--eps 0.1`）：**27/63 项退出码 1**，清一色**多怪遭遇**（`SMALL_SLIMES`/`GREMLIN_GANG`/`EXORDIUM_THUGS`/`LOTS_OF_SLIMES`/`AUTOMATON`/…）—— 多怪动作空间大（逐个目标 + 卡选），5000 sims 访问不完。
  · 修法：`visited` 由 `list[(Action, idx)]` 改 **`dict[idx, Action]`**（取值 O(1)，**结构上消灭「反查不到」这一失败类**）；`eps` 分支直接 `legal[rng.randrange(len(legal))]`（`legal = list(b.enumerate_actions())`，本来就遍历过一次做 mask）—— 随机动作**允许**未被访问（其 `pi` 为 0，这正是 eps 语义）。
  · ⚠️ **顺带修的更严重的一条**：原来 eps 掷币用注入的 `rng`、而**抽动作用 `np.random`（全局流）** ⇒ 「同 seed ⇒ 串行/并行逐位相同」**根本不成立**。现在随机性一律走注入流（`tau` 分支的 `np.random.choice` 也改成 `rng.choices`；`tau=` 全仓无显式传参 ⇒ 零实际行为改动）。
  · 守卫：`_chk_encs.py` 从「五件事」扩到**六件事** —— 新增 `eps = 1.0` 全表真采（每步都换随机动作 ⇒ **必然**踩冷门动作）+ **同 seed 重跑逐位相同**。上机：修前 126 场 → **36 分片 / 27 项失败**；修后 → **63 分片 / 0 失败**。
  · 复核（一条）：`.venv/Scripts/python.exe collect_encs.py 2 200 /tmp/x.npz --one 4 --floor 4 --eps 0.1`（`--one 4` = `SMALL_SLIMES`，修前必 `StopIteration`）。

- **采集耗时标定（2026-09-30 实测，`sims` 与墙钟）**：`CULTIST@4` 20 场单进程 = **50 sims → 112 ms/场、500 → 145、5000 → 457** ⇒ 固定开销 ≈ **100 ms/场**（建局 + 按层发卡组/遗物/hp），搜索边际 ≈ **69 µs/sim**。**别拿训练直觉外推**：预算 ×10 只贵约 3×。63 项 × 1000 场 @ 5000 sims（= 63,000 场）在 westb `--workers 32` 实测 **ETA ≈ 53 min**（不是天）。

- **`_env.workers` 合并（2026-09-30）**：`default_workers()` 调用者只有 `workers()` 自己 ⇒ 摘掉，三态解析保留（`None`/空 = 本机默认、`'serial'`/`0`/`'off'` = `None`、否则按数值）。⚠️ 合并时撞出潜伏 bug：`str(v or "")` 把 **int `0`** 变成 `""` ⇒ 返回 16 而不是 `None`（argparse 只给字符串 ⇒ 线上没炸）；现 `str(v).strip().lower() if v not in (None, "") else ""`。复核：`workers(0) is None`、`grep -rn default_workers` 为空。

- **远端缺 `tmux`（2026-09-30 现场）**：westb **没装** tmux，而 `_sync.sh` 的 `start/attach/kill/status` 全靠它。`apt-get install -y tmux` 直接失败（`Unable to locate package`，**索引是空的**）⇒ 先 `apt-get update` 再装（3.2a，`/usr/bin/tmux`；`conda install -c conda-forge tmux` 也可）。**换实例后的必做项**。

- **🚨 遗物 `GAMBLING_CHIP` 会让整项采集作废（2026-09-30 深夜，已修）**：`collect_encs.py 1000 5000 --eps 0.1 --workers 32` 跑完 rc=0，但战报只有 **2 项 / 2000 场** —— **61/63 项退出码 1**，报错全部是
  `azaction.py:114 mask[b.action_to_idx(a)] = True` → `RuntimeError: 动作无法编码进统一下标：不认识的动作类型`。
  · 链路：`ActionType` 有 5 个成员，`actionToIdx` 只写 4 个分支 ⇒ **`MULTI_CARD_SELECT` 落 `default:` 当场抛**；引擎里产生它的只有 `CardSelectTask::EXHAUST_MANY`（Elixir）/`GAMBLE`（Gambler's Brew）**外加遗物 `GAMBLING_CHIP`**（`BattleContext.cpp:452`，回合开始强制进弃牌多选屏）。实测开局即选牌屏的 41 局 **41 局都带它**；比例随层上升（floor 4→1/400、30→10、50→25、**53→0**）⇒ 与「只有 53 层的 2 项活下来」吻合。上游源码在枚举处写着 `// just dont deal with this right now`。
  · 修法（用户裁决 A，与 `BANNED_POTIONS` 同构）：`scene.BANNED_RELICS = (RelicId.GAMBLING_CHIP,)`，`grant_shaped_relics` 里「抽到就 `gc.remove_relic` 重抽」（档位/件数仍由引擎表定，只多耗 RNG）。⚠️ 只在**无拾取副作用**的遗物上成立；⚠️ `shape_floor` 是共享的 ⇒ `collect_encs`/自对弈/评估/多守卫同雷。
  · 守卫：`_chk_shape.py` 第 7 件（4 层 × 150 seed：遗物块不含 `BANNED_RELICS` + 开局输入态不是 `CARD_SELECT`）。**灵敏度反向验证过**：清空禁用表 ⇒ floor 50 当场 25/400 复现。
  · ⚠️ **进度条会骗人**：失败项是快速崩掉的，`as_completed` 照样计数 ⇒ 看着像我 50/63「正常」。**判作业成败只能读日志里的战报/失败行**。

- **训练线程数实测（2026-09-28，三台机器）**：探针 `_tmp_ab28.py`（**同一进程内**跑 1 与 8 线程两臂，带 `assert torch.get_num_threads()==th`），固定 `data/_ab28.npz`（400 场 @500 sims / 12811 样本）+ `train(seed=0,epochs=64)`：
  · **换线程数 = 换轨迹**（远端 2.8：`val_ce` 1.3232 vs 1.3066、`max|Δw|` 0.6465；本机 2.13：1.3210 vs 1.3316、0.8947）⇒ `--train-threads` 是配方的一部分。

  · **同线程数跨进程逐位可复现**（远端两遍、本机两遍，以及 **3 路并发 × 8 线程超订** 三进程全同）⇒ 旧笔记「8 线程下跨进程**偶发**不确定」**没复现**。
  · **跨 torch 版本也逐位相同**（远端 2.8.0+cu128 与 **新实例 2.12.1+cu130** 的 sha/val_ce/`patho` 梯度全同）⇒ 0.0166 的差是**确定性 kernel 分块效应**。差异幅度随步数放大（32 ep 仅 0.0007）。
  · `patho` 臂（值目标 ×1e5、loss≈5.95e9）两端 `max|Δg|=0` ⇒ 「单步算子逐位相同」仍成立，但推不出「整段训练逐位相同」。
  · ⚠️ 勘误：我先前记的「本机 1 vs 8 逐位相同」**是错的**（旋钮 `STS_TRAIN_THREADS`→`--train-threads` 迁移后两臂可能同线程数）。已写进 `.agent/memory/sts-lightspeed-rl.md` 续十三 + `_az_train.py` 两处注释。

- 作业纪律：一律 `./_sync.sh start ...`（**脚本与会话名均必填**——2026-09-24 定调：目标选择无默认值；日志 `job-*.log`）；完成判据 = 日志尾部 `COMMAND_EXIT_CODE` 的**数值**（137=SIGKILL≠完成）。**观看 = attach（唯一语义）：`STS_SESSION=<名> ./_sync.sh attach`（pane 里是任务本尊）；完成触发器 = `wait`（静默：只等完成标记、不转发内容；`watch` 别名已删）；`status`/`log` 保留为一次性点读**。⚠️ 我的 agent 终端 pty 恒 80×30（`stty cols` 无效）⇒ 我 attach 会**把窗格拖成 80 宽**（窗格跟随客户端；远端 mux 宽度锁 2026-09-24 已撤除）。

- **坑⑤（2026-09-24 实测，代价 2GB 日志 + 82 分钟空烧）：不要在远端作业运行期间改快照布局 / 重编 `slaythespire.so`。** 父进程持有旧 `slaythespire` 模块，而**每个新起的 pool worker 会重新 import** ⇒ 拿到新布局 ⇒ 与父进程传来的旧 ckpt 不匹配：`load_state_dict` 抛 `size mismatch for scale: 1076 vs 1151` / `trunk.0.weight: [256,1306] vs [256,1381]` ⇒ multiprocessing pool **无限重启 worker** 刷屏（实测 **218 KB/s**，75 分钟写 978 MB，两份）。⚠️ **最坏的地方是它静默**：`_chain.sh` 仍报「✅ 完成（exit=0）」并接力下一棒（因为父进程的退出码是 0）。⇒ 改布局前先 `./_sync.sh status` + `ps` 确认没有作业在跑；发现刷屏日志就先 `kill` + 把日志截断（`head -c 40000 f > f.head.log; : > f`）。

- **坑⑥（2026-09-24，教训）：绝不要用 `rm` + 重建去改「用户正打开/可能在编辑」的文件。** 我对 `azlayout.py` 这么干了一次（为了整体重排），而用户当时也在改同一个文件 ⇒ 他的编辑被我的版本覆盖，他反馈「你把 X 删掉了」而我的磁盘版本里 X 明明在（因为那是我写的）—— 于是我们又多花了一轮在"我到底改了什么"上。**改大文件只能用增量替换；确实要整体重写就先问，并把回滚路径摆出来**。同源：用户会**撤销**他不认的编辑（`MonsterEncounters.h`、`azlayout.py`）⇒ 动手前先读当前磁盘内容，别依赖自己上一轮的认知。

- **漫游采集（`_collect_walk.py`，2026-09-23 v3 定调 → 同日 v4 资源加量）**：漫游**只当战斗的真实触发器**（贡献遭遇组合/act/floor/顺序/结局；败⇒局终止、终幕 Boss 胜⇒通关）。其余资源每场开打前**原地重置+随机**：`gc.reset_run_resources()`（新绑定：清并重建 牌组/遗物/药水、**遗物池重灌**——`returnRandomRelic` 无放回抽池、不重灌会越抽越少；`initRelics` 是追加式必须先 clear；max_hp/cur_hp 回基础）+ 卡组/遗物/药水按层缩放随机（p=floor/48；**v4（2026-09-23 用户定调）**：白2→8/蓝1→5/金1→1+2p/咒0→2p **+ 随机升级 randint(4p,8p) 张**（新绑定 `gc.upgrade_random_cards`→`Deck::upgradeRandomCards`，不手挑）；遗物白1→5/蓝0..1+2p/**Boss=min(2,act-1)**；**药水 randint(0,3) 不跟层数**；v3 旧配比作废）+ hp=u(0.35,0.95)×max_hp、金币=0..99+14/floor。同一局**复用同一个 GameContext**（不再每场新建）⇒ 战斗走引擎正常 `b.init(gc)`/`b.exit_battle(gc)`，奖励/Boss 遗物/换幕/通关/战败全由引擎处理。`walk_fp`=walk-**v4**⊕boss fp（指纹 `6f516c…`→`a16f8f4c…`，v3 及以前缓存作废）；`seed` 列=整局 seed（battle_split 按局切）；Ascension=0（对齐真机）。多怪覆盖 ~47%。⚠️ **n800 一批的幕覆盖：act1 89% / act2 11% / act3 = 0**（场级 floor≥32 仅 13 局、全折在 Act2 Boss；13/800 = 1.6% 摸到 act2 Boss、0/13 过关）——act3 要靠万局涓流或**直接构造**（见 notes 续三十三/三十四）。

- 绑定杂项（2026-09-23）：`obtain_relic`/`obtain_random_relic` 返回值是 **opensScreen**（是否弹选牌屏），不是"是否获得"（`GameContext.cpp` `return opensScreen`）——别当成功判据；`Deck::removeAllMatching` 有**越界隐患**（`Deck.cpp` 循环从 `cards.size()` 起，应 size-1；仅事件分支用，未修，勿踩）。

- 公平性纪律与评测口径（2026-09-23 用户定调）：**不用隐藏信息** —— 真机不搞 RNG 导出+重放搜索（「理论上是一种作弊」）；教师/搜索 = **「开天眼」口径**（模拟器内搜索看得到本局真实未来抽牌顺序），**裸网才是公平口径**；模型输入侧本就干净（`azlayout.PILE_TOP_K=0` 牌堆只给集合 + `aznet.card_feats` 置换不变池化 + `_chk_pool.py` 守卫；raw 快照的堆序只作调试、不喂模型）。评测工具 `_eval_walk.py`：同分布漫游、同 seed 配对、按幕/房间/怪群拆表、±CI、大 n。**n=1000 结果**（种子 20000..20999）：t4000 **90.3%** / t500 89.7 / e8+M100 89.3 / e32 裸 87.8 / e8 裸 87.6 / e2 裸 86.7（±0.6~0.8pp）。**差距全在规划型战斗**：GUARDIAN 33/5/17（师4000/e8/M100）、HEXAGHOST 41/16/22、SLIME_BOSS 40/22/34（M100 超 t500）、GREMLIN_GANG 99/75/95；act1 小怪双方全 100；act2 上层（BookOfStabbing/GremlinLeader/Slavers）教师也打不动（数据里没有解）。蒸馏训练量 e2→e8→e32 已饱和（+0.9→+0.2pp，val_ce 仍改善 ⇒ 模仿路线到顶）；搜索阶梯 87.6→89.3→(n200 的 M500)90.3 仍上行 ⇒ 闭环靶子 = 规划型仗的 value 头。

- 「模仿饱和」判据（2026-09-23 讨论）：现有证据只覆盖**训练量一轴**（e2/e8/e32 CI 内；不显著≠等价）。饱和 = 优化/容量/数据/目标质量**四轴**联合命题；容量探测 ≠ 只放大策略头（信息在主干）。免重训探针：① ckpt 在 held-out 上的 KL/top1；② 教师自洽性=噪声底；③ 同可见特征标签不一致率=**信息底**（教师开天眼；Guardian/Hexaghost 缺口像信息底而非容量底）。判序：目标探针（加 sims 重打标签）→ 容量探针（固定数据宽 256/512/1024×多种子，双指标 KL+胜率）→ 都不动=信息底，只有换目标（自对弈真 Z）才有增量。

- **模仿探针已落地（`_imit_probe.py`，2026-09-23）**：npz+ckpt 离线（无引擎）算 KL/top1/NLL/温度校准KL*，按 train|val（同 `battle_split(group=seed)`）/帧型/怪群拆表 + 均匀/边际基线 + 教师置信分层；新数据优先用 `enc` 列。**结果**（walkdata_v3，val act）：e2/e8/e32 → top1 58.6/61.8/63.4%、**NLL硬 0.985/0.911/0.882**（≈副档 val_ce ⇒ 训练用**硬目标**）、原始 KL 0.33/0.39/0.45、**校准 KL\* 0.232/0.223/0.216 @ T*=2.9/3.3/3.8**（网熵 1.02/0.94/0.88 vs 教师 0.96）。判读：**原始 KL 上升=锐化伪影**（硬目标不惩罚压尾巴；校准后单调下降 ⇒ 排序信息仍在变好）；**模仿指标未饱和但胜率饱和**；清晰态仅 38% top1 85-87%、模糊态 52% top1 39-45%；**逐位重复 obs=0 ⇒ 噪声底需构造配对**（待办）；打不过的 SLIME_BOSS/HEXAGHOST/GUARDIAN 模仿指标并不差 ⇒ 缺口在多步规划。**指标纪律：用 NLL硬/top1/校准KL\*，别用原始 KL/NLL软**。锐化只在「π 当先验用」时可能成问题（判决实验 e32+M100 vs e8+M100 未做；缓解=软目标/温度）。

- **软目标第四次复现「更差」（2026-09-23，walk v3）**：教师熵/logN≈0.66（≠旧场景 0.95，不是灌噪声 regime）；同数据/seed/8ep：探针侧软网**分布更忠实**（KL 0.186 < 硬 0.392，且低于硬的**最优温度校准** 0.223；T*=0.87≈1）、明确态 top1 更高（88.7 vs 85.4），但**模糊态 top1 43→34**、裸网 n=1000 **87.6%→86.2%**，决策重的仗崩（THREE_SENTRIES 69→46、GREMLIN_NOB 85→71、HEXAGHOST 16→6）⇒ **KL/NLL软 与胜率反相关**：教师 π 的"对冲式平分"是噪声，学它稀释决策边界。目标读数=NLL硬/top1；软目标结案（别第五次）。产物 `data/walk_v3_n800_soft8.pt`。

- **闭环抖动判据（2026-09-23）**：离线蒸馏无反馈回路（KL 变化只是读数；判定噪声底 = 8 线程跨进程不可复现 + n=1000 评估 ±0.7pp）。闭环要盯**相邻迭代策略位移**（`_imit_probe` 新增 cross 段）：e2→e8 TV 0.109/argmax 一致 78.3%、e8→e32 0.086/83.8%（**在收敛**）、e32→soft8 0.221/68.2%（**换口径位移最大**，比两段训练之和还大）；参照=离教师 TV 0.25-0.27。对策=固定口径 + 限制步长（同 seed n≥1000 对照才判"涨"）+ 覆盖注入（`STS_SELFPLAY_SAMPLE`/`EPS` 居中）+ 新旧数据混合 + checkpoint 选举；已知停滞模式=argmax 执行 + 固定 seeds ⇒ 同一轨迹重标注（闭环燃料是覆盖）。

- **遭遇表（`_enc_ids.py`）**：C++ 权威（正向）：`include/constants/MonsterIds.h`（id 数值=声明序，Python `sts.monster_name(id)` 解名；2=ACID_SLIME_M、52=SPIKE_SLIME_M、3/53=酸/刺小史莱姆、23/37=绿/红虱）+ `src/combat/MonsterGroup.cpp::createMonsters`（组合与**变体=随机分支**）+ `MonsterEncounters.h`/`GameContext.cpp`（各幕池子）。**2026-09-23 起新采集数据带 `enc` 列**（`_walk_battle` 返回五元组、直接读 `gc.encounter`；`_assemble_walk` 展开逐帧）——派生信息在源头记录，不再事后反推；旧缓存回退 `_imit_probe.ENC_IDS`（该表仍由 `_enc_ids.py` 枚举生成，支持多 seed 扫变体）。完整变体集应读 C++/加绑定，不要穷举 seed。

- **Git 远端账目（2026-09-23 收口）**：`origin`=FrozenStream/sts_lightspeed（**正牌 fork**，parent=gamerpuppy、7/26 创建、从未 push ⇒ 内容=上游快照，fork 上**仅 `master`=7476a819**）；`upstream`=gamerpuppy（master 同 7476a819；另有 develop/mcts/reorganize）。事故：7/26 一次 `fetch origin --tags` 曾把 pybind11 的 37 个分支 refs 灌进本地 `origin/*`（origin URL 一度指 pybind11）⇒ 假角标「behind 3289」；已修：清陈旧 refs + `origin/master` 重建 = 7476a819 + `master` 分支 `--unset-upstream`（恢复跟踪 = `git branch --set-upstream-to=origin/master`）；经代理 `fetch --prune origin` 复核通过，现 `master...origin/master = 3/0`（本地 3 提交独有）。**本机连 GitHub 必须走代理 `http://127.0.0.1:7890`**（`export http_proxy/https_proxy`；直连 443 不通）。

- 坑①b：`_sync.sh check` 的指纹**以本地清单遍历远端**（`_fp_remote` 只 md5 清单内文件）⇒ **远端多出来的文件永远看不见**；查真差异须 `diff <(ls *.py *.sh | sort) <(ssh ... "ls *.py *.sh | sort")`。实测远端残留 22 个已被本地归档/删除的旧脚本。

- 坑②（**09-24 勘误**）：本地 `build/Debug/slaythespire.*.pyd` 常陈旧（不重建就带不动新快照布局，本地守卫会挂）。**但本地是能编的**：bash 里确实没有 `cmake`/`MSBuild`，可是 **VS Code 的 CMake Tools 扩展能编**（`build/` 已有 MSVC cache；目标 `slaythespire` → 直接产 `.pyd`，约 1 分钟，只有既有的 clang-cl `-Wunused-function` 噪声）。**改完 C++ 先本地重编再跑本地守卫**，别把它当"只能远端编"。刷新 pyd 会连带把 `_FALLBACK` 类过期副本的问题**暴露出来**（见 `realtest/comm_map.py` 待办）。

- 坑③：给含特殊字符的大文件做多行替换容易失配 —— 拆小步编辑；同一文件不要在同一批里并发替换。

- 坑④：`bash -s` heredoc 里调用 `script` 会**吞掉剩余 heredoc 当 stdin**（后续行变成 pty 回显、不执行）——所有远程一次性 `script` 调用要加 `</dev/null`。

- **撞壁概率实测（09-25）**：300 局 @UCT(4000) / 4038 场 / 59823 决策点 ⇒ 撞 `MAX_STEPS` **0 场**、撞 `MAX_TURNS` **0 场**（极值 70 动作 / 17 回合；act1 2704 / act2 1185 / act3 149）。95% 上界 ≈ 0.074%/场。

- **本地采集吞吐（09-25 干净基准）**：5.65 s/局（16 workers）⇒ 万局 ≈ 15.7 h（单局串行 18.3 s ⇒ 并行仅 ~3.2×）。万局仍应放远端。

- **金币口径改「简单 0..400 随机数」（09-27，用户指令）**：`_collect_walk.shape_floor` 的 `gc.gold = st.randint(0, 99) + st.randint(0, GOLD_PER_FLOOR * gc.floor_num)` → `gc.gold = st.randint(0, 400)`；`GOLD_PER_FLOOR = 14` 删；fp 串 `gold=0..99+14/floor` → `gold=0..400`。**新 `walk_fp = 0d0a3eb79c7f4523144eec08924d144d407e5c94`**（`WALK_VER` 保持 `v6`：v6 从未采过数据 ⇒ 无缓存冲突）。`.agent/notes.md` 里几处 `0..99+14/floor` 是 v3 历史记录，**不动**。

- **上限的账（09-27 澄清）**：`_az_train.MAX_STEPS`（**动作数**，现值 1000，早就在）/ `MAX_TURNS`（**回合数**，现值 50，09-25 新加）—— 这两个被 `_collect_walk`/`_collect_dagger`/`_eval_walk` 共用；`_walk.py` 独有 `MAX_LOOP=30000`/`MAX_BATTLE_STEPS=20000`。**MCTS 侧没有上限**，只有 `sims`（预算）与 `MCTS_BATCH`（叶子批量）。

- **战斗收场规则（09-25 定调）**：三条同源战斗线（`_collect_walk._walk_battle` / `_collect_dagger._dagger_battle` / `_eval_walk._eval_battle`）已删「连续 5 步能量没变 ⇒ 强制结束回合」（判据太宽：0 费牌/药水都不改能量；实测触发 4/894 动作），改成 `_az_train.MAX_TURNS`（现值 50，`MAX_STEPS` 现值 1000；超了就收场，`z` 记未获胜）。`_walk.py` 未动（独立冒烟漫游，刻意不在导入期拉 torch）。

- **重点训练/构造采集（09-24 评估，notes 续四十六）**：①“给弱怪加权/多采”**非瓶颈** —— 普查：问题仗是帧大户（HEXAGHOST 5.12%/3245 场、GUARDIAN 4.65%、问题仗合计 ≈21.6% 帧；三兄弟各 ~1k 场）⇒ 缺口 = 规划/复合误差 + 三兄弟教师标签上限（6–20%，先跑预算阶梯）。②**按怪池构造独立战斗可行且机制现成**（`_gen_eval.make_battle` 先例；`gc.act`/`gc.floor_num` 可写）⇒ 计划 `_collect_battle.py` + 配额（act3 解锁）+ “构造 vs walk” 对照；③ 自对弈采集器建其上。⚠️ 唯一不匹配：RUN_CTX 直方图 7 维（构造局无地图）→ 按 walk (act,floor) 注入或接受常量。walk 保留（eval 基建+整局线）。

- **统一网判读（notes 续四十八 + 五十一）**：逻辑层各头定义域错开、无目标冲突（战斗头 = E[G_战后|s] ≈ 延伸进战斗时刻的 V_run）；冲突全在优化/容量：①两套输入管线（v0a=按域分派+兼容外壳、不合并嵌入表）②H=128 容量负迁移（等参对照；先加宽→分塔）③z vs G 尺度+显式配比（分域 batch）④构造假上下文（域嵌入 / run 头不读注入位）⑤非平稳漂移经共享主干放大（版本钉死+冻结交替）。判序：v0a 零列等价闸 → 等参消融 → 干扰直读。**自对弈 v1 不依赖统一网**（耦合在数据层）。**（续五十一）战斗头←全局头只在终局叶子单向 detach + 搜索回传扩散 ⇒ 不打架；必打架三形态 = 逐帧拟合 / 量纲不迁（z↔G 全链迁；旧 z 值数据退役或 z 辅助头）/ 对称互拟；待防 = 冻结靶 + 容量消融。** **（续五十二）训练稳定性：分网+离线靶 > 统一网+快照/交替 > 统一网裸跑同时训**（干带传导 / 冻结头≠静止函数→需快照 / 配平与归因纠缠；分网稳定性=冻结换的，非免费）；残余 ≈ 干带耦合（容量问题）；判定 = v0a 闸 + 消融（kill：退化>1pp 且 512 不救 ⇒ 回分网）。

- **守望纪律（09-24 用户两次指出「你又没有保持监听」）**：远端任何时刻都必须有 listener —— 撤旧守望前先落实继任者（两层：远端 `_chain.sh` 会话 + 本地 `wait` 静默监听）；GPU 支线撤销时撤了 `cap` 却忘重挂，等于无人监听。已重挂：`STS_SESSION=cap`（12:56 就位，等 `job-chain-_eval_walk.log` ⇒ `_probe_capacity.py`）+ 本地 `wait`。

- **DAgger v2 判决（09-24 续五十六）**：六臂 n=1000 —— 教师4000 93.0 / 教师500 92.3 / e8 裸 89.8·M100 92.4 / **dagger2 裸 89.9·M100 92.3** ⇒ 相对 e8 **+0.1 / −0.1pp**（CI ±0.6）⇒ **分布轴一阶、不可累积，结案**（dagger1 是 +0.6/0.0）。缺口仍全在规划型仗（GREMLIN_LEADER 42%、COLLECTOR 4%、CHAMP 6%、AUTOMATON 3% ⇒ 与 e8 同形）。优化/数据/分布三轴全关 ⇒ 下一步只剩**目标质量轴（自对弈 v1 真 Z）**。容量探针 13:15:27 已自动接力开跑（6 路并发 = 12 核）。

- **整局通关率实测 + 口径陷阱（09-24 续五十八）**：walk v4 万局**通关 2/10000 = 0.02%**（逐局取 `bid` 最大的那场判胜负）；死亡点 = Act1 Boss **33.3%** / Act2 Boss 26.8% / Act2 精英 19.9%。原因不是搜索（我们的教师与上游 `BattleScumSearcher2` 逐位同源、同一份 7476a81），而是**资源不发育** —— walk 每场开打前 `reset_run_resources()` + 按 p=floor/48 **随机发卡/随机升级/随机遗物/随机血量**，且不进奖励/商店/营火屏 ⇒ 「把玩家全部构筑决策随机化」的整局；外部项目 `Jialeiv/sts-rl-agent` 报的 2%@2000 / 6%@50000 是**正常发育、只把地图随机**的整局。⚠️ 列名陷阱：`t_win/t_hp/t_max_hp/t_turn/t_enemy` 是**战斗级**终局列（`t_enemy` = 敌人剩余血量比，遭遇名在 `enc` 列）——别拿 `t_win` 当整局胜负。**可比口径的现成工具 = `_collect_run.py`**（地图随机 + `runpolicy` 启发式处理奖励/商店/营火 + 外挂教师 `sims` + 打印「终局: 通关 N/M 平均楼层」），目前只跑过 smoke（5 局@50）；待办 = 并行化后跑 200 局 @2000 并换 `net:` 臂。

- **⚠️ 用户 09-24 连续砍掉三块「不该进战斗观测」的东西（runCtx 瘦身的理由，别再往回加）**：① **地图**（曾 22 维，`computeMapFeat` + `BattleContext::mapFeat`，与 `runlayout.map_features()` 同源，×10000 定点）—— 「处于什么节点的战斗决策目标不会有大的差异」，而且**构造场景根本没有 `curMapNode`** ⇒ 只会让数据合成变难。**已整体删除**（含 `BattleContext` 字段与 C++ 计算函数）；run 网的 `map_ahead` 是**地图决策**、不受影响。② **isElite / isBoss** —— 「和 id+动作的嵌入完全等价」（知道 `monster_id` 就知道是不是 Boss/精英）⇒ 冗余。**已删**（含 `Snapshot` 写入、`isEliteRoom/isBossRoom`、以及 `MonsterEncounterPool::elites` 派生的 `isEliteEncounter`）。※ 其间一度改成"取自房间 `gc.curRoom`"，用户随后判定连这个都不必要 —— 现在 `BattleContext::roomKind` 也**已删**，`BattleContext.h` 不再 include `constants/Rooms.h`。引擎内部 `ScumSearchAgent2` 仍按 `isBossEncounter` 给 3× **搜索预算**（那是预算不是观测）。③ **act** —— 「层数/16 就是 act」：**完全由 floor 决定**（一层一节点、floor 单调递增；实测 act1 = floor 1..16）⇒ `BattleContext::actNum` **已删**。⚠️ 用户会**撤销**他不认的编辑（本轮撤销过 `MonsterEncounters.h`）—— 改前先看当前文件内容。

- **`_chk_all.py` 是 engine 档的入口**（**13 项**，秒级；含 `_chk_revive.py`）—— 改布局/引擎后必跑。**09-24 实况：12/13**，唯一红项 = `真机桥接`（见下条 `realtest/comm_map.py`，与本次改动无关，是被刷新后的 pyd "照出来"的旧账）。

- **注释纪律补充（用户 09-24 定调）：不要"只标一个数字"** —— 形如 `MONSTER_FIELDS = ... # 12` 或 `playerOff = 0; // 8` 的注释一律改成**写全内容**（每个字段是什么）。已清：`bindings/slaythespire.cpp` 快照偏移块（抽出 `playerLen`/`countsLen` 常量 + 每块上方列全字段）、`azlayout.py`（monster 块逐下标清单、`INPUT_STATE_CARD_SELECT` 写出枚举三态、run_ctx 拆成 `RUN_CTX_{FLOOR,GOLD}_IDX`）。

- **整局发育分布实测（09-24 续五十九）**：新驱动 `_collect_dev.py`（口径 = 上游 bot：随机地图 + `runpolicy` 非战斗启发式 + `STS_DEV_SKIP=1` 时引擎 `gc.skip_battles` 跳过战斗当胜利；A0 铁甲；行 = 战斗外决策点，含 `deck/deck_rarity/relic` 定长 64 列）。**1000 局 / 111257 行 / 229s，本地跑**。逐层实测（floor → 牌组/基白/蓝/金/咒/升级/遗物）：0→10.2/9.9/0.2/0.1/0/0.1/1.0；16→17.4/13.1/3.4/0.8/0.1/2.8/1.3；33→24.9/15.1/6.2/2.2/0.3/7.1/2.4；48→31.6/17.1/9.1/3.5/0.6/12.5/3.6。**对照 `_collect_walk` 的 `p=floor/48` 发牌（⚠️ 初版误判已勘误）**：`reset_run_resources` 末尾会 `initPlayer()` **重建初始牌组 10 张 + 初始遗物 1 件**（初版只读了前半段的"清空" ⇒ 一度误判"牌组只有一半"）。实际 = 牌组 17.9/22.0/26.1 vs 实测 17.4/24.9/31.6（**基本对齐**）；真偏差两条：① 升级偏少（act2/3 差 40–50%，v4 floor48=6 vs 实测 12.5）② **遗物偏多 2.5–3×**（4.0/7.0/9.4 vs 1.3/2.4/3.6）。增量 ≈ +0.45 张牌/层、+0.26 升级/层、+0.05 遗物/层。⚠️ 已知偏差：商店**不购买**（绑定无 shop API）⇒ 遗物低估；`skip_battles` 不掉血不输 ⇒ hp% 恒 0.97–1.00；**药水列恒 0**（`runpolicy.step_rewards` 只领卡牌，药水在原生里是独立 `RewardsActionType::POTION` 动作）。**`devshape.py` 6 段定稿（全表，无逐层）**（用户定调：拿掉逐层表）：牌组追加白/蓝/金/咒 = `(1.7,1.2,0.3,0.0)` → `(3.7,2.5,0.4,0.1)` → `(5.5,4.0,1.5,0.1)` → `(7.0,5.1,1.6,0.3)` → `(7.7,6.9,2.8,0.5)` → `(8.8,8.4,3.0,1.0)`、遗物 `1→2.8→4.6→6.4→8.2→10`、升级 `0→2→4→6→8→10 ±2`（`UPGRADE_JITTER=2`）；模块 102 行（原 ~190）；**已删掉 v4/v5 双路径与 `STS_DECK_SHAPE` 开关**（用户："你写代码也不嫌恶心"）——只留一条 6 段路径：`build_shaped_deck(gc, sub)` / `grant_shaped_resources(gc, sub)` 不再收 `p`，`_floor_progress` 一并删除（3 个调用点 `_collect_walk`/`_collect_dagger`/`_eval_walk` 同步改）。**`walk_fp()` 已升到 `walk-v5`** ⇒ v4 及以前缓存作废、需重采（~1.8h）；`_imit_probe.py` 不校 fp（只 `check_cache_obs`）⇒ 容量探针的探针段不受影响、`_eval_walk.py` 本就不读缓存 ✓。⚠️ 遗物旧结论（"v4 偏高 2.5–3×、削到 mid"）**已撤回**：真实 act3下 遗物 9.5 ≈ v4 原值（旧结论来自"不买商店"的低估数据）。那个项目的部署（远端 `/root/autodl-tmp/ext/`：clone + 干净上游 7476a819 + 两个 patch + build312 + `dev_record.py`/`report_native.py`）**已按用户要求撤现场**，只留 `/root/autodl-tmp/native_dev_n1000.jsonl`（1000 局逐层记录）作数据留档；本地不留任何那个项目的文件。

- **`FactorPolicyHead` 形参错位 + 改名 + 删死开关（09-28）**：形参顺序曾与唯一调用点（`AZNet.__init__`，位置实参）不一致 ⇒ `flat_head`（旧名 `flat_rest`）宽度 `8−8×10−5×7 = −107`，`arch="factor"` **构造即崩**。现按「表示宽度 → 输出形状 → 开关」重排，并删掉两个**已死**条件（`hand_num` / `n_potion` —— `AZNet` 总是给）；改名：`k_hand_num`→`k_hand_extra_scalars`、`k_num`→`k_mon_extra_scalars`（策略头）/`k_cand_extra_scalars`（选牌头）、`flat_rest`→`flat_head`、头内 `hand_num`→`hand_extra_scalars`、`num`→`mon_extra_scalars`、`*_tok`→`*_emb`（`hand_tok`/`mon_id_tok`/`pot_tok`/`cand_tok`）、`none_tok`/`discard_tok`→`none_target`/`discard_target`（这两个是 `nn.Parameter` 虚拟目标行、不是查表）、单字母入参 `q`→`state`、`q4`→`query`（`self.q` 保留：q/k 是查询/键术语，且 `_diag_head` 靠 `select_head.q.weight` 判架构）（语义 = **除 id 外的额外标量列**；用户两次否掉 `hand_num` 与 `hand_slot_num`，别再改回「槽/数值」这类词）；`FactorSelectHead` 不再收 `n_num`（读 `SEL_EXTRA_N`）；两个头统一成「`cat`(嵌入, 额外标量列) → 一个 Linear」：`k_card`+`k_hand_extra_scalars`→`k_hand`、`k_cand_extra_scalars` 并入 `k_cand`、`k_mon_extra_scalars` 也删掉（`k_mon` 直接吃 `cat([mon_id_emb, mon_extra_scalars])`，`mon_dim` 随之 = `monster_rank + MONSTER_FIELDS` = 20，虚拟行/配对项跟着变宽、参数 +416）—— 合并是**超集**（旧写法把数值半边的映射秩锁在 ≤ rank），三个 key 现在同形：`cat`(嵌入, 额外标量列) → 一次投影。⚠️ **`state_dict` 键名变了**（`data/` 里只有 run 家族权重，factor 无落盘）。同日继续（用户第二轮："改成卡牌额外标量数据的语义"）：`azlayout` 的 `HAND_NUM_OFF/N/IDX`→`HAND_EXTRA_OFF/N/IDX`、`SEL_NUM_IDX`→`SEL_EXTRA_IDX`（新增 `SEL_EXTRA_OFF/N`；两个 `*_EXTRA_OFF` 改成从 `HAND_FIELDS`/`CARD_SELECT_FIELDS` **派生**，不再是手写 `(1,2,3)`），`aznet` 缓冲 `_idx_hand_num`/`_idx_sel_num`→`_idx_hand_extra`/`_idx_sel_extra`（缓冲名 = 常量名加 `_idx_` 照旧）。验证：**纯改名**只跑 `py_compile` + 三配置构造/前向 + `_chk_res.py`（36/36）就够；只有改**计算/形参面**才需要「旧副本逐位对照」（同 seed 各建一份，比 `state_dict()` 键集与 `torch.equal`）。⚠️ 我留在 `$TEMP/sts_ref/` 的旧副本与对照脚本**已被用户删除**，别再引用。同族坑：块宽漂移是**静默**的，只有取值域越界才吵。

- **配对项两层合一层（09-28 续；`超集`，参数量 −16512）**：`n_mon`(tar_dim→hidden) + `n_mon_mix`(hidden→hidden) 中间**没有非线性** ⇒ 精确等于一个仿射（`(W₂W₁)t + (W₂b₁+b₂)`；实测 `allclose`、整头 logits 差 1.2e-7 = float32 求和序）。现只留单层 `n_mon`，`forward` 写 `槽 key ⊙ self.n_mon(tgt)`（⚠️ 投影必须在 `⊙` 之前，否则有效矩阵恒对角）。`W₂W₁` 秩 ≤ 20（实测 20）⇒ 旧写法是「全部 `hidden×20` 矩阵」的零测子集，合并**严格超集**；两层 bias 全 0 + 乘积方差 `= 1/hidden` ⇒ 初始尺度不变（0.1237 vs 0.125 @ h=64）。⚠️「`M`(hidden×hidden) 作用在已算好的 `k_target(tgt)` 上」等价但贵 6×（`W_t` 列满秩 ⇒ `{M·W_t}` = 全部 `hidden×tar_dim` 矩阵）。参数量 (h=128,d=2)：flat 225077 / factor 234879 / **factor+bi 237567**（旧 254079）/ factor+bi+selfact 248004。`state_dict` 键再变（`n_mon_mix.*` 消失）⇒ `_diag_head` 改认 `policy_head.n_mon.weight` + 旧两层权重**显式报错**（附折叠公式）；磁盘上**无**旧 bilinear 权重。验证：`py_compile`、四配置前向(96/60/1)+bilinear 全参有梯度、`_chk_res` 36/36、`_chk_all` engine **13/14**（唯一红项 = 已知 `realtest/comm_map.py` 的 `KeyError: 'potion_bag_off'`，与本次无关）。⚠️ **勘误（同名旧词）**：本文件上面那条 09-28 记录里的 `k_mon` 现名 **`k_target`**、局部 `mon_dim` 现名 **`tar_dim`**。

- **`FactorPolicyHead` 签名瘦身 + 输出宽改「真实产出」断言（09-28 续二）**：删 5 个**已死**参数（`policy_dim`/`n_hand_slots`/`n_hand_targets`/`n_potion_slots`/`n_potion_targets` —— 旧 `flat_head`（现 `end_head`、宽度写死 1）用它们算宽度；实测用乱写的值建头，`state_dict` 形状与前向**逐位相同**）⇒ 现签名 = `(hidden, card_rank, monster_rank, potion_rank, bilinear)`。宽度契约改成 `AZNet.__init__` 末端的端到端断言（构造时跑一次 `self(zeros(1,OBS_DIM))`，断言 `p.shape[-1]==POLICY_DIM`、`s.shape[-1]==SELECT_DIM`），⚠️ 跑前临时 `eval()`（实测 train 前向吃 RNG / eval 不吃），⚠️ 该断言在 `arch="flat"` 下**恒真**（宽度就是它定义的）⇒ 只保护 factor 臂。同波同步了 `FactorPolicyHead` docstring 与 `azlayout.POTION_TARGET_N` 的注释。验证：四配置前向(96/60/1)+参数量 flat 225077 / factor 237567 / factor+bi 237567 / factor+bi+selfact 248004、`_chk_res` 36/36。✅ **已定（09-28 续十一）**：**保持无条件构造** —— 权重永远存在，开关只决定「要不要用上」（`pair="none"` 时它白占参数、梯度恒 None，这是接受的代价；见 `.agent/CODE_STYLE.md`「开关与权重」）。

- **`FactorSelectHead` 同风格重排（09-28 续三）**：与 `FactorPolicyHead` 对齐 —— 无 docstring、`# ── key ──`/`# ── score ──` 分段、`forward` 里 `query` 前置、`key`→`key_cand`、行尾/形状注释对齐。⚠️ `__init__` 里 `k_cand` 建在 `q` **之前** ⇒ **同 seed 初始化值又变了**（`nn.Linear` 构造期就抽随机数）；键名/形状/参数量不变（select 头仍 `q`/`k_cand`/`out`）。掩码契约从 docstring 挪到 `forward` 行内注释（`AZNet.select_keys` 里本来也有一份）。验证：`py_compile`、四配置前向（96/60）、`_chk_res` 36/36。⚠️ 精确改这类 CJK 注释行前，用 `line.replace(' ', '·')` 打印来分辨「终端假空白」与真空格（本轮就是靠它把 `(卡牌嵌入 ⊕ …)` 匹配准的）。

- **`AZNet.cfg` 已删（09-28 续十五）**：用户定调「搞这些花哨的参数保存没什么意义，需要保存的时候参数就在脚本文件里面写明了」⇒ 删掉 `aznet.AZNet.__init__` 里那条 `locals()`∩signature 抄法（`save_ckpt` 是它**唯一**读者）⇒ `_az_train.save_ckpt` 改 `cfg = dict(REG)`。副档 `reg` 因此变 **6 键**（`arch/depth/dropout/hidden/pair/sel_arch`）—— **五个嵌入宽度不再进副档**，非默认宽度存盘会在 `load_state_dict` **响亮**失败（否则它只会静默错配）。验证：真跑 `train <缓存> 1 0 <out>` + 读副档 + 引擎档 14/15。⚠️ **副档本身**已于同日**删除**（续十六：改单文件自描述存档，9 个下游 + `net_kwargs` 全改走 `_ckpt`）。

- **权重存档 = 单文件自描述（09-28 续十六，**取代**上面那条断链）**：`_ckpt.py` 是**唯一读写入口** —— `FMT=1` 的 `torch.save({fmt, weights, config, ...})` 一个文件；`read()`（归一化）/ `config_of()`（取不到 = None，**不猜**）/ `weights_of()` / `migrate()`。战斗网元数据 = `space`/`z_spec`/`metrics`，整局网 = `train` + `run_dim`/`obs_schema`；`config` **一律 = 建网配置**（`AZNet(**net_kwargs(cfg))` / `RunNet(**cfg)` 直接可用 ⇒ `runnet.run_kwargs` 因零调用者删除）。旧两文件存档**仍能读**（`fmt=0`，打一行 ⚠️），`migrate()` 重存后权重**逐位不变**并删副档。复核一条：`python _ckpt.py <档>` 打印 `[单文件]` / `[⚠️ 旧的两文件]` + 配置 + 键数。全局 `REG`→**`CONFIG`**（`_az_train`）；写侧落 `config=` 后 8 个读点曾仍读 `["reg"]`（6 处硬 KeyError + 2 处静默退回默认）—— 已全修，注释残留也清了。**教训：一个契约靠字符串字面量复制在 9 个文件里 ⇒ 改名对类型检查器全程静默。** 验证：18 文件 `py_compile`、真跑 `train`+`_diag_head`、`_chk_all.py` **14/15**、`_chk_runtrain.py` **29/0**。（历史：一度出现过写侧落 `config=` 而 8 个读点仍读 `["reg"]` 的断链 —— 随本次改造一并修掉。）

- **训练打点 = TensorBoard（09-28 续十七）**：`_tb.py` 是**唯一打点入口**（`writer/scalars/text/recipe/fault/close`），目录 `data/runs/<权重名或数据名>/seed<k>`；看：`tensorboard --logdir data/runs`。**故障告警双写**：TB（`fault/<标签>` 置 1 + text 详情）+ `data/runs/faults.jsonl`（可 grep，**不依赖 tensorboard 装没装**），四条发散闸（步级 loss / epoch 末参数 / 存盘拒收 / 确定性自检）全接上。旋钮 `--tb`/`--tb-dir`（整局训练器是 env 系 ⇒ `STS_RUN_TB=0`）。⭐ 打点是**纯旁路**：`_chk_threads.py` 权重 sha 加打点前后**逐位相同**（`bc0fddf8188bb688`/`005d84706dfc32e4`）⇒ 以后再加打点也用这条 sha 复核。`tensorboard` 是可选依赖（`uv add tensorboard` 进了 `pyproject.toml`）⇒ 远端要单独装。复核：`ls data/runs`。

- **按怪物遭遇组采集（09-28 续十八）**：**`_collect_encs.py`**（独立脚本，与 `_collect_walk`/`_collect_dagger` 同风格）—— `python _collect_encs.py <每组场数> <sims> [out.npz] [--encs all|A,B,...]`；缓存 `encdata_n<n>_s<sims>.npz` 多两列 `enc`（每场遭遇 id）/`encs`（请求名单）。**为什么需要**：`_az_train.collect` 的遭遇由 `gc.boss` 定 ⇒ 多怪行只占全仓 **0.08%**（`_chk_enc.py` 在 `_ab28.npz` 读数）⇒ 要「分裂的多怪/精英」必须指名打。⭐ **实测 63 组全部可采**（1 场/组@30sims，16 workers，26s，教师总胜率 54.0%；Act1 弱怪 100% vs Act2-3 精英/Boss 大量 0%）。⚠️ **每组一个子进程**：个别遭遇原生 abort（`INVALID` 退出码 **3** = MSVC `abort()`、**零输出**），`try/except` 与 Pool 都接不住。指纹 `scenario_fp(encs=...)` 与 Boss 缓存必然不同，`encs=None` 路径逐位不变（`f0ea451dcc15382e`）。续跑 = 分片 `<缓存 stem>_parts/<ENC>.npz` 复用。守卫 `_chk_encs.py`（engine 档 **15/16**）。复核：`python _collect_encs.py 1 30 /tmp/x.npz --encs CULTIST`。

- **`AZNet` buffer 命名统一（09-28 续五，纯改名、零兼容性影响）**：规则 = **`_idx_` + 常量名小写去掉尾部 `_IDX`**；例外 = `scale`（非索引表 + **唯一 persistent**，在 state_dict 里 ⇒ 改名会作废战斗网 ckpt，不动）、`_sel_region_mask`（**布尔掩码**，不属 `_idx_` 家族）。改了 5 个：`_idx_card`→`_idx_card_id`、`_idx_mon`→`_idx_monster_id`、`_idx_mon_row`→`_idx_mon_id_row`、`_in_sel_region`→`_sel_region_mask`、`_pile_n`→`_pile_lens`。⚠️ **`_idx_mon` 是 `_idx_mon_row` 的前缀** ⇒ 不能同批替换（会变成 `_idx_monster_id_row`）；用带上下文的锚点（`_idx_mon: torch.Tensor` / `("_idx_mon", …)` / `self._idx_mon).long()`）消歧义即可一次批量完成。验证：旧名残留扫描全 0、`state_dict` 仍 14 键且含 `scale`、无任何 `_idx_`、四配置前向 96/60/1、参数量不变、`_chk_res` 36/36。⚠️ 上述第 96/97 行那句「缓冲名 = 常量名加 `_idx_`」**不准确**，以本条规则为准。⚠️ 遗留（**已于 09-28 续十 解决**）：常量本身曾不齐（`MONSTER_ID_IDX` vs `MON_ID_ROW_IDX`）—— 后者已删，改由 `azlayout` 给 `MONSTER_EXTRA_IDX`（缓冲名 `_idx_monster_extra`）。

- **怪物目标行「像手牌一样」（09-28 续十）**：`azlayout` 新增 `MONSTER_EXTRA_OFF`（排除 `(0,4)`，10 项）/ `MONSTER_EXTRA_N` / `MONSTER_EXTRA_IDX`（5×10），与 `HAND_EXTRA_*` 同形 + 一条按 `CAT_IDX` 的断言对账；`aznet.MON_ID_ROW_IDX` 删除。理由：旧写法把 `[0] monsterId`/`[4] intentMoveId`（都在 `CAT_IDX`、`SCALE` 盖成 1）**又当标量喂了** ⇒ `k_target` 收到 0..65 / 0..196 的原始 id，旁边是 `/80` 级别的归一化值，量级差 10~200× 且无语义。目标向量改为 **两个 id 嵌入在前 + 10 个数值标量在后**（与手牌 key 同形）：`tar_dim` 20→**26**，`FactorPolicyHead` 多收 `intent_move_rank`（**招式 id 进目标 key 是新信息**）。buffer `_idx_mon_id_row`→`_idx_monster_extra`。代价只在 factor 头（h=64 参数量 factor 116287→**117067**；trunk/`in_dim` 1382/flat 臂/缓存全不受影响）。验证：四配置前向 96/60/1、`policy_keys` 形状 `(10,8)/(10,3)/(5,8)/(5,8)/(5,10)/(5,8)`、旧名残留 0、`_chk_res` 36/36。⚠️ 顺手发现**未修**：`_diag_head.py` 回退路径的 `pair` 靠 `n_mon` 键名推断 —— 名字已过期且 `n_tar` 无条件构造 ⇒ `pair` 推不出来（bilinear 权重会被静默当 `none` 加载）；磁盘上无 factor 权重 ⇒ 影响 0。**该坑已修（09-28 续十一）**。

- **「开关不推、只决定用不用」定调（09-28 续十一，已写进 `.agent/CODE_STYLE.md`「开关与权重」节）**：用户一句话 —— **权重永远存在，开关只决定「要不要用上」**。⇒ ① 不要为省参数把某层构造写进 `if <开关>`（权重会随开关存在/消失）；② **开关不能从 `state_dict` 反推**（两种取值下键集合与形状全同，`strict=True` 拦不住），重建网络必须有配置记录（副档）。落地：`_diag_head.build_for` 回退路径删掉 `pair=\"bilinear\" if \"policy_head.n_mon.weight\" in sd ...`（名字过期 + 逻辑不成立），改成 `arch == \"factor\"` 且无可用副档 ⇒ **直接 SystemExit**（`n_mon_mix`/`m_card` 专属报错仍优先命中），`_来源` → `\"键名推断（flat 臂）\"`。验证四路径：flat 无副档→推断；factor 无副档→拒绝；旧两层→专属报错；有副档→走 `reg`（bilinear 正确）。※ `arch`/`sel_arch` 的键名推断**仍合法**（那是换模块 = 结构性差异，不是开关）。

- **`AZNet` 注释同风格收敛（09-28 续六，纯注释、零行为改动）**：类 docstring 46→14 行；buffer 块 / 自检块 / `cfg` 块 / CAT_SPEC 块各压到 2–4 行；删掉全部「实测数字」与历史叙事（`86 µs`/`419 µs`、`train_ce 0.685→0.022`、`（以前是共享的 embed_dim）`、`（开关已删）`、`铎错`）。保留的全是**契约/不变量**：五种 id 宽度决定 `in_dim`、嵌入表行 0 = 空槽（禁 `padding_idx=0`）、牌堆只给集合、buffer 为何 `persistent=False`、`scale` 是布局探针、自检为何临时 `eval()`。验证：`py_compile` + 四配置前向 96/60/1（h=64 参数量 114165/116287/116287/117380）+ 已删短语 grep 残留 0。※ 未动文件头模块 docstring（仍带「表面与拆分前一致」这类历史句）。

- **值目标 `enginenorm` = 引擎整式（09-28 续八，取代续七）**：口径 = `t_escore / (100 * t_max_hp)`，`t_escore` 由**新绑定** `sts.evaluate_end_state(b)` 取（= `BattleScumSearcher2::evaluateEndState` 的返回值）。绑 C++ 而不在 Python 拼的理由：败局支要 `energyWasted`/`cardsDrawn`（**未进快照**）、非小怪血比、THREE/FOUR_SHAPES 特判 —— 那两项本身是 `BattleContext.h:57,58` 的现成字段（零统计成本，只是没导出）。⚠️ **35 基线不能去**：实测 40 场 @500 sims（赢 15/输 25）引擎分赢 3590~9293、输 4.4~11.2；去 35 后赢局下限 ~0.01 会与败局**重叠**；保留 ⇒ 赢 **0.479~1.239**、输 **0.0006~0.0015**、落差 **0.477**（`old` 是 1.0+）⇒ **先扫 `c_puct`（按 ~0.48 缩）**。同时**删掉** `engine`/`lossshape`/`hpw`（含 `HP_W`）⇒ 现存口径 = `enginenorm`/`old`/`hp_turn`；终局列 `t_pot` → **`t_escore`**（旧缓存跑 `_z_revalue.py rew` 补列，零搜索；`old`/`hp_turn` 不读它）。连带：绑定重编 pyd、`cmd_rew` 读 `Z_COLS`、副档顶层 `z_spec`、`_flags/MIGRATION.md/battle-net-summary.md` 同步。验证：手算 + 本地真跑 `collect 40 500`（两分支实测范围/序）+ `train(1 ep)` 通过 + `_chk_all` engine **13/14**（唯一红项 = 已知真机桥接）。历史 val_ce / 胜率**只与 `--z-spec old` 同口径**。

- **战斗网的秩/初始化尺度收回 `aznet` 自己持有（09-28 续九）**：`aznet.RELIC_RANK=16` / `POTION_RANK=8` / `INIT_STD=0.02`（模块级三连赋值，紧跟 `# ---- 网络` 段头），`AZNet` 签名默认与三处 `nn.init.normal_` 都读它；**`import multi_hot as MH` 已删**（`aznet.MH` 属性随之消失，全仓无人依赖）。`multi_hot.py` 现在明确是**整局网 `runnet` 专用**（其三个常量 = 整局网默认值，文件头也改了）。可以各持一份的理由：两网 09-25 分家后权重互不相干，`_res_prior.py` 搬表时秩不等形状当场对不上（响亮）。连带：`_chk_res.py` 断言 `relic_linear` 形状那条改用 `aznet.RELIC_RANK`（runnet 的 in_dim 公式仍用 `MH.RELIC_RANK`）。验证：`MH` 残留 0、数值逐位同（⇒ `in_dim` 数学上不可能变，实测仍 1382）、`relic_linear=(16,181)`、四配置参数量不变、`_chk_res` **36/36**。

- **五个嵌入宽度收成 `aznet` 常量、且不进 `REG`（09-28 续十二）**：用户问「net 的超参不进 REG 吗？」，中途试过「REG 引用常量」，随即定调「那这些也不用留了」⇒ **终案：宽度不进 `REG`**。`aznet` 顶部 = `CARD_RANK/MONSTER_RANK/INTENT_MOVE_RANK = 8,8,8` + `RELIC_RANK/POTION_RANK/INIT_STD = 16,8,0.02`（**唯一来源**，`AZNet` 签名默认值读它）；`_az_train.REG` 只剩训练侧选择 6 键（`hidden/dropout/depth/arch/sel_arch/pair`）。⚠️ **副档仍完整**：`net.cfg` = `locals()` ∩ 签名形参，会把默认值里的五个宽度捞进来 ⇒ `_dump_battle`/`_gen_eval` 的 `REG.update(副档 reg)` 照常补齐（别把 `net.cfg` 改成只记非默认项）。验证：`AZNet(**REG)` 与 `AZNet()` 键集+形状全同、`in_dim` 仍 1382、副档 `reg` 仍 11 键、`_chk_res` 36/36。

- **线程 A/B 的三项落地（09-28）**：① 探针转正为守卫 **`_chk_threads.py`**（登记进 `_chk_all.py` engine 档 ⇒ 本地 **14/15**；它断言「同线程数跨进程权重 sha 相同」，换线程数只作**观测**；合成数据自包含，本地 14 s / 新实例 18 s；带真实缓存 `--epochs 64` 可复现那次实测）；② `--train-threads` **默认 8 → 1**（`_env.train_threads()`）⇒ **09-28 之前的单跑读数与之后的默认口径不是同一条轨迹**（比之前先看表头 `train_threads=`）；③ 合成随机数据上 1 vs 8 只差 1.9e-9（本机）/ 5.2e-8（新实例），真缓存 64 ep 才放大到 0.65 ⇒ 反证差异不是调度噪声。文档已同步：`.agent/memory/sts-lightspeed-rl.md` 续十三 + `notes.md` / `experiment-pitfalls.md` / `MIGRATION.md` / `doc/battle-net-summary.md` / `_sp_ab.py`。

- **发散护栏（09-28 续十四）**：`_az_train.py` 新增四条 NaN/Inf 闸 —— ①步级 `loss` 有限性（报 epoch/batch 进度、lr、batch、device 与**上一个有限 loss**）②每 epoch 末扫参数（`_first_nonfinite()` 报第一个坏参数名；与 ① 是两种病）③`save_ckpt` **拒存**非有限权重（能 strict 加载但评估恒 0% = 静默坏结果）④`check_determinism` 先查非有限再比一致性（`NaN != NaN` ⇒ 旧写法误报成「没切 eval」）。自证会拒（注入 NaN 参数 / `z` 灌 1e30 造 `loss=inf`）+ `_chk_threads.py` 回归 sha **逐位不变** ⇒ 零行为改动；引擎档 **14/15**。**过拟合不自动报警**（本项目选点指标是胜率不是 `val_ce`；也**不挑 best / 不早停**，否则等于用 val 选点）；用户未选 grad-norm 打点、**不默认裁剪**（裁剪=改配方）。⚠️ `_run_train.py`（整局网线）未加同类护栏。

- **场景口径两改（09-29，本地）**：① `devshape.draw` 从「截断正态 + 四舍五入 + 下限 0」换成 **Knuth 泊松**（只从 `rng.random()` 取数 ⇒ 仍是调用方那套 `random.Random`、不引 numpy）⇒ 低均值档不再被抬高（实测 0.3 → 0.30，旧法 0.372 / +24%）。② 遗物与药水**分家**：`scene.grant_shaped_relics`（掉落遗物走**引擎档位表** —— 新绑定 `gc.obtain_random_drop_relic()` = `returnRandomRelic(returnRandomRelicTier(gc.relicRng, gc.act))`、act4 全蓝；**金遗物从此会出现**，旧的「白∶蓝 4∶1」手写配比与 `devshape.relic_split` 已删）+ `grant_shaped_potions`；Boss 遗物件数 = `min(2, gc.act-1)`。顺带新增只读绑定 `sts.relic_tier(id)`（`getRelicTier`）。⚠️ **两条都改全部 seed 的取值** ⇒ 旧 `data/*.npz` 会与新数据**静默混用**（缓存闸只看 `space/obs_dim/task`）⇒ 或删旧缓存、或把口径写进 `task`/`src`。复核（一条）：`.venv/Scripts/python.exe _chk_shape.py` 全通过 + 遗物档位构成含 `RARE`/`BOSS`（`floor 51` 全 `UNCOMMON`，与引擎 act4 表一致）。`_chk_shape` 的牌组趋势检查同日改成「容差 1.5 张的单调 + 均值 ≈ 表值（±3 张）」：相邻段真差 ~1 张而单段 σ≈4 张，12 个 seed 做严格单调是掷硬币（Boss 遗物还会 ±3 张地动牌组：空笼 −2 / 召唤铃 +3 / 小房子 +1）。

- **DAgger 整条线已删（09-29，本地）**：`collect_dagger.py` + 失效旧副本 `_collect_dagger.py`（`git rm`；两者都在 HEAD `f0419fbd` 里，可找回）⇒ 采集层只剩 `collect_encs`（教师）/ `collect_selfplay`（自对弈）。连带：`collect.run_battle` 的 **`execute` 插槽一并删**（DAgger 是它唯一调用者）⇒ 现在 `run_battle(seed, enc, floor, label)`，记什么就执行什么；`collect` 的 `src` 取值只剩 `teacher`/`mcts`；`_az_train`/`_chk_shape`/`_eval_walk`/`_sync.sh`/`.agent/SCRIPTS.md` 的提及已同步；`_sync.sh` 的 `_JOBS_RE` 顺带改成认 `collect_[a-z]+`（活采集器早已没有 `_` 前缀，旧正则认不到、漏报作业）。⚠️ `realtest/comm_map.py` 的 `snakedagger`/`DAGGER_STAB` 是**游戏里的匕首怪**、不是 DAgger，别动；`.agent/notes.md` 与 `doc/battle-net-summary.md` 里的 DAgger 读数（v1 +0.6pp / v2 +0.1pp / 分布轴一阶结案）**保留**作历史。复核：`ls *.py | grep -i dagger`（应为空）+ `grep -rn "run_battle(" --include=*.py .`（应 2 处、都是 4 元）。

- **编码保护整层删除 + 规范落地（09-29，本地）**：用户定调「不需要任何兜底措施，出错立刻崩溃」，已写进 `.agent/CODE_STYLE.md`「失败与兜底」节 + 我的记忆 `/memories/code-style.md`。落地：`azaction` 删掉 `ActionEncodingError` / `_UNENCODABLE_WARNED` / `_warn_and_skip` / `_unique_idx_ok` / `_claim_idx` / `unencodable_count`；`--allow-unencodable`（`_flags.py` + `_env.allow_unencodable`）与 `aznet` 的显式重导出一起删；C++ `actionToIdx` 的 5 处 `return -1` 全改成**当场抛** `std::runtime_error("动作无法编码进统一下标：…")`（pybind11 → Python `RuntimeError`）；`SINGLE_CARD_SELECT` 分支本来就不查范围 ⇒ 越界由 Python 侧 `mask[i]` 的 IndexError 崩。哨兵各自备一份 `_bad_count`（`_chk_actions` / `_gen_eval`；**只数不抛**，规则允许），`_chk_select._probe` 内联判定，`_chk_actions._guard_selftest` 删除。复核（两条）：越界动作 `b.action_to_idx(sts.Action(sts.ActionType.CARD, 9, 0))` 应抛 `RuntimeError`；`_chk_actions`/`_chk_mask`/`_chk_select`/`_chk_res`/`_chk_revive` rc=0（09-29 实测）。⚠️ 该 C++ 改动**没推远端**（远端 `slaythespire.so` 仍是旧的 `-1` 语义）。

- **坑⑧（Pylance 桩缓存陈旧，别当真错）**：`build/Debug/slaythespire.pyi` 只在**语言服务器启动时读一次**；改绑定→重编（POST_BUILD 重刷桩）之后，Pylance 不会重读 ⇒ 已打开的文件继续报「"xxx" 不是模块"slaythespire"的已知属性」这类**假错**（实测：把桩里的 `action_layout` 整个删掉、原子替换，Pylance 的 hover 照样解析出它 ⇒ 根本没重读）。判定顺序：① `python -c "import slaythespire as s; print(s.<名>)"`；② 桩里 `grep "^def <名>"`；③ 新建一个探针文件写同一句 → 新文件不报错 ⇒ 就是缓存。解法：`Pylance: Restart Language Server`，或**碰一下报错文件本身**（`touch` 即可，内容不用改）。复核桩是否完整：`python -c "import re,io,slaythespire as s; src=io.open('build/Debug/slaythespire.pyi',encoding='utf-8').read(); pub={n for n in dir(s) if not n.startswith('_')}; print(sorted(pub-set(re.findall(r'^\s*(?:def|class) (\w+)',src,re.M))))"`（应为空）。

- **坑⑦（CMake 旗标务必「追加」）**：本机编译器是 **clang-cl**（`MSVC` 与 `CMAKE_CXX_COMPILER_ID=="Clang"` **同时为真**）⇒ `CMakeLists.txt` 里 `if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") set(CMAKE_CXX_FLAGS "...")` **覆盖**了平台默认的 `/DWIN32 /D_WINDOWS /GR /EHsc`，而 `/EHsc` 当时只挂在 `slaythespire` 一个 target 上 ⇒ `small-test`（也编译 `bindings/bindings-util.cpp`，里面有 `throw`）报 `cannot use 'throw' with exceptions disabled`。现态：`if(MSVC) add_compile_options(/EHsc)` 放在建 target 之前（所有 target 都吃到）+ 用 `string(APPEND CMAKE_CXX_FLAGS " ...")` 追加 GCC/Clang 旗标（`-O3` 仍排在 `CMAKE_CXX_FLAGS_<CONFIG>` 之前 ⇒ Debug 由 `/Od`、`-O0` 定调）。**别再用 `set(CMAKE_CXX_FLAGS ...)` 改全局旗标**。复核：`cmake --build build --config Debug`（全 4 个 target：`slaythespire.pyd`/`main.exe`/`test.exe`/`small-test.exe` 全绿）；CLI 里没有 `cmake`/`MSBuild`，只能走 VS Code 的 CMake Tools。

### 📌 2026-09-28 增量：`aznet`/`azlayout` 参数面改名（**零行为改动**）

- `FactorPolicyHead.__init__` 形参**按位置**传（唯一调用点 = `AZNet.__init__`），顺序定为
  「表示宽度 → 输出形状 → 开关」：`(hidden, card_rank, monster_rank, potion_rank, policy_dim,
  n_hand_slots, n_hand_targets, n_potion_slots, n_potion_targets, bilinear)`。
  ⚠️ 此前形参名顺序与调用点的位置实参顺序**不一致** ⇒ `flat_head`（旧名 `flat_rest`）宽度
  算成 `8−8×10−5×7 = −107`，`arch="factor"` **构造即崩**。
- 删掉两个**已死**条件（`AZNet` 永远会给）：手牌额外标量的宽度改为直接读 `HAND_EXTRA_N`；
  药水支无条件建（`k_potion` / `discard_tok`）。`FactorSelectHead` 不再收 `n_num`（读 `SEL_EXTRA_N`）。
- **改名（统一语义 = 除 id 外的「额外标量列」）**：头内 `hand_num`→`hand_extra_scalars`、
  `num`→`mon_extra_scalars`、`cand_num`→`cand_extra_scalars`；模块 `k_num`→
  `k_mon_extra_scalars` / `k_cand_extra_scalars`、`k_hand_num`→`k_hand_extra_scalars`、
  `flat_rest`→`flat_head`；张量 `*_tok`→`*_emb`（`hand_tok`/`mon_id_tok`/`pot_tok`/`cand_tok`）、
  `none_tok`/`discard_tok`→`none_target`/`discard_target`（这两个是 `nn.Parameter` 虚拟目标行，不是查表）；`azlayout`：`HAND_NUM_OFF/N/IDX`→`HAND_EXTRA_OFF/N/IDX`、
  `SEL_NUM_IDX`→`SEL_EXTRA_IDX`（新增 `SEL_EXTRA_OFF/N`；两个 `*_EXTRA_OFF` 都改成从
  `HAND_FIELDS` / `CARD_SELECT_FIELDS` **派生**，不再是手写 `(1,2,3)`）；`aznet` 缓冲
  `_idx_hand_num`/`_idx_sel_num`→`_idx_hand_extra`/`_idx_sel_extra`（缓冲名规则见 09-28 续五）。
- 同批清掉「为 reshape 单掏一个 batch 变量」的写法（`aznet.py` 里已无 `shape[0]` 取 batch）：
  展平用 `.flatten(1)`（打分头输出末维是 1 ⇒ 连 `squeeze(-1)` 都省）、虚拟目标行用
  `expand_as(对照张量的首槽)` 顺带拿 batch 维、其余 reshape 直接用 `-1`。
- ⚠️ **`state_dict` 键名变了**（`k_*` / `flat_head`）⇒ 旧 factor 权重 load 不了
  （`data/` 里只有 run 家族权重，factor 无落盘；`_diag_head.py` 的拒绝断言已同步）。
- 验证：`HAND_EXTRA_IDX`/`SEL_EXTRA_IDX` 与旧硬编码式**逐项相等**（30 / 180 列）；
  三种配置（flat / factor / factor+bilinear+sel factor）构造+前向正常（96 / 60）；`_chk_res.py` 36/36。
- 头内单字母入参也清了：`forward(q, …)`→`forward(state, …)`、中间量 `q4`→`query`
  （`self.q` 保留 —— q/k 是「查询/键」术语，且 `_diag_head` 靠 `select_head.q.weight` 推断架构）。
  ⚠️ 改名后**必须跑一次 factor 前向**（本次漏改 `flat_head(q)` 就是靠冒烟测试 `NameError` 抠出来的），
  并 `grep` 一遍旧名连**注释**一起扫（`n_hand`/`n_target`/`n_potion` 三条行尾注释就是这样漏网的
  —— 它们已是旧形参名、在新 `forward` 里根本不在作用域）。
- 两个头统一成同一结构：**「`cat`(嵌入, 额外标量列) → 一个 Linear」**。`k_card` + `k_hand_extra_scalars`
  → `k_hand`（`Linear(card_rank + HAND_EXTRA_N → hidden)`）；`k_cand_extra_scalars` 并入 `k_cand`
  （`Linear(card_rank + SEL_EXTRA_N → hidden)`）。与「两半各自线性再相加」是**同一函数类**（bias 相加
  可吸收，实测 `allclose`）。怪那一路**也合并了**：`k_mon` 直接吃 `cat([mon_id_emb, mon_extra_scalars])`
  （`k_mon_extra_scalars` 已删），`mon_dim` 随之为 `monster_rank + MONSTER_FIELDS = 8+12 = 20`
  （虚拟行 `none_target`/`discard_target`、目标 key `k_target`、配对投影 `n_mon` 都按它定义）。
  合并是**超集**：旧写法把数值半边的映射秩锁在 ≤ `monster_rank`。
- `FactorPolicyHead` 内部语句次序重排成 **卡牌 → 药水 → 打分**（`k_hand` → `k_potion`/`discard_target`
  → `q`/`k_target`/`none_target`/`out`/`flat_head`/配对项；`forward` 里两个槽 key 提到最前）。
  ⚠️ **模块创建次序变了 ⇒ 同 seed 的新初始化值也变了**（load 进来的权重不受影响）。
  验证法：同权重下用旧语句次序复现一遍比 `torch.equal`（本次 `pair=none`/`bilinear` 都 True）。
  ⚠️ 键名再变（`k_hand`/`k_cand`/`k_target` 输入宽度）；`_diag_head` 那句「缺 `k_hand` ⇒ 不喂手牌额外标量的旧权重」已同步。

### 📌 2026-09-28（续）：策略头配对项 `n_mon` + `n_mon_mix` → **单层**（超集，参数量 −16512）

- 配对项原来是 `n_mon: tar_dim→hidden` 再 `n_mon_mix: hidden→hidden`，但**中间没有非线性**
  ⇒ 两层精确等于一个仿射：`W₂(W₁t+b₁)+b₂ = (W₂W₁)t + (W₂b₁+b₂)`（实测 `allclose`；整头 logits
  最大差 1.2e-7 = float32 求和顺序）。现在只剩 `n_mon = Linear(tar_dim, hidden)`，`forward` 写
  `槽 key ⊙ self.n_mon(tgt)`（⚠️ 投影必须在 `⊙` **之前** —— 否则有效矩阵恒为对角，这条注释保留）。
- **合并后是严格超集**：`W₂W₁` 秩 ≤ `tar_dim`(20)（实测 20 = 上限）⇒ 旧两层只能表示「所有
  `hidden×20` 矩阵」里的零测子集，单层能表示全部。与 `k_card`+`k_hand_extra_scalars` 同型。
- 初始尺度不变：两层 bias 都初始化 0 ⇒ `c = 0`；乘积元素方差 = `hidden·(1/hidden)² = 1/hidden`
  ⇒ 与单层 `std=1/hidden**0.5` 同分布（实测 `hidden=64`：0.1237 vs 0.125）。
- ⚠️「`M`(hidden×hidden) 作用在已算好的 `k_target(tgt)` 上」是**等价但更贵**的写法：
  `k_target.weight` 列满秩 ⇒ `{M·W_t}` = 全部 `hidden×tar_dim` 矩阵 ⇒ 表达力一样，却要 `hidden²+hidden`。
- **参数量**（`hidden=128, depth=2`）：flat 225077 / factor 234879 / **factor+bilinear 237567**
  （旧 254079；−16512）/ factor+bi+sel_factor 248004。
- ⚠️ **`state_dict` 键再变**（`n_mon_mix.*` 消失）⇒ `_diag_head.py` 的 `pair` 推断改认
  `policy_head.n_mon.weight`，并把**旧两层权重**显式报错（附折叠公式）。实测磁盘上**没有**旧
  bilinear 权重（`data/*.pt` 只有 run 家族）⇒ 无实际作废。
- 验证：`py_compile`（`aznet`/`_diag_head`/`_chk_res`/`_az_train`）；四配置前向（96/60/1，B=1/4）
  + bilinear 分支全参数有梯度；`_chk_res.py` 36/36；`_chk_all.py` engine 档 **13/14**（唯一红项 =
  已知的 `realtest/comm_map.py` 旧副本 `KeyError: 'potion_bag_off'`，与本改动无关）。

### 📌 2026-09-28（续二）：`FactorPolicyHead` 签名瘦身 + 输出宽改为**真实产出**断言

- 删掉 5 个**已死**参数：`policy_dim` / `n_hand_slots` / `n_hand_targets` / `n_potion_slots` /
  `n_potion_targets`（旧 `flat_head` 用它们算宽度；改名 `end_head` 并把宽度写死 1 后就没人读了 ——
  实测：用乱写的值建头，`state_dict` 形状与前向输出**逐位相同**）。签名现在 =
  `(hidden, card_rank, monster_rank, potion_rank, bilinear)`。
- 宽度契约从「推导式」换成 `AZNet.__init__` 末端的**端到端断言**：构造时跑一次
  `self(zeros(1, OBS_DIM))`，断言 `p.shape[-1] == POLICY_DIM`、`s.shape[-1] == SELECT_DIM`。
  ⚠️ 跑前临时 `eval()`：实测 train 前向**吃 RNG**、eval 不吃（构造期吃 RNG 会悄悄移动别处的
  初始化流）。⚠️ 该断言在 `arch="flat"` 下**恒真**（宽度就是 `Linear(hidden, POLICY_DIM)` 定义的）
  ⇒ 它保护的是 factor 臂（实测 `POLICY_DIM=999` 时 factor 抛「产出宽 96 != 999」，flat 不抛）。
- 同波更新注释：`FactorPolicyHead` docstring 写明输出宽公式与契约位置；`azlayout.POTION_TARGET_N`
  那句「定义放这里是因为头要建网格」改成「契约由 `AZNet` 按真实产出核对」。
- ✅ **已处置（2026-10-02）**：选了「删掉 `pair` 开关让它常开」—— 见本条上方 2026-10-02 那条。
  （历史现状：`n_tar` 无条件构造 ⇒ `pair="none"` 时它有参数但不进前向，梯度恒 None、永远停在
  初值，state_dict 白多 2688 个权重；factor 与 factor+bilinear 参数量同为 **237567**。）
- 验证：`py_compile`；四配置前向（96/60/1，B=1/4）；参数量 flat 225077 / factor 237567 /
  factor+bi 237567 / factor+bi+selfact 248004；`_chk_res.py` 36/36。

- 同风格重排：`FactorSelectHead` 也改成与 `FactorPolicyHead` 一致的形状 —— 无 docstring、
  `# ── key: cat(emb, scalars) → linear ──` / `# ── score：（q + key） → linear ──` 分段、
  `forward` 里 `query` 前置、`key`→`key_cand`、行尾注释与形状注释对齐。
  ⚠️ **`__init__` 里 `k_cand` 现在建在 `q` 之前 ⇒ 同 seed 的新初始化值又变了**（`nn.Linear` 建的时候
  就抽随机数；键名 / 形状 / 参数量均不变：select 头仍 `q`/`k_cand`/`out`）。
  掩码契约（候选 `cardId != 0`、非法/空槽整槽填 0）从 docstring 挪到 `forward` 的行内注释
  （`AZNet.select_keys` docstring 里本来就有一份，不会丢）。验证：`_chk_res.py` 36/36。

- ⚠️**【2026-09-28 续十五：已删除】** `self.cfg`（当时的写法 = 不手抄、`locals()` ∩ 形参名）：
  `{k: v for k, v in locals().items() if k != "self" and k in inspect.signature(type(self).__init__).parameters}`
  ⇒ 以后加形参自动进 cfg（此前是 11 个键的手写 dict；漏加会**静默**记成默认值 —— `dropout` 这类
  不进 `state_dict` 的参数，存盘那行 `strict=True` 自检抓不到）。
  ⚠️ 两个实测坑（第一版都被断言 `net.cfg == 形参默认 | 覆盖` 抓住）：① `locals()` 里有 **`__class__`**
  （零参 `super()` 造出来的 cell 引用）—— class 对象不可 JSON 序列化 ⇒ 副档会**直接写不出来**；
  ② `inspect.signature(type(self).__init__).parameters` **含 `self`** ⇒ 要显式排掉。
  验证：两配置 `cfg` 逐项等于「形参默认 | 覆盖」、`_az_train.save_ckpt` 副档写得出来且**零告警**
  （即自检通过）、`_chk_res.py` 36/36。`cfg` 的唯一读者仍是 `save_ckpt`（`getattr(net,"cfg",None) or REG`），
  副档 `reg` 的下游 8 个脚本不变。

### 📌 2026-09-28（续五）：`AZNet` 的 buffer 命名统一（**纯改名，零兼容性影响**）

- 规则定稿：**`_idx_` + 「常量名小写、去掉尾部 `_IDX`」**（机械可推，加常量时不用再想名字）。
  只有两个例外：`scale`（不是索引表，且是**唯一 `persistent=True`** —— 它在 `state_dict` 里，
  改名会作废战斗网 ckpt，**不动**）、`_sel_region_mask`（**布尔掩码**，明确不属 `_idx_` 家族）。
- 改了 5 个：`_idx_card`→**`_idx_card_id`**（旧名丢了 `_id`）、`_idx_mon`→**`_idx_monster_id`**
  （旧名把 `monster` 缩成 `mon`、也丢了 `_id`）、`_idx_mon_row`→`_idx_mon_id_row`、
  `_in_sel_region`→**`_sel_region_mask`**、`_pile_n`→`_pile_lens`（普通属性，不是 buffer）。
- ⚠️ 替换坑：`_idx_mon` 是 `_idx_mon_row` 的**前缀** ⇒ 这两个不能在同一批里做
  （会变成 `_idx_monster_id_row`）。本次改用**带上下文的锚点**
  （`_idx_mon: torch.Tensor` / `("_idx_mon", …)` / `self._idx_mon).long()`）消掉前缀歧义，一次批量完成。
- 验证：旧名残留扫描 5 条全 0；`_buffers` 键 = 新名；**`state_dict` 仍 14 键、`scale` 仍在、
  无任何 `_idx_`**（⇒ 与旧权重完全兼容，`persistent=False` 的那些本来就不进 ckpt）；四配置前向
  96/60/1；参数量不变；`_chk_res.py` 36/36。
- ✅ 遗留（**已于 2026-09-28 续十 解决**）：常量本身曾不齐（`MONSTER_ID_IDX` vs `MON_ID_ROW_IDX`）——
  后者已删，改由 `azlayout` 提供 `MONSTER_EXTRA_IDX`（缓冲名 `_idx_monster_extra`）。

### 📌 2026-09-28（续六）：`AZNet` 注释同风格收敛（**纯注释，零行为改动**）

- 统一成与两个头同款的分段风格（`# ── 段名 ──` + 契约短句），类 docstring 46 → 14 行。
- 四大块各压到 2–4 行：**buffer 块**（两份理由 + `persistent=False` 一句 + `scale` 是探针）、
  **自检块**、**`cfg` 块**、**CAT_SPEC 块**；形参表从 18 行逐条改成 6 行分组。
- 删掉的全部是**实测数字 / 历史叙事 / 会过期的描述**：`86 µs`、`419 µs`、`train_ce 0.685→0.022`、
  `（以前是一个共享的 embed_dim）`、`（开关已删）`、`铎错`、`把写惯的那套搬错网` 之类的复述。
- 保留的仍是**契约/不变量**：五种 id 宽度决定 `in_dim`、嵌入表「行 0 = 空槽、可学」（禁 `padding_idx=0`）、
  牌堆只给集合（不传堆序）、`index_select` 只收 Tensor、buffer 为何要注册、`scale` 当布局探针、
  自检为何临时 `eval()`。
- 验证：`py_compile`；四配置前向 96/60/1（h=64 参数量 114165/116287/116287/117380，与改前逐项相同）；
  已删短语 grep 残留 0。
- ⚠️ 未动：文件头**模块 docstring**（仍带「表面与拆分前一致」这类历史句）—— 要收敛就是下一刀。

### 📌 2026-09-28（续七）：值目标加 `enginenorm` 口径 + **设为默认**

> ⚠️ **本节记录的是中途形态（赢局支去 35 + 败局恒 −1）—— 已被同日「续八」取代。**
> 现口径请以续八为准：`enginenorm` = 引擎整式 ÷ `100*maxHp`，`t_pot` 列也换成了 `t_escore`。

- 新口径 `enginenorm`（`_zspec.py`）：引擎 `evaluateEndState` 的**赢局支**去掉公共因子 100 与
  赢局基线 35，只留 `curHp + 4*pot - 0.01*turn`，再按玩家 `maxHp` 归一
  ⇒ `(t_hp + 4*t_pot - 0.01*t_turn) / t_max_hp`；**败局仍是常数 −1**。
- 为什么这样归一：`mcts_search` 的 `q = W/N` **未归一化** ⇒ 换尺度 = 偷偷改探索强度
  （`hpw` 就是这么失败的）。新口径赢 ∈ [0, ~1.1]、败 −1，与 `old` 同级 ⇒ `c_puct` 不用重扫。
- 为什么不拄引擎整式：引擎的败局支还有 5 项，其中 `energyWasted` / `cardsDrawn`
  是 `BattleContext` 的**现成字段**（`BattleContext.h:57,58`）但**既不在快照也没绑定**
  ⇒ 要全对齐得加 2 行 `def_property_readonly` + 重编 pyd（本机 CMake Tools 可做）。
  当前选的是「赢局形状照拄引擎、胜负落差不变」。
- ⚠️ **新终局列 `t_pot`**（药水瓶数）：`Z_COLS` 6 项。药水槽没绑到 Python ⇒ 从快照
  `potion_meta` 块读（`azlayout.POTION_COUNT_IDX`）。**旧缓存没有这一列** ⇒
  `z_values` 会响亮报错并把 `rew` 指出来，跑一次 `_z_revalue.py rew` 补列即可
  （零搜索，**不是重采**）。`old` 不读 `t_pot` ⇒ 旧缓存在 `--z-spec old` 下照常可用。
- 📌 **在本 boss 场景 `t_pot` 恒 0**（`_boss` 从 0 药水开局、战斗中不会获得；`_z_revalue stats`
  实测「0 个决策点上有合法药水动作」）⇒ 药水项今天是个 no-op，存列是为了以后场景换。
- 连带改动：`_z_revalue.cmd_rew` 的列清单改读 `Z_COLS`（不再手抄字面量，加列只改一处）；
  `_az_train.save_ckpt` 的副档**新增顶层 `z_spec`**（否则 `val_ce` 跳口径静默不可比）；
  `_flags --z-spec` 帮助文本、`.agent/MIGRATION.md`、`doc/battle-net-summary.md` 同步。
- 验证：`py_compile` 四个文件；手算核对（赢 80/80 满血、10 回合 → `0.99875 = (80-0.1)/80`，
  10 血 + 2 瓶药 → `0.22375 = (10+8-0.1)/80`）；缺 `t_pot` 响亮报错、补齐后自检过；
  **本地引擎真跑 `collect 3 20` + `train(epochs=1)` 端到端通过**（落盘列含 `t_pot`，
  日志打印 `值目标口径 = enginenorm`）。
- ⚠️ **未决**：败局是否照引擎（要不要加 `energyWasted`/`cardsDrawn` 绑定 + 3 个新终局列）——
  用户当时反问「这些数值是现成还是要额外统计」，答完待其定。另：历史 val_ce / 胜率
  **全部只与 `--z-spec old` 同口径**。

### 📌 2026-09-28（续八）：`enginenorm` 定稿 —— **整式照抄引擎**（含败局支）+ 删三个口径

- 口径 = `t_escore / (100 * t_max_hp)`；`t_escore` 是**引擎自己的终局分**，由新绑定
  `sts.evaluate_end_state(b)` 取得（`bindings/slaythespire.cpp`），即
  `BattleScumSearcher2::evaluateEndState` 的返回值：
  赢 `100*(35 + curHp + 4*药水 − 0.01*回合)`、
  输 `10*(1−非小怪血比) − 活怪数 − 0.2*能量浪费 + 0.03*抽牌 + 2*药水 + 0.2*回合`。
- 为什么**绑 C++ 而不是在 Python 拼**：败局支要 `energyWasted` / `cardsDrawn`（**未进快照**）、
  非小怪血比、THREE/FOUR_SHAPES 特判 ⇒ 抄一遍就是等着静默漂开。（那两项本身是
  `BattleContext` 的**现成字段** `.h:57,58`，零统计成本，只是没导出。）
- ⚠️ **35 基线不能去**（续七那份去掉 35；一旦败局也照引擎就必须加回）：实测 40 场 @500 sims
  （赢 15 / 输 25）—— 引擎分赢 **3590~9293**、输 **4.4~11.2**；去 35 后赢局下限 ~0.01
  会与败局区间**重叠** ⇒ 网络学不出「赢比输好」。加 35 后：赢 **0.479~1.239**（mean 0.81）、
  输 **0.0006~0.0015** ⇒ 序成立，落差 **0.477**。
- ⚠️ 尺度变了（`old` 落差 1.0+）⇒ `q = W/N` 未归一化 ⇒ **先扫 `c_puct`（按 ~0.48 缩）**。
- 删口径：`engine` / `lossshape` / `hpw`（连 `HP_W` 常量）。现存 = `enginenorm` / `old` / `hp_turn`。
- 终局列 `t_pot` → **`t_escore`**（药水项已折进引擎分）。旧缓存跑 `_z_revalue.py rew` 补列
  （零搜索，不是重采）；`old` / `hp_turn` 不读它 ⇒ 在那两个口径下照常可用。
- 连带：`_z_revalue.cmd_rew` 列清单读 `Z_COLS`、副档顶层 `z_spec`、`_flags --z-spec` 帮助、
  `.agent/MIGRATION.md`、`doc/battle-net-summary.md`。
- 验证：`py_compile` 四文件；绑定可用；手算核对；**本地真跑 `collect 40 500`**（上表两分支
  实测范围/序）+ `train(1 ep)` 通过（旧口径自检过、`old`/`hp_turn` 仍可用、`hpw` 报「未知口径」）；
  `_chk_all.py` engine **13/14**（唯一红项仍是已知的真机桥接）。
- 📌 历史 val_ce / 胜率**只与 `--z-spec old` 同口径**。

### 📌 2026-09-28（续九）：战斗网的秩/初始化尺度收回 `aznet` 自己持有

- 现状：`aznet.RELIC_RANK = 16` / `POTION_RANK = 8` / `INIT_STD = 0.02`（模块级三连赋值，
  紧跟 `# ---- 网络` 段头），`AZNet` 签名默认与三处 `nn.init.normal_` 都读它；
  **`import multi_hot as MH` 已删**（`aznet.MH` 属性随之消失 —— 全仓无人依赖，已 grep）。
- `multi_hot.py` = **整局网 `runnet` 专用**；它的 `RELIC_RANK`/`POTION_RANK`/`INIT_STD`
  现在明确写成「整局网的默认值」，文件头那句「只留 INIT_STD 与两个秩常量」也跟着改了。
- 为什么可以各持一份：两网 09-25 分家后权重互不相干，`_res_prior.py` 搬表时秩不等
  形状当场对不上（响亮）；初始化尺度不同也不构成任何契约。
- 连带：`_chk_res.py` 里断言 `relic_linear` 形状那条改用 `aznet.RELIC_RANK`
  （runnet 的 in_dim 公式仍用 `MH.RELIC_RANK`）；`_az_train.REG` 的注释同步。
- 验证：`aznet` 里 `MH` 残留 **0**、`hasattr(aznet,'MH')` False、三方数值逐位同（16/8/0.02）
  ⇒ `in_dim` **数学上不可能变**（实测仍 1382）、`relic_linear=(16,181)`、`potion_embed=(44,8)`、
  四配置前向与参数量 114165/116287/116287/117380 不变、`_chk_res.py` **36/36**。

### 📌 2026-09-28（续十）：怪物目标行「像手牌一样」—— 补 `MONSTER_EXTRA_*`，两个 id 嵌入放开头

- 起因：`MON_ID_ROW_IDX`（aznet 本地拼的**整块 60 列**）与手牌/候选的处理不对称 ——
  `HAND_EXTRA_OFF = range(1, HAND_FIELDS)` / `SEL_EXTRA_OFF = range(1, CARD_SELECT_FIELDS)`
  都是「块内**除名义列外**」，而怪物的 extra 把 `[0] monsterId` / `[4] intentMoveId`
  又当标量喂了一遍。这两列在 `CAT_IDX` 里、`SCALE` 被盖成 1 ⇒ 进 `k_target` 的是
  **原始 id 数值**（0..65 / 0..196），旁边 10 列是 `/80` 一类的归一化值（≤ ~1.5）——
  量级差 10~200×，而那个「大小」本身没有语义。
- 现在：`azlayout` 新增 `MONSTER_EXTRA_OFF`（= 排除 `(0, 4)`，10 项）/ `MONSTER_EXTRA_N` /
  `MONSTER_EXTRA_IDX`（5×10）—— 与 `HAND_EXTRA_*` 同形；并加一条断言按 `CAT_IDX` 对账
  （「块内哪些列是名义列」只由 `CAT_SPEC` 说了算）。`aznet.MON_ID_ROW_IDX` 删除。
- 目标向量布局改为 **两个 id 嵌入在前、数值标量在后**（与手牌 key 同形）：
  `mon = cat([mon_id_emb, mon_move_emb, mon_extra_scalars])` ⇒ `tar_dim` 20 → **26**。
  `FactorPolicyHead` 因此多收一个 `intent_move_rank`；**招式 id 进目标 key 是新信息**
  （以前那列是原始整数，现在是它自己的嵌入）。
- buffer `_idx_mon_id_row` → **`_idx_monster_extra`**（照续五规则：`_idx_` + 常量名小写去 `_IDX`）。
- 代价：只动因子化头 —— `k_target` / `n_tar` / `none_target` / `discard_target` 的目标维
  20 → 26（h=64 参数量 factor 116287 → **117067**）。**trunk / `in_dim`（仍 1382）/ flat 臂 /
  一切缓存都不受影响**（`tar_dim` 不进 `in_dim`）。
- 验证：`py_compile`；`MONSTER_EXTRA_OFF = (1,2,3,5,6,7,8,9,10,11)`；`policy_keys` 六个张量形状
  `(10,8)/(10,3)/(5,8)/(5,8)/(5,10)/(5,8)`；四配置前向 96/60/1；aznet 旧名残留 0；`_chk_res` **36/36**。
- ⚠️ 顺手发现（**未修**）：`_diag_head.py` 无副档回退路径里的
  `pair="bilinear" if "policy_head.n_mon.weight" in sd else "none"` —— ① `n_mon` 已改名 `n_tar`；
  ② `n_tar` **无条件构造** ⇒ `pair` 根本推不出来，bilinear 权重会被**静默**当成 `none` 加载
  （键/形状全同）。磁盘上目前无 factor 权重 ⇒ 影响为 0；有副档时走 `reg` 正确。

### 📌 2026-09-28（续十一）：「开关不推、只决定用不用」定调 + `_diag_head` 回退路径改成拒绝

- 用户定调（已写进 `.agent/CODE_STYLE.md` 新增节「开关与权重」）：**权重永远存在，
  开关只决定「要不要用上」。** ⇒ ① 不要为省参数把构造写进 `if <开关>`（权重会随开关存在/消失）；
  ② **开关不能从 `state_dict` 反推** —— 两种取值下键集合与形状全同（`strict=True` 拦不住），
  重建网络必须有配置记录（`save_ckpt` 副档）。
- 落地：`_diag_head.build_for` 的键名回退路径删掉 `pair="bilinear" if "policy_head.n_mon.weight"
  in sd else "none"`（名字早已过期 + 逻辑根本不成立）；现在 `arch == "factor"` 且**没有可用副档**
  时**直接 SystemExit**（旧的 `n_mon_mix` / `m_card` 专属报错仍在前，优先命中）。
  `_来源` 改成 `"键名推断（flat 臂）"`（回退路径只剩 flat 能走到）。
- 验证四条路径：flat 无副档 → 推断；factor 无副档 → 拒绝；旧两层 `n_mon_mix` → 专属报错；
  有副档 → 走 `reg`（`pair=bilinear` 正确传到 `policy_head.bilinear=True`）。
- 注：`arch` / `sel_arch` 的键名推断**仍然合法** —— 那是**换模块**（结构性差异），不是开关。

### 📌 2026-09-28（续十二）：五个嵌入宽度收成 `aznet` 常量，**不进 `REG`**（数字只写一处）

- 起因（用户）：「net 的超参不进 REG 吗？」 → 中途试过「REG 引用常量」，用户随即定调
  **「那这些也不用留了」** ⇒ 终案：**宽度不进 `REG`**（唯一来源 = `aznet` 的签名默认值）。
- 现在：`aznet` 顶部 = `CARD_RANK, MONSTER_RANK, INTENT_MOVE_RANK = 8, 8, 8` +
  `RELIC_RANK, POTION_RANK, INIT_STD = 16, 8, 0.02`（**唯一来源**，`AZNet` 签名默认值读它）；
  `_az_train.REG` 只剩**训练侧的选择** 6 键：`hidden` / `dropout` / `depth` / `arch` / `sel_arch` / `pair`。
- **分工**：**宽度/秩 = 本网的身份**（归 `aznet`，且**不进 `REG`**）；容量与开关 = 训练侧选择
  （归 `REG`，都有 CLI flag）。测试脚本想扫宽度就直接给 `AZNet(**{**REG, "card_rank": 16})`。
- ⚠️ 副档仍然完整：`net.cfg` = `locals()` ∩ 签名形参，**会把默认值里那五个宽度捞进来** ⇒
  `_dump_battle.py` / `_gen_eval.py` 的 `REG.update(副档 reg)` 照样补齐宽度（这两处是唯一的
  「从副档重放配置」路径，别把 `net.cfg` 改成只记非默认项）。
- 验证：`REG` 键 = 6 个；`AZNet(**REG)` 与 `AZNet()` 键集+形状全同、`in_dim` 仍 1382；
  副档 `reg` 仍 11 键（含五个宽度）；`_chk_res` **36/36**。

### 📌 2026-09-28（续十三）：训练线程数 —— **换线程数 = 换轨迹**；同线程数跨进程/跨 torch 版本**逐位可复现**

- 触发：`_train_seed` 的 docstring 原本写「多线程下同配置同种子跳进程都会漂（`val_ce` 差 0.008）」。
  用探针 `_chk_threads.py`（跑**两个独立进程**比权重 `sha`；当时是临时脚本 `_tmp_ab28.py`，同日转正）
  在固定条件（`data/_ab28.npz` = 400 场 @500 sims / 12811 样本、`train(seed=0, epochs=64)`）下重测。
- **实测**：
  | 机器 | 1 线程 `val_ce` | 8 线程 `val_ce` | `max\|Δw\|` | 同线程数跨进程 |
  |---|---|---|---|---|
  | 远端 torch **2.8.0+cu128** | `1.3231931329694746` | `1.3065794011724787` | 0.6465 | 逐位相同（两遍） |
  | 远端 torch **2.12.1+cu130** | **与上行逐位相同** | 同上 | 0.6465 | 逐位相同（两遍） |
  | 本机 torch **2.13.0+cpu** | `1.3210145458003284` | `1.331649096340079` | 0.8947 | 逐位相同（两遍） |
- ⇒ 三条结论：
  1. **线程数是配方的一部分**：1 与 8 线程不是同一条轨迹（`val_ce` 差 0.01~0.02、权重最大差 0.6~0.9）；
  2. **同线程数、跨进程 ⇒ 逐位可复现** —— 连 **3 路并发 × 8 线程（24 线程挤 16 核，刻意超订）**
     的三个进程也逐位相同 ⇒ 旧笔记那句「8 线程下跨进程**偶发**不确定」**今天没复现**；
  3. **跨 torch 版本（2.8.0+cu128 ↔ 2.12.1+cu130，同为 Linux/x86-64）⇒ 逐位相同** ⇒ 那个 0.0166
     是**确定性的 kernel 分块效应**，不是随机调度噪声（否则跨版本不可能连小数第 16 位都一样）。
- ⚠️ 差异幅度**随训练步数放大**：同机 2.8 上 32 ep 只差 0.0007、64 ep 差 0.0166 ⇒ 不是常数。
- ⚠️「**单步**算子逐位相同」与「**整段训练**逐位相同」是两件事：探针的 `patho` 臂（值目标 ×1e5、
  loss ≈ 5.95e9）在两端都 `max|Δg| = 0` ⇒ 09-21 那条「算子都在 `grain_size` 以下 ⇒ 走同一条
  串行路径」**仍然成立**，但它**推不出**整段训练逐位相同（差异是逐步累积放大的）。
- ⚠️ **勘误**：本文下方「已验证结论」里那条 🚨（09-13 记、09-21 复核过）的措辞不按字面成立；
  我中途还一度记过「本机 1 vs 8 逐位相同」，**那条是错的**（旋钮从 `STS_TRAIN_THREADS` 迁到
  `--train-threads` 之后，两臂很可能跑的是同一个线程数 —— 这正是探针里要放线程数断言的原因）。
- 处方不变、理由换新：`seeds` 并行训练仍 `--train-threads 1`（单线程数值**不依赖调度**
  ⇒ 3 路并发时各臂互不污染）。⚠️ 顺带暴露一个不对称：`n_seeds>1` 走 1 线程，而 `n_seeds==1`
  直接 `train()` 走 `--train-threads` —— 而它**默认已改成 1**（同日决定）⇒ 这个不对称消失了。
- **复现**：`python _chk_threads.py data/_ab28.npz --epochs 64`（内部起子进程、比 `sha`）；
  夹具缓存留在 `data/_ab28.npz`（本地与两个远端实例都有副本）。
- **同日落地三件**：
  1. 探针转正为守卫 **`_chk_threads.py`**（在 `_chk_all.py` 的 engine 档，已过）：合成数据自包含、**~14 s**；
     断言「同线程数跨进程权重 `sha` 相同」，两个线程数之差只当**观测**（将来 torch 把两者做一致了也不该报红）。
  2. `--train-threads` 默认 **8 → 1**（`_env.train_threads()`）：**配方一致性优先于速度**，要提速就在
     任务层铺进程。⚠️ 因此 **2026-09-28 之前的单跑读数与之后的默认口径不是同一条轨迹**（跨口径比要先看表头的 `train_threads=`）。
  3. 合成随机数据上 1 vs 8 只差 **1.9e-9**（`val_ce` 打印位相同）⇒ 差异是**脚下就存在的 1e-9**
     被不稳定的过拟合轨迹吹大的（真缓存上 64 ep 放大到 0.65）——这也反证它不是调度噪声。
- **机制二分（同日，`_probe_thread_mech.py`，本地 torch 2.13）**：把「多线程改了哪一步」逐个拆开测 ——
  `randperm` 置换 / dropout 掩码 / 纯 GEMM（4 种形状）/ Adam 的两行逐元素算子
  （`mul_.add_`、`mul_.addcmul_`）/ 真实数据单步梯度 / 只跑 `Adam.step()`（两边参数与梯度先钉成
  逐位相同）—— **全部逐位相同**；**同线程数重复跑也逐位相同**（对照干净，不是测量噪声）。
  唯一分叉的是**把它们串起来跑多步**：第 1 步参数逐位相同、但动量 `trunk.0.weight.exp_avg`
  差 **2.3e-13**；第 2 步参数差 **1.9e-9**；64 epoch 后 0.65。
  ⇒ ① 1 与 8 线程**各自都是确定性的** ⇒ 多线程不是「随机」，而是给出**另一个确定的结果**；
  ② 触发点是**多算子串联下的分块/归约边界**（孤立算子测不出来），精确指认还要继续二分（本次未完成）；
  ③ 1e-9 之所以能长成 0.65，是因为训练处在病态/过拟合 regime（这份小缓存 val R² ≈ −0.17）
  ⇒ 小差异被逐步指数放大（合成随机数据 4 ep 只差 1.9e-9；真缓存 64 ep 差 0.65；真缓存 32 ep
  的 `val_ce` 只差 0.0007）。

### 📌 2026-09-28（续十四）：训练发散护栏（NaN/Inf）落地 + **「过拟合不自动报警」定调**

- 起因（用户）：「当前似乎没有对网络有过拟合和 NaN 崩溃的检测和警告？这在现代神经网络任务里面是不是不太好」
- **NaN/Inf：确实缺，已补四条**（全在 `_az_train.py`）：
  1. **步级 `loss` 有限性**：报错带 epoch/batch 进度、`lr`、`batch`、`device`，以及**上一个有限
     loss**（`loss.detach()` 取值，只多一次同步）。
  2. **每 epoch 末扫参数**（`_first_nonfinite()` 报出第一个坏参数名）。与 ① 是**两种病**：
     ① 在 loss/前向侧，② 在更新/优化器侧（loss 仍有限但权重已坏）。
  3. **`save_ckpt` 拒绝存非有限权重**（抛 `RuntimeError`）：这种 ckpt 能通过 `strict=True`
     加载（形状对得上）但评估恒 0% ⇒ **静默**坏结果，必须在源头拦。
  4. **`check_determinism` 先查非有限值再比一致性**：`NaN != NaN` ⇒ 旧写法会把发散
     误报成「是不是没切 eval」（完全不同的病）。
  - 自证会拒（注入 NaN 参数 / 把 `z` 灌成 1e30 造 `loss=inf`）：四条都按预期响；
    `_chk_threads.py` 回归 sha 与改前**逐位相同** ⇒ **零行为改动**。
- **过拟合：不自动报警**。这项目的选点指标是**胜率**而不是 `val_ce` —— e32 的 val 改善不买胜率、
  这份小缓存 `val R² ≈ −0.17` 是常态、软目标的 KL/NLL 与胜率**反相关** ⇒ 加「gap 超阈值就 WARN」
  只会天天响然后被无视。正确形式 = gap 继续**打印**（已有）+ 离线 `_imit_probe.py`
  （NLL硬/top1/校准 KL\*）+ 要加就加「`val_ce` 连续 N 轮上升」的**纯日志**一行。
  同理**不挑 best、不早停** —— 那等于用 val 选点，会把「val 与胜率脱钩」重新搅浑。
- 用户**未选**：global grad-norm 打点（发散前兆最灵敏）。**不默认裁剪**（裁剪 = 改配方）。
- ⚠️ `_run_train.py`（整局网训练器，另一条线）**没有**同类护栏。
- **同日后续（`save_npz` 拆除 + 全仓「失效导入」审计）**：
  · 用户问「`save_npz` 还有没有必要」。实测三条**都成立**（`np.savez` 真带 `allow_pickle=True`
    ⇒ 键名撞上会静默丢一整列；Pylance LSP 实测 `dict[str, ndarray]` 与 `dict[str, object]`
    解包**都会** reportArgumentType，只有 `Any` 能过；它有 4 个调用点）—— 但用户定调**拆掉封装**。
  · 落地：四处调用点改内联 `np.savez(cache, **data)`，`Any` 放宽挪到**生产者**两处
    （`_assemble` 的返回注解 + `selfplay` 里的 `out: dict[str, Any]`）⇒ 写入点不用各写局部别名；
    规则与代价写成 `_az_train.py` 采集段头的一段注释。**LSP 复验：四个文件都无 reportArgumentType**；
    端到端 `collect 6 40` 真写 → 读回 18 个键齐全。
  · ⚠️ 代价（已写进代码注释）：**不再拦**「某一列真叫 `allow_pickle`」—— `save_npz` 的 `ValueError`
    没了；`_z_revalue.py` / `_collect_run.py` 的直接写入本来就没这道闸。
  · 顺手用 `workspace/diagnostic` 审计「未知的导入符号 / 未定义」：全仓 **5** 处 —— `_old/` 3 处
    （归档，允许腐烂，含刚删的 `save_npz`）、`realtest/_chk_comm.py` 1 处（已知红项那条线），
    **唯一现役且真崩的**是 `_collect_dagger.py` 里 `from _env import ROLLOUT_THREADS`
    （旋钮迁移漏改的一个**函数内** import ⇒ 该脚本一跑就 ImportError；已修为 `rollout_threads()`）。
    ⇒ 教训：**迁移时函数体内的局部 import 最容易漏**（模块级 import 都被 grep 抓到了）。
  · **未修（不在本次范围）**：`_collect_dagger._dagger_battle` 里 `net_action(...)` 的返回值没查
    `None`（`reportOptionalMemberAccess`，与 `_play` 的 `if a is None: break` 不一致）；
    `realtest/_chk_comm.py:136` 一处「int 不可迭代」。

### 📌 2026-09-28（续十五）：删掉 `AZNet.cfg` —— 配置以**脚本里的 `REG`** 为准

- 起因（用户）：「`#sym:cfg` 搞这些花哨的参数保存没什么意义，需要保存的时候参数就在脚本文件里面写明了」。
- 落地：`aznet.AZNet.__init__` 里那条 `self.cfg = {k: v for k, v in locals().items() if ...}` 删除
  （连同它 4 行注释），原地留一句契约：「网络**不记**配置；配置就是 `_az_train.REG`（+ CLI 覆盖）」。
  `_az_train.save_ckpt` 改成 `cfg = dict(REG)` —— 它的**唯一读者**就是这里，所以只动了两个文件。
- 验证：`py_compile`；真跑 `train data/_ab28.npz 1 0 <out>` ⇒ 训练/存盘/确定性自检都正常，
  副档 `reg` = 6 键（`arch/depth/dropout/hidden/pair/sel_arch`）+ `z_spec=enginenorm` + `space=96`；
  引擎档 `_chk_all.py` **14/15**（唯一红项仍是真机桥接）。
- ⚠️ 两个后果（有意接受）：
  1. **五个嵌入宽度不再进副档**（原来的 `locals()` 拄法会把签名默认值一起捞进来）。宽度唯一来源仍是
     `aznet` 签名默认值 ⇒ 用副档重建（`_diag_head`/`_dump_battle`/…）拿到的就是默认宽度；若真有人用
     非默认宽度存盘，重建会在 `load_state_dict` **响亮**失败 —— 这是偏好的失败方式，不是静默错配。
  2. `save_ckpt` 里那句 `AZNet(**cfg).load_state_dict(..., strict=True)` **自检保留**：现在它验的是
     「脚本里的 `REG` 能不能装下这份权重」，正好等价于「权重是不是用当前配方建的」。
- 📌 副档（`<weights>.json`）**本身保留**：它有 9 个下游（`_diag_head`/`_dump_battle`/`_gen_eval`/
  `_imit_probe`/`_probe_capacity`/`_probe_runrange`/`_res_prior`/`realtest/comm_battle` + `net_kwargs`），
  删它就不是「去掉花哨」而是「换一套重建协议」—— 要删得单独决定。

### 📌 2026-09-28（续十六）：权重存档改**单文件自描述**（`_ckpt.py`）—— 删掉 `.json` 副档

用户定调：「这种副档…很粗鲁，不够优雅」+「我不认识 reg 是什么单词，所以 B」⇒ 选单文件自描述。

- **格式**（`_ckpt.FMT = 1`）：一个 `torch.save` 的 dict = `{fmt, weights, config, <元数据>}`。
  战斗网元数据 = `space`/`z_spec`/`metrics`；整局网 = `train`（训练旋钮）+ `run_dim`/`obs_schema`。
  **`config` 一律 = 建网配置**（战斗网 `_az_train.CONFIG`、整局网 `_run_train.CFG`）⇒ 读侧
  `AZNet(**net_kwargs(cfg))` / `RunNet(**cfg)` 直接用，**不需要**过滤器（`run_kwargs` 因此删掉）。
- **唯一读入口 `_ckpt.py`**：`read()`（归一化）/ `config_of()`（取不到返回 None，**不猜默认值**）/
  `weights_of()` / `legacy_sidecar()` / `migrate()`。两个隐式约定（`f"{path}.json"`、`["reg"]`）
  从此只存在于这一个模块里。
- **旧的两文件存档仍能读**（`fmt=0`；战斗侧取副档 `reg`，整局侧「整个 json 就是配置」也收），
  但 `read()` 会**打一行 ⚠️** 并建议 `migrate()`；`migrate()` 重存后权重**逐位不变**、删旧副档。
- **改名连带**：`REG` → `CONFIG`（`_az_train`）后写侧曾落 `config=`、8 个读侧仍读 `["reg"]`
  （6 处硬 KeyError + 2 处静默退回默认）—— 本次一并修掉。教训：契约靠**字符串字面量复制 9 处**
  ⇒ 改名对类型检查器**全静默**。
- 验证：18 个文件 `py_compile`；真跑 `train data/_ab28.npz 1 0 data/x.pt` ⇒ 单文件、目录里**没有** `.json`；
  `_ckpt.py <档>` 读出 `config` 6 键 / 16 权重键 / 元数据 `[metrics,space,z_spec]`；
  `_diag_head` 走 `_来源=存档配置`；旧档读取、`migrate()`（权重逐位相同、副档消失）、裸权重
  `config={}`（不猜）均如预期；`_chk_all.py` **14/15**（唯一红项仍是既有的真机桥接）、
  `_chk_runtrain.py` **29/0**。

### 📌 2026-09-28（续十七）：训练打点 = TensorBoard（`_tb.py`）+ **故障告警双写**

- **看**：`tensorboard --logdir data/runs`。每次训练一个目录 `data/runs/<权重名或数据名>/seed<k>`。
- **`_tb.py` 是唯一打点入口**：`writer` / `scalars` / `text` / `recipe` / `fault` / `close`。
  曲线：`train/loss`、`train/grad_norm`（逐 batch；**只看不动** —— 只算范数、不裁剪，裁剪=改配方）、
  每 epoch `train/param_absmax`（发散的**连续**征兆）、`train/ce|r2`、`val/ce|r2`、`hparams/*` + Text `配方`。
- **故障告警**：`fault/<标签>` 标量置 1 + text 详情，**同时**追写 `data/runs/faults.jsonl`
  （可 grep，**不依赖 tensorboard 装没装**）。四条闸（步级 loss / epoch 末参数 / 存盘拒收 /
  确定性自检）全接上；`fault()` **自己绝不抛**（否则会盖掉真正那条）。
- **旋钮**：`--tb`（默认开）/ `--tb-dir`；整局训练器 env 系 ⇒ `STS_RUN_TB=0`。
- ⭐ **打点是纯旁路**：加完逐 batch 梯度范数后，`_chk_threads.py` 的权重 sha 与之前**逐位相同**
  （`bc0fddf8188bb688` / `005d84706dfc32e4`）—— 这是“打点没改数值”的最硬证据（权重 sha 一变就说明改了）。
- ⚠️ `tensorboard` 是**可选依赖**（`uv add tensorboard` 已进 `pyproject.toml`/`uv.lock`）：没装只警告
  一次，训练/守卫全不受影响；远端要单独装（见 `.agent/REMOTE.md`）。
- ⚠️ 踩坑：`python ... | head -N` **会在 N 行后 SIGPIPE 杀掉上游**（首次验证时训练被腰斩，
  事件文件只剩 88 字节头部，误判成“没写进去”）。

### 📌 2026-09-28（续十八）：**按怪物遭遇组**采集（`_collect_encs.py`）—— 多怪形态的解法

- **剂量**：`python _collect_encs.py <每组场数> <sims> [out.npz] [--encs all|A,B,...]`；
  缓存 `encdata_n<n>_s<sims>.npz`，比 Boss 缓存多两列 `enc`（每场遭遇 id）/ `encs`（请求名单）。
- **为什么**：`collect` 的遭遇由 `gc.boss` 决定 ⇒ 多怪行只占全仓 **0.08%**（`_chk_enc.py` 在
  `_ab28.npz` 上的读数）；要「分裂的多怪 / 多怪目标 / 精英」只能**指名**打。
- ⭐ **实测 63 组全部可采**（1 场/组 @30 sims，16 workers，26s，1388 行，教师总胜率 54.0%）：
  Act1 弱怪组 100%、Act2-3 精英/Boss 大量 0% ⇒ 正负样本都有；多怪行占比可控
  （AWAKENED_ONE 100%、AUTOMATON 83%、CENTURION_AND_HEALER 81%）。
- ⚠️ **每组一个子进程**：引擎会对个别遭遇原生 abort（实测 `INVALID` 退出码 **3** = MSVC
  `abort()`，且**零输出**，缓冲全丢）——`try/except` 与 Pool 都接不住（后者还会反复重启
  worker 刷爆日志）。子进程自己串行，并行度在遭遇层。
- **指纹**：`scenario_fp(encs=...)` 把组名带进指纹 ⇒ 与 Boss 缓存**必然不同**；
  `encs=None` 路径逐位不变（`f0ea451dcc15382e`）⇒ 旧缓存全部有效。
- **续跑**：分片 `<缓存 stem>_parts/<ENC>.npz` 已存在就复用（目录名带 n_per/sims）。
- 守卫 `_chk_encs.py`：列口径 / 指纹不误认 / 三道闸 / 训 1 epoch / 分片复用 / `--encs INVALID`
  必须响亮失败；engine 档 **15/16**。`_chk_enc.py` 改读 `enc` 列（零重放），旧缓存仍重放。

### 📌 2026-09-28（续十九）：采集**全部独立成文件**（`_collect*.py`）；按怪组采集改成**枚举 × 分布地点**

用户定调（原话）：「为什么要把采集部分塞到train文件里面？」「这些采集脚本应该完全独立！」
⇒ 新增**数据侧公共层 `_collect.py`**，依赖**单向**（它不认识 `_az_train`）：

| 文件 | 内容 |
|---|---|
| `_collect.py` | `TRAIN_SEED0` / `MAX_STEPS` / `MIN_PAR_BATTLES` + 终局读数（`sts_victory`/`engine_score`/`terminal_stats`）+ 采集原语（`collect_battle` / `assemble`）+ 缓存三道闸（`check_cache_obs` / `check_action_space` / `check_scenario_fp`） |
| `_collect_boss.py` | 原 `_az_train.py collect`（遭遇由 `gc.boss` 定，指纹 `scenario_fp()`） |
| `_collect_encs.py` | 按怪组 × 分布地点（见下） |
| `_collect_walk.py` / `_collect_dagger.py` | 只把导入从 `_az_train` 换成 `_collect` |

- `_az_train.py collect` **不再采集**：子命令保留成指路牌（打印 `python _collect_boss.py ...`、
  退 2）；`all`（端到端）按需 `from _collect_boss import collect`。常量归属：`PAR_MIN_BATTLES`(150)
  → **`_collect.MIN_PAR_BATTLES`（唯一来源）**，`TRAIN_SEED0` 随迁；**`EVAL_SEED0` 留在
  `_az_train`**（`--eval-seed0` 可覆盖的**运行旋钮**，且只有评估侧读它 —— 搬走会得到
  「`from import` 拿到旧值」那类静默失效）。
- ⚠️ `_collect_dagger.py` 原来从 `_az_train` 取 `CONFIG` 建学生网 ⇒ 改成从**存档本身**取
  （`_ckpt.config_of`）：蒸馏必须与产数据的那个网同构，「训练脚本的当前旋钮」反而是错的答案。
- 复核：`grep -rn "from _az_train import" *.py` 应只剩训练侧符号（`CONFIG` / `EVAL_SEED0` /
  `train` / `rollout_*` / `battle_split` / `targets` / `z_values` / `MCTS_BATCH` / `AZNet`）；
  `scenario_fp()` 仍 **`f0ea451dcc15382e`**；`_chk_all.py` engine **15/16**（唯一红项 = 已知
  的 `真机桥接`，与本次无关）。

按怪组采集（`_collect_encs.py`）同一波的三条改动：
- **表 = 任务定义**：删掉 `--encs A,B` 字符串开关与 `parse_encs`；表项 = `(sts.MonsterEncounter.<组>, <层号>)`，
  初始状态走 `_boss.make_stage_battle(seed, enc, floor)`。
- **分组依据 = 引擎自己的池子**（`include/constants/MonsterEncounters.h` 的 `MonsterEncounterPool`）：
  一幕**前半场**抽弱怪池、**后半场**抽强怪池，精英全幕同池，Boss 在幕末；63 项**恰好覆盖全部
  63 个有效成员**（`THREE_DARKLINGS` 同时在 Act3 强弱两池，只收弱怪档）。
  ⚠️ 凭印象写的分组曾有两处错：`SPHERIC_GUARDIAN` 是 **Act 2 弱怪**（不是 Act 1 Boss）；
  Act 1 弱怪池只有 4 组（CULTIST/JAW_WORM/TWO_LOUSE/SMALL_SLIMES）—— `GREMLIN_GANG` /
  `BLUE_SLAVER` / `LOOTER` 属**后半场强怪池**（强怪池共 10 组）。
- **缓存列**：`enc`(`int32`) + `enc_floor`(`int32`)，**不再有** `encs` 字符串列；`_collect.assemble`
  按「有层号」**自己**选指纹 `encs_fp()`（= `walk_fp()` ⊕ 排序后的 `(enc@floor)` 去重集）⇒
  与 Boss 缓存**不可能混训**。子进程 CLI 传 int：`--one <id> --floor <层>`（缺 `--floor` 当场
  `raise SystemExit`、rc=1，不静默退回 Act-1-end 规格）。⚠️ 原 `missing_encounters()`（「枚举 − 表」的
  漂移闸）**已于 2026-09-30 删除**（用户：「本来就只能从枚举里面取数据，你还检查在不在枚举里面干嘛」）：
  表项天生是枚举成员（名字写错在 import 期就 `AttributeError`）⇒ 它管不了表；反向差集对采集行为
  **零影响**（只 print、打完照样采）且实测恒为空（64 成员 = 63 项 + `INVALID`）⇒ 判据不挡事。
  代价：引擎将来加新遭遇不再有自动提醒，查覆盖率要手敲
  `python -c "import slaythespire as s, collect_encs as C; print(len(s.MonsterEncounter.__members__), len(C.ENCS))"`（应 64 / 63）。
- ⭐ **实测「按层发初始状态」真的生效**：引擎侧遗物数随层 **1 → 3 → 4 → 5 → 6**
  （act1 / act2前 / act2后 / act3前 / act3后），能量 Act 3 变 4；缓存侧同组怪、同 seed、
  只换层 ⇒ **首行观测不同**。
- 守卫重写 `_chk_encs.py`（engine 档，22s）：3 项 × 2 场 @25 sims、workers=2，验列 / 指纹
  （含「只改层号也得变」）/ 地点进初始状态 / 续跑（分片 mtime 不变）/ 失败响亮（`--one 0`
  原生 abort 非零退出；全失败必须抛错且**不产出**缓存）。
  ⚠️ **2026-10-01：「失败响亮」那两条已删**（用户要求）—— 它靠 `--one 0` 喂 `INVALID` 触发
  引擎 `MonsterGroup.cpp:471` 的原生 abort 来验「子进程失败会被判失败」，而那个 abort 本身
  就是用户要清掉的东西。现覆盖 = 列 / 指纹 / 地点 / 续跑 / `eps`。

- **全怪组采集首跑（09-28 续六，细节见 `notes.md`）**：远端 `encdata_n1000_s500.npz`
  （**5.2GB / 63000 场 / 987767 行 / 教师 65.5% / 指纹 `447bf7c6d6e9c8e1`**，63 项零失败，359s）。
  ⭐ **难度阶梯**（按地点发初始状态）：Act1 弱怪 100% → Act1 Boss 16~28% → Act2 Boss 4.6~11.2%
  → Act3 Boss 1.2~2.5% → 心脏 0%；**多怪行 36.0%**（Boss 缓存只有 0.08%）。分片在
  `data/encdata_n1000_s500_parts/`（同命令重跑 = 续跑）。⚠️ 该实例 `nproc` 报 128
  但 **cgroup 只有 16 核** ⇒ worker 取 16；`STS_WORKERS` 是空转遗留（没人读，用 `--workers`）。

### 📌 2026-09-25 增量（覆盖上面那份 09-23 快照里已过期的数字）

- 战斗快照 `OBS_DIM=1110` / 网络输入 `in_dim=1382`（= `AZNet(...).trunk[0].in_features`，**别手抄**）。
  ⚠️ 本行 2026-09-28 复核改写：此前记的 `1111 / 1385` **已过期**。复核命令：
  `python -c "import slaythespire as s, aznet as A; print(A.OBS_DIM, s.snapshot_layout()['len'], A.AZNet().trunk[0].in_features)"`
  （后两个数必须相等/自洽；`_chk_snap.py` 也在 `_chk_all` engine 档里盯着）。
  布局唯一来源仍是 `sts.snapshot_layout()`（`azlayout.py` 顶部逐键具名，其下零裸取用）。
- **开关现状**：只剩 `arch` / `depth` / `sel_arch` / `pair`。
  已删：`pile_top_k` `pile_pool` `pad_zero` `norm` `res_pool` `use_resources` `gate_select`
  `hand_key`（变必给）`pair=mul`、以及 **`hand_pos`（2026-09-25）** ⇒ 手牌恒**逐槽 concat**。
- **资源块两网分家（2026-09-25）**：战斗网 = 不带 bias 的 `Linear(RELIC_TYPES, relic_rank)`
  （遗物多热列和；初始化显式 `std=aznet.INIT_STD` 对齐旧表，否则 `nn.Linear` 默认大 3.7×）
  + **裸 `nn.Embedding` 逐槽查表**（药水；`clamp(min=0)` 必留，负 id 会让查表抛错）
  ⇒ 战斗网**完全不用** `multi_hot`（09-28 起连常量也不用 —— 三个都搬进 `aznet` 自己，见续九）。
  整局网仍用 `MultiHotEmbed`/`SlotList`/`Block`/`pool_inputs`（那边要**按 id 查行**给候选打分，
  且走**替换式池化** ⇒ 才需要 `consumed`/`out_width`）；`_res_prior.py` 的迁移接口改成
  `relic_linear.weight.T`（[181, rank]、行 = id ⇒ **先验文件格式不变**）。
- **id 空间取值域命名定稿（2026-09-25）**：凡是「这个 id 取几种值」（= 嵌入表行数 / one-hot
  宽度 / 多热列数）**一律 `*_TYPES` 后缀**，`azlayout` 里共 9 条同一族：`ENCODED_CARD_ID_TYPES`
  (742) / `POTION_TYPES` (44) / `RELIC_TYPES` (181) / `MONSTER_ID_TYPES` (66) /
  `MONSTER_MOVE_ID_TYPES` (197) / `INPUT_STATE_TYPES` (32) / `CARD_SELECT_TASK_TYPES` (21) /
  `BATTLE_OUTCOME_TYPES` (3) / `STANCE_TYPES` (4)。旧名 `NUM_CARD_IDS` / `N_POTION_IDS` /
  `RELIC_LEN` 全删（6 个文件 20 处同步 ⇒ 是**纯改名**，数值/宽度/键集均不变）。
  ⚠️ 唯一例外：遗物没有 `relic_types` 键（`RelicId` 没有名字表 ⇒ C++ 那个键同时充当块宽，
  叫 `relic_len`）⇒ Python 名与键名不同形，已在 `azlayout` 里标注。
- **嵌入宽度全展开（2026-09-25）**：`AZNet` 的共享 `embed_dim=8` **删除**，改成**每种实体 id
  各自一个**：`card_rank=8` / `monster_rank=8` / `intent_move_rank=8` / `relic_rank=16` /
  `potion_rank=8`（签名：`hidden, dropout, depth, arch, sel_arch, pair, *五个 rank`）。
  两个头跟着显式指名尺寸：`FactorPolicyHead(hidden, card_rank, monster_rank, policy_dim, …)`
  （`k_card←card_rank`；`k_num→monster_rank`；`k_mon`/`n_mon`/`none_tok`/`discard_tok ← 2×monster_rank`）、
  `FactorSelectHead(hidden, card_rank, n_num)`。`net.cfg` 记这 5 个；`_az_train.REG` 的
  `embed_dim=8` → 三个新 `*_rank=8`（遗物/药水仍走 `MH.*` 默认）；`_diag_head` 的键名推断路径同步。
  ⚠️ 这是**参数面改写、零行为改动**：改动前后 4 种配置的 `state_dict` 键名/形状逐项相同、
  `in_dim` 仍 1385、参数量 225,461 不变。⚠️ `cfg` 的键变了 ⇒ 老 `*.pt.json` 里的 `embed_dim`
  会被 `net_kwargs` 按「未知键」打 ⚠️ 忽略（那些权重本来已废）。
  ⚠️ `FactorPolicyHead` 里那个与配置字符串同名、类型是 bool 的 `pair` 参数**改名 `bilinear`**
  （调用点写 `bilinear=(pair == "bilinear")`）—— 原来那行 `pair=pair == "bilinear"` 像自赋值，是个陷阱。
- **快照键收口：只留 `potion_slots`（2026-09-25）**：C++ 原先同时导出 `potion_slots` 与 `potion_slot_len`
  （同值 5 ⇒ **同一个数的第二份拄本**）⇒ 删掉 `potion_slot_len`，保留 **`potion_slots`**（用户定调：
  「两端语义才对应」—— run 侧也叫 `potion_slots`，而且它与其余逐槽块的 `hand_slots`/`pile_slots`/
  `deck_slots`/`monster_slots`/`card_select_slots` 同族）。C++ 里 `potionSlotLen = potionSlots`
  只当**内部块宽**（写快照的循环 + `snapshotLen`）且**不再导出**；Python 侧块宽就是
  `POTION_SLOTS`（每槽 1 项，与 `hand_slots × hand_fields` 同一套读法）。
  读取点改造 7 处：`azlayout`(POTION_SLOTS) / `_chk_res`(4) / `_chk_difficulty` / `_dump_battle`。
  ⚠️ **不改快照字节**（只是 dict 键）⇒ 缓存/ckpt 不作废；验证：`snapshot_layout()` 现 **46 键**、
  `azlayout` 具名数 = 46（文件头「把每一个键具名一次」重新成立）、OBS_DIM 仍 1111、`_chk_all` 12/13。
  ⚠️ 还差：`realtest/comm_map.py` 的 `_FALLBACK` 与读取处仍写 `potion_slot_len`（它本来就在真机桥接红项里）。
- **C++ `Snap` 按 layout 顺序重排（2026-09-25）**：`bindings/slaythespire.cpp` 的 `struct Snap` 改成
  **一块一节、按快照字节顺序**：player → counts → hand → 三个牌堆 → masterDeck → monster →
  playerAttr → playerCounter → playerStatus → monsterStatus → cardSelect → selectMeta → runCtx →
  relic → potionMeta → potionSlot → `snapshotLen`；每节 = **宽度 + 起始下标**（原来宽度散在三处，
  relic / potion / runCtx 的宽度还夹在偏移链中间）；**不占快照**的「id 空间 / 取值域」整批挪到最末尾。
  `snapshot_layout()` 的 `d[...]` 也按同一顺序排（先 `len`，再逐块宽度→偏移，最后 types）。
  顺手修：① `playerCounterOff` 里那个裸 `+ 3` → 新增 `playerAttrLen = 3`；
  ② 新增两个**导出**键 `player_len` / `player_attr_len` ⇒ 键数 46 → **48**，`azlayout` 全具名，
  `PLAYER_LEN`/`PLAYER_ATTR_LEN` 从「下一块起点 − 本块起点」改成直接读（那两行推导与说明已删）。
  ⚠️ **零行为改动**：本地重编通过；所有偏移 / 总宽（0, 8, 15, 55, 115, 175, 235, 299, 359, 362,
  375, 462, 677, 917, 921, 923, 1104, 1106, 1111）与改前**逐项相同**、OBS_DIM 仍 1111、
  `_chk_all` 12/13。
  📌 读这个文件时注意：终端与 `read_file` 在**显示宽度 > 80 列**处会插入假空格
  （例如把「同一个」显示成「同 一个」）⇒ 要逐字节核对长中文行，得用
  「按显示宽度分块打印 `repr()`」的办法（每块 ≤ 60 列）。
- **`BattleWrapper` 撤掉（2026-09-25）**：那个类 = `{ BattleContext *bc; }` + 14 个方法，而
  `BattleContext.h` 里**一个 `friend` 都没有**（只用 public 成员）、唯一消费者是 `bindings/` ⇒ 零封装价值。
  现在它们是 **`bindings/bindings-util.cpp` 里 `namespace sts::py` 的自由函数**（声明在
  `bindings/slaythespire.h`）：驱动一步 `selectCard`/`endTurn`/`drinkPotion`/`discardPotion`、
  搜索 `scumSearchStep`/`rootPolicy`/`searchStats`（+ `SearchStats` 结构）、动作词表
  `enumerateActions`/`actionToIdx`/`idxToAction`、扁平读 `getHand`/`getDrawPile`/`getDiscardPile`；
  常量 `unifiedActionCount=96` / `endTurnIdx=95` 也在那个头文件里。
  `include/sim/BattleWrapper.h` + `src/sim/BattleWrapper.cpp` **已删**，`bindings/slaythespire.cpp` 里
  那套 `BcForward`（成员指针 → lambda、每次调用造一个临时对象 + `const_cast`）也随之删掉、
  `.def` 直接传函数指针（CMake 用 `GLOB_RECURSE` ⇒ 不用改 CMakeLists）。
  ⚠️ **Python 侧名字一字未动**（`b.get_hand()` / `b.search_stats(sims)` …）；`_chk_all` 仍 12/13、
  重编通过 + 直接烟雾测试过 14 个函数（含 `search_stats` 的 pybind 默认参路径）。
  📌 `drink_potion`/`discard_potion` 喂**空槽**会走到引擎内部的致命错误（进程直接死，不是 Python
  异常）—— 原样搬运的旧行为，调用方只能喂合法动作。
- **数据文件命名收口（2026-09-25）**：`_env.cache_name(kind, n, sims, ver=..., **extra)` /
  `_env.ckpt_name(npz, epochs)` 是**唯一**拼名点，形如 `<kind>_<ver>_n<N>_s<SIMS>[_<k><v>].npz`、
  权重 `<数据stem>_e<E>.pt`（拿到权重能反查数据）。`ver` **必须**传指纹里的版本常量
  （`_collect_walk.WALK_VER="v5"` / `_collect_dagger.DAGGER_VER="v1"`，与 `walk_fp()`/`dagger_fp()`
  的哈希串同源）⇒ 升版本时**文件名自动跟着变**，不会出现「文件叫 v2、指纹写 v1」。
  三个默认产物全部改成派生：`walk_v5_n<N>_s<SIMS>.npz`、训练默认输出 `<数据>_e<E>.pt`
  （**不再是 `az_boss.pt`**，且 `all` 也用 `save_ckpt` 补上了配置副档）；`-` = 用派生名
  （要指定 seed0 就得占住那个位置）。`bossdata_n<n>_s<sims>.npz` **名字不变**（它靠 `scenario_fp` 守卫）。
  ⚠️ 动机：清理时发现 `data/` 里 `walkdata_n800_s4000`（无版本）/`walkdata_v3_*`/`walkdata_v4_*`/
  `_walkv5_n500`（前导下划线、版本在中间）并存 —— **光看名字分不出哪个是现役**。
- **`data/` 清理（2026-09-25）**：远端 33 GB → **66 MB**（169 → 32 个文件）。
  判据**不看名字、看文件内容**（新脚本 `_audit_data.py`：读 `npz` 里的 `obs_dim`/`fp`/`src`、
  比对 `.pt` 的键集与当前 `AZNet`）⇒ 发现**全部战斗侧缓存都是 `obs_dim=1076`**（当前 1111）
  ⇒ 包括 `bossdata_n20000_s8000.npz`（3 GB）、`walkdata_v4_n10000_s4000.npz`（10.4 GB）、
  `dagger_v1/v2.npz`（17.8 GB）、`_walkv5_n500.npz` 在内**全部作废**，已删（不能迁——
  既定策略是「重采而非迁移」）；72 个 `.pt` 也全是死权重（键名变过三次）⇒ 已删。
  **保留**：`res_prior.pt`（先验文件，格式不变）、`run_mapstats_smoke.npz`（`_chk_rundata`/`_chk_runnet` 引用）、
  整局线权重 `run_*.pt`、结果 JSON（含 `A20runs.json` 真机留档）。
  ⚠️ 顺手修掉唯一一处硬写旧缓存：`_probe_capacity.py` 的 `CACHE` 原本写死 v4 文件名
  ⇒ 改成从 `WALK_VER` 派生（`STS_CAP_CACHE` 可覆盖）。
- **重采 + 重训流水线（2026-09-25 起）**：`_collect_train_off.sh`（远端，`_sync.sh start` 拉起）
  = 采集 → 训练 8ep → **主动关机**；任一步 rc≠0 ⇒ 不关机（保持开机看日志）。
  本次跑 `10000 局 @ UCT(4000)` → `data/walk_v5_n10000_s4000.npz` → `..._e8.pt`。
- **`shape_floor`：层数 → 发育状态的唯一入口（2026-09-25）**：
  `_collect_walk.shape_floor(gc, rng_seed=None, variant=None)` 一次写全「牌组/升级 + 遗物/药水
  + hp/金币」（`floor_num` / `act` **归调用方**，见下方 📌），**不经过整局漫游** ⇒ 这就是「直接指定怪物的重点训练」
  的开局接口：
  `shape_floor(gc, 37, seed)` → `b = sts.BattleContext(); b.init(gc, sts.MonsterEncounter.THREE_SENTRIES)`。
  `rng_seed=None` ⇒ 只用层号（同层同状态，定点复现）；给 int ⇒ 同 (层, 种子) 同状态。
  ⚠️ 三个随机流（`sub*7919+13` / `sub*104729+7` / `sub*65537+29`）互不干扰 ⇒ 把
  **三个调用点各抄一遍**的副本（漫游 `_walk_battle` / DAgger `_dagger_battle` / 评测
  `_eval_battle`）合并后**逐位不变**：66 组合逐项相同、`walk_fp()` 仍是 `c3e777e39ad5`。
  `act` **不由本模块管**（`devshape.act_of` / `ACT_EDGES` 已删）—— 漫游路径上它由引擎设好，
  指定层/指定怪物时由**调用方**设；幕分界（act1 战斗只在 floor 1..16、act2 首个战斗层 = 18）
  只作为 `_chk_shape.py` 的**固定装置** `ACT_EDGES_FIXTURE` 保留，用漫游对答案。
  新守卫 `_chk_shape.py`（已登记进 `_chk_all` engine 档，1.8 s）查 7 件事：定点复现 /
  只用层号 / 层数↑⇒平均牌组·遗物**单调**↑ / 基础牌**单调**↓ / 两套形态各 50% /
  **指定怪物直接开局**的端到端（怪数、血量、总牌组、快照长度都对得上）。
- **两套卡组形态 + 删基础牌随层上升（v6，2026-09-25）**：`devshape.VARIANTS = ("big", "lean")`、
  `VARIANT_P = 0.5` ⇒ **出现概率相等**（实测 400 个 sub：big 194 / lean 206）；`variant_of(sub)`
  用**独立**随机流（`sub*31337+3`），一场战斗只抽**一次**，四个调用点必须用同一个形态。
  · **big（大卡组）** = 上游 bot 那套发育、**逐列对齐实测**（60 种子牌组均值）
    12.8/15.7/20.1/25.9/28.2 vs 锚点 12.9/16.4/20.3/23.6/26.7 ✓
  · **lean（小卡组）** = **以精简总牌数为目标**：追加量 ≈ big 的 60% + 删基础牌更狠
    ⇒ 牌组 10.9/12.5/13.9/15.9/17.2（act3 ≈ 17 而不是 30）✓ **数字是设计值、没有实测来源**
    （那份实测只有 bot 的一种风格）⇒ 引用时别写成「实测」。
  · 两套**共用**遗物/升级/hp/金币/药水 ⇒ lean 牌少而升级张数照旧 ⇒ 每张牌升级率更高
    （act3：10/17 ≈ 59% vs big 的 10/28 ≈ 36%）。
  · 删基础牌两套各有表：`REMOVALS = {"big": (0,1,1,2,2,3), "lean": (1,2,3,4,5,6)}`；
    实现只删 `is_starter_strike_or_defend`（真实玩家不删痛击），且删在**加牌之前**（那时牌还没升级）。
  ⭐ **取整方式只有一种（2026-09-25 定调：「全部作为期望」+「以它们为中心的正态取值」）**：
    四张表全是**期望值**，一律走 `devshape.draw(mean, rng)` = **以 mean 为中心的正态**
    （`σ = √mean`、四舍五入、下限 0）；`n_int`/`UPGRADE_JITTER`/`REMOVAL_JITTER` 全部已删。
    σ 不另给常数：`√mean` 就是**计数**的固有涨落（泊松）⇒ 量小窄（0.3 ⇒ σ≈0.55）、量大宽
    （8.8 ⇒ σ≈3.0），表里仍然只有一个数。⚠️ 截断正态 ⇒ 实现均值略高于表值：
    实测 0.3 → 0.373（+24%）、1.7 → 1.755（+3%）、8.8 → 8.80；要严格保均值换泊松抽样。
    ⚠️ 效果：升级/删牌重新有了段内散布，相邻段重叠（不再是硬台阶）。
  📌 **`floor_num` / `act` 归调用方**（2026-09-25 用户定调：「这两个数据不是这个函数应该动的」）：
    `shape_floor(gc, rng_seed=None, variant=None)` **只读** `gc.floor_num`（`rng_seed=None` 时拿它当种子），
    只动牌组/升级/遗物/药水/hp/金币六样；`devshape.act_of` / `ACT_EDGES` **已删** ——
    幕分界现在只是 `_chk_shape.py` 里的**固定装置**（`ACT_EDGES_FIXTURE`，用漫游对答案）。
    ⚠️ 后果：指定怪物/指定层的流程里**调用方要自己设** `gc.floor_num` 与 `gc.act`
    （后者只有 Boss 遗物件数 `min(2, act-1)` 用；新 `GameContext` 默认 act=1 ⇒ 不设就没 Boss 遗物）。
    三个调用点（漫游 `_walk_battle` / DAgger / 评测）已改成 `shape_floor(gc, sub)` ✓。
  ⚠️ **与实测锚点的偏差是有意的**：实测 `BASIC 9.6 → 7.3`（全程只掉 2.3），其中 Boss 遗物的
    拾取效果（Pandora's Box 那类）实测已吃掉 0 → 1.4 ⇒ 严格对齐的话「人删牌」只剩 ~1 张额度。
    现在 big 取到 3 张 ⇒ 终值 BASIC ≈ 4.95（比锚点低 2.4）、lean ≈ 2.6；要回到对齐实测的
    力度把 big 那列改回 `(0, 0, 1, 1, 1, 1)`。
  ⇒ `WALK_VER` **v5 → v6**（fp 串：`var=big/lean(50)`、`thin=stage6(devshape,var)`、`up=stage6(devshape)`）
    ⇒ v5 及以前作废；`walk_fp()` 现为 `7f28b0f4f63e…`。
  `_chk_shape.py` 现在查 7 件事（新增 ⑦ 两套形态各 50% + lean 更精简且差距随层扩大；
  ④⑤ 改成**逐形态**查单调 —— 两套混在一起均值会被拉平）。
- **⭐ 非战斗地图节点全部跳过（2026-09-25 定调：「walk 跳过所有非战斗地图节点，因为我们的
  采集根本不看真实 gc」）**：`_collect_walk.NON_BATTLE_SCREENS` = REWARDS / BOSS_RELIC_REWARDS /
  EVENT_SCREEN / SHOP_ROOM / REST_ROOM / TREASURE_ROOM ⇒ 这六种屏**只**走
  `gc.regain_control()`（= 执行挂着的 `regainControlAction`，**不领任何东西**：不拿卡/金币/
  遗物/药水、不休息、不开箱、不选 Boss 遗物）。
  ⚠️ **不能**像旧代码那样直接 `screen_state = MAP_SCREEN`：那个挂着的 lambda 里带着**推进必需**
  的转换 —— 普通房 = 回地图；Boss 战后奖励 = 进 Boss 宝箱（`enterBossTreasureRoom`）；
  Boss 宝箱 = `transitionToAct(act+1)`。省掉它整局就走不下去（Boss 后本幕地图没有节点 →
  `map_choices` 空 → 提前 break，outcome 永远 UNDECIDED）。
  ⚠️ 旧写法里 `choose_campfire_option(0)`（休息）/ `choose_treasure_room_option(False)` /
  `choose_boss_relic(3)` 内部也就是 `regainControl()` ⇒ 跳过与它们是**同一状态迁移**，只是少
  了治疗/拾取（而那些本来每场开打前都会被 `shape_floor` 覆盖掉）。
  ⚠️ 副作用：`rng` 现在只给地图取巷用（以前奖励翻牌也抽它）⇒ **同一个 seed 的路径变了**
  （「均匀随机走图」这个分布没变，所以 `walk_fp` 不升版、缓存名不变；反正 v6 还没有数据）。
  已同步的文档/注释：`_collect_walk` 模块头 + `run_one_collect`/`_walk_battle` docstring、
  `_eval_walk` 模块头 + `_eval_battle`、`_collect_dagger` 的引擎出口注释。
  验证：`_chk_all` engine **13/14**（唯一红项仍是真机桥接）；5 个 seed 实跑（含 seed 5 走到
  act2/floor19，证明 Boss 奖励→Boss 宝箱→换幕这条链没断）；小采集 2 局端到端落盘
  （obs_dim 1111、`fp` == `walk_fp()`、`src=walk`）。
  ⚠️ 顺带发现（**未改**）：`_walk_battle` 往 `term` 里塞的 `t_floor`/`t_act` 不在
  `_az_train.Z_COLS` 里 ⇒ 落盘时被丢掉（要按层统计得先加这两列）。
- **⭐ 战斗收场规则换成「硬回合上限」（2026-09-25 定调：「这里的锁死太宽了，直接限制最大回合数吧」）**：
  删掉「连续 5 步能量没变 ⇒ 强制结束回合」，改成 `_az_train.MAX_TURNS = 50`（读 `b.turn`；
  超了就 break 出战斗循环、`z` 按「未获胜」记 = -1）。三个同源战斗函数一起改：
  `_collect_walk._walk_battle` / `_collect_dagger._dagger_battle` / `_eval_walk._eval_battle`。
  · **为什么要删**：能量不变**不是**卡住的充分条件 —— 0 费牌连打、喝药水（免费）都不改能量，
    教师正常的手顺会被误判。实测触发率 **4 次 / 894 个动作 / 56 场（≈0.45%）** ⇒ 每触发一次
    就替教师多按一次 `end_turn`，是真实的数据污染（小但确实存在）。
  · **为什么不选更小的值**：实测正常战斗 `t_turn` max = **9**（81 场 @60 sims，中位 3）、
    历史全量 p99 = 15 / max = 30 ⇒ 50 只在死循环/病态局触发。而 `MAX_TURNS` 撞顶的场次
    `t_turn = 51 > TRUNC_TURN(30)` ⇒ `drop_trunc_mask` 天然能筛掉它们（旧口径那些场次
    `t_turn` 冲到 **500**、`turn/60 = 8.3` 是 25× 的离群特征，现在被压成 0.85）。
  · 兜底仍是 `MAX_STEPS = 3000`（回合内的死循环不涨 turn，靠它收）；`_walk.py` 独有自己的
    `MAX_BATTLE_STEPS = 20000` 与旧判据**未动**（它是独立冒烟漫游，且刻意不在导入期拉 torch）。
  · 验证：`_chk_all` engine **13/14**（唯一红项 = 真机桥接）、`_chk_shape` 全通过、小采集 2 局
    端到端落盘正常（88.9% 教师场胜率，样本数 310 → 315 —— 少了几次强制结束回合）。
- **⭐ 撞壁概率实测（300 局 @UCT(4000)，2026-09-25）**：**4038 场 / 59823 决策点里，
  撞 `MAX_STEPS` 的 0 场、撞 `MAX_TURNS` 的 0 场**（观测极值：单场最多 **70 动作**、
  最多 **17 回合**；`MAX_TURNS` 现在是 100）。幕覆盖 act1 2704 / act2 1185 / act3 149。
  0 命中 ⇒ 95% 上界 ≈ 3/4038 = **0.074%/场**。对比：旧口径 boss 缓存曾 5/20000 场撞
  `MAX_STEPS`（那些是"被能量判据救回来后仍卡到 3000 步"的），漫游这套一次都没有。
- **⭐ 本地采集吞吐（干净基准，2026-09-25）**：32 局 @4000 sims / 16 workers = 180.8 s
  ⇒ **5.65 s/局** ⇒ 万局 ≈ **15.7 h**；单局串行 18.3 s ⇒ 16 workers 只有 **~3.2×**
  加速（≈20% 效率，原因未查 —— `STS_ROLLOUT_THREADS` 默认已是 1，不是 torch 线程打架）。
  ⇒ 万局仍放远端（12 核那台历史上快得多）。
- **⭐ 快照删掉 `outcome` 列（2026-09-25 定调：「快照里可能不需要这个」→「删除，别给我写啰里啰唆的注释」）**：
  它是 `BattleContext::outcome`（`Outcome` 三态），写在 player 块第 8 项；但**所有训练帧都是
  `UNDECIDED`**（每条收集路径都在 `while not b.is_battle_over()` 里取快照；实测 93 帧取值集 = {0}），
  而它在 `azlayout.CAT_SPEC` 里是名义类别 ⇒ `aznet` 每帧 one-hot 出一个恒定的 `[1,0,0]`：
  **3 个零信息输入维度**。唯一会非零的地方是 `rlenv.step` 的 `obs_after`（终局帧），
  但 `rlenv.trajectory` **没有任何外部消费者**（`rltrain.py` 也已不在）⇒ 实际是死的。
  改动：C++ `Snap` player 块 8→7 + 删 `s.push_back(bc.outcome)` + 删 `outcomeTypes`/`static_assert`
  + 删 `snapshot_layout()["battle_outcome_types"]` 键；Python 删 `P_OUTCOME`/`BATTLE_OUTCOME_TYPES`、
  `CAT_SPEC` 那行、`_SCALE_SPEC` player 行尾的 `4`、`aznet.cat_feats` 那行 one-hot。
  ⇒ **`OBS_DIM 1111 → 1110`、`in_dim 1385 → 1382`**（参数量 225461 → 225077 = −3×128）；
  旧缓存/旧 ckpt 作废（本来就都无效）。
  ⚠️ 顺手修了 3 个**手抄快照下标**（删列后当场错位暴露）：`_chk_shape.py` 的 `snap[14]`
  （deckSize → `counts_off + 6`）、`_chk_snap.py` 的 `snap2[8]/[9]`、`_chk_actions.py` 的 `[8]`
  （→ `snapshot_layout()["counts_off"]`）。以后**一律派生**，别写数字。
  `_chk_all` 回到 **13/14**（真机桥接照样红；那份 `realtest/comm_map.py` 旧副本现在更旧了）。

- ⚠️ 本轮 `state_dict` 键名变过三次（`cat_embed.*` → `emb_monster_id/emb_intent_move_id`、
  删 `policy_head.k_id.*`、`relic_state.table.*` → `relic_linear.weight`）
  ⇒ **09-25 之前的战斗网 ckpt 全部作废，必须重训**；**`.npz` 缓存不受影响**（`OBS_DIM` 未变）。

### 📌 2026-09-23 状态快照（**动手前先核现场**，细节见 `notes.md` 续九~十三）

- 战斗快照 `OBS_DIM=1002`（`potion_types=44`、末尾 5 列逐槽药水、`potion_slot_off=997`）；整局 `RUN_DIM=1039`
- `AZNet.in_dim=1288`（`hand_pos=slot`）/ 1189（`pool`）；`arch` 的 flat / factor **两条都活**
- **开关现状**：只剩 `arch` / `depth` / `sel_arch` / `pair` / `hand_pos`
  - 已删：`pile_top_k` `pile_pool` `pad_zero` `norm` `res_pool` `use_resources`（⇒资源恒可见）
    `gate_select`（⇒选牌区置零无条件）；`hand_key` 变**必给** `full`（⇒ `k_hand_num` 恒存在）；
    `pair` 只剩 `none | bilinear`（`mul` 已删、传它会报错）
  - ⚠️ **09-22 之前的权重全部 load 不了**；**缓存不受影响**（`n6_n20000_s500.npz` 有效）
- **运行通道（09-23 起）**：长任务一律 `./_sync.sh start …`（远端 `tmux`，会话名 `STS_SESSION`，
  默认 `sts`）；`./_sync.sh attach | kill` 接入 / 终止。守望 = 本地异步循环、**每轮新开 ssh**、
  读到 `COMMAND_EXIT_CODE` 收工、**`break` 收尾（别 `exit`）**；⚠️ 掉线重连**不影响完成回调**；
  退出码有第二证据（tmux `#{pane_dead_status}`）；台账记在 `_sync.sh.sessions`
- 删开关的判据分三类（**别用 A/B 数字推翻第二类**）：①被数据否掉 ②按**能力**定 ③口径已定
- 两个新抓到的「静默撒谎」坑：`collect()` 会**静默复用同名缓存**；`_sync.sh status` 曾只报
  最新那个日志（并行作业漏臂）+ 无退出码即报「仍在运行」—— 都已修

# ③ 已验证结论（跑出来的数字与结论）

### 已验证结论（别重复踩）
- ✅ **牌堆编码 = 纯集合**（`PILE_TOP_K=0` / `PILE_POOL=mean` / `PILE_SKIP_EMPTY=True`）：
  结论来自**部署口径**，**不是**消融 —— 只有抽牌堆顶有意义且默认不可见（除 `Frozen Eye`），
  弃牌堆顺序注定被洗牌销毁，消耗堆没有按序取牌的机制 ⇒ 顺序只是**出牌历史的指纹**（过拟合面）。
  代价可枚举：`Havoc` / `Mayhem` / `Warcry` / `Headbutt` 失去「看堆顶」的能力。
  ⚠️ 但**别再引用「k=0 最好」的消融结论**：千场级重测里 6 个配置全落在 27.8~29.8%
  （单配置 3 种子 std 就有 0.4~2.3pp ⇒ 彼此分辨不出），旧 4pp「害处」是 200 场小数据的产物。
  （旧数据仅历史：k=0 11.8% / k=4 7.8% / k=30 8.5%；池化 mean>sum，k=0 时 11.8 vs 8.7）
- **别给卡牌 embedding 加 `padding_idx=0`**：train_ce 0.685→0.022、val_ce 0.96→1.89
  （可学习的「空槽」嵌入 e0 在当正则项）。开关 `AZNet(pad_zero=)`，默认 False
- ❌ **「网络/教师胜率比恒为 25%」已被推翻**（那曾是两个状态空间的巧合）。
  换到更简单的卡组 + 新编译器后是 **44.4%**。比值**随卡组变化**，
  只在同一卡组内可比，**不要再当普适常数引用**
- `NNInterface::getObservationMaximums()` 是 412 维**跑图**观测，语义/长度都对不上战斗快照，
  归一化要用 `aznet._SCALE_SPEC` 按块定义
- 🎯 **MCTS 搜索增益阶梯（@8000 标签，3 种子 × 200 局，同运行内配对）：**
  **裸 28.2% → MCTS(20) 34.5% → MCTS(100) 36.8% → MCTS(500) 42.7%**
  **3 种子 × 4 档全部严格单调（9/9）**，增量 +6.3/+2.3/+5.9 **不衰减**（100→500 p≈0.04）
  占教师(64.5%)比：43.7% → 53.5% → 57.1% → **66.2%**
  ⭐ **MCTS(500) 完全不需要教师**（只用网络先验 + value + 树搜索）却比裸网络强 +14.5pp
  ⇒ 有了不依赖 C++ 引擎的更强标签生成器，闭环的燃料问题解决了
  成本：MCTS(500)+batch=4 ≈ 2 s/局；2000 局自对弈 ≈ 65 min
- 🔁 **闭环第一次迭代 = 成立**（2026-09-13，**单变量**对照：N1 的教师段与 N0 训练数据逐位相同，
  已用 `np.array_equal` 验过前 34700 行 obs+pi81）：
  N0 `bossdata_n1000_s8000.npz` 34700 样本 → 裸 **29.0** / 20:**37.0** / 100:**40.5** / 500:**44.0**
  N1 `it1_n1000_s200_mix.npz` 77087 样本（同一批 34700 行教师 + 42387 行自对弈@MCTS(200) b=4）
    → 裸 **31.0** / 20:**36.5** / 100:**43.5** / 500:**59.0**（Δ +2.0 / −0.5 / +3.0 / **+15.0pp**）
  - MCTS(500) 的 +15.0pp（n=200 → 3σ）**可下结论**；裸网络 +2.0pp 与噪声不可分
  - **收益吃在 value 头**：裸网络完全不用 value，裸不动 + 搜索大涨 ⇒ 策略头几乎没吸收，瓶颈在策略头
  - 形状变了：N0 的 100→500 只 +3.5pp（近饱和），N1 是 **+15.5pp** → 增益随预算放大 = value 变好的特征
  - N1+MCTS(500) 的 59.0% **超过教师 500 sims 的 47.0%**（同表头哨兵值）；N0 的 44.0% 还不如教师
- 📐 **π 平坦度实测**（熵/logN，1.000=均匀；决定软目标敢不敢用）：
  教师 UCT 8000 sims = **0.951**（峰值 0.336，合法数 4.4）；自对弈 PUCT 200 sims = **0.751**（峰值 0.528）
  ⇒ 「软化有害」**只对教师成立**（旧数 94.0% ≫ 67.8% 是**换卡组前**在精英场景测的，别引用）；
  自对弈 π 明显更尖、有真实排序 → 于是专门去试了软目标（**结论是否定的**，见下）
  ⇒ 已加 `STS_SOFT_TARGET=1`（`soft_target()`，**默认 off，历史结果逐位不变**）
  ⚠️ 混淆：掺教师数据的缓存两种行都有，开开关等于**同时**改坏教师那半
  ❌ **实测结果：软目标更差**（同数据同 seed 单变量）：裸 **31.0→18.5%**（崩）、
    20: 36.5→34.0、100: 43.5→46.0、500: **59.0→53.5%**；val_R2 不变（+0.201→+0.204）
    → 损伤**精确落在策略头**（裸网络=策略头 argmax），value 头毫发无损
    → 机制：`val_ce=1.000` = 网络忠实学会了目标分布的熵；教师 π(熵/logN 0.951≈均匀)
      那半在教它"对什么都不确定"，而 argmax 最怕这个
  ⇒ **"软化有害"第三次复现（教师 2 次 + 这里 1 次），结案**。
    策略头的瓶颈是**容量/数据量**，不是目标信息量。
- 🎯🎯🎯 **`factor` 架构曾把裸网络推动（2026-09-20）** —— ⚠️ **细节已删**（旧布局 1111 / 旧卡组的 A-B，绝对数字已作废）：当时结论 = `factor` 明显优于 `flat`；现默认仍是 `flat`，要重挑架构必须在当前协议上重测。原记录：
  因子化策略头（`logit(手牌槽 i, 目标 t)` = 广播和 → 小 MLP，215,218 参数）替换扁平 `Linear(128,96)`。
  **单变量配对**：同 20k 缓存、同 8 epoch、同 `STS_SEED0` 默认值 0（⇒ 同数据同初始化）、
  同评估 n=200 / seeds 20000+，**只有 `STS_ARCH` 不同**。

  | 架构 | 裸 | MCTS(20) | MCTS(100) | MCTS(500) | val_ce |
  |---|---|---|---|---|---|
  | `flat`（`zl_old_s0`） | 35.5 | 49.5 | 52.0 | 63.5 | 0.742 |
  | **`factor`** | **42.0** | **51.5** | **55.5** | **69.5** | 0.714 |
  | Δ | **+6.5** | +2.0 | +3.5 | **+6.0** | −0.028 |

  ⭐ **这是本项目第一次动到裸网络（= 纯策略头 argmax、完全不用 value）**。
  此前所有杠杆全死：数据量（5k 后饱和 29.4/33.1/32.0）、训练步数（+0.1pp, p=0.909）、
  软目标（−12.5pp）、容量（`ab_cap` 加宽 trunk 只 +0.5pp）⇒ 唯一有效的是**头的形式**。
  - 🔁 **训练量再确认一次（2026-09-21，`run.ab_e32_s{0,1,2}.log`，8ep → 32ep = 4× 步数）**：
    `train ce 0.710 → 0.694`、`val ce 0.716~0.723 → 0.701~0.704`（**train 与 val 同时改善**，
    不是过拟合），而胜率 **39.8/51.0/57.8/67.3 → 39.8/49.7/57.3/66.3**
    （Δ = 0.0/−1.3/−0.5/−1.0pp；逐 seed 为 +1.1/−3.0/−4.0 等，**无 3/3 方向**）。
    ⇒ 又一次「**拟合更好 ≠ 打得更好**」：**CE 目标已饱和，训练量不是杠杆**。
    ⚠️ 这是 `experiment-pitfalls.md`「不同 epoch = 不同协议」的**例外用法**：这里**故意**只改 epoch、
    其余全同（同缓存/同 `STS_SEED0`/同评估种子段），问的正是"训练量本身是不是杠杆"。
  ✅ **3 seeds 复现已完成（`run.abf3.log`），结论成立**：

  | 臂（同 20k/8ep/`STS_SEED0`=0/seeds 20000+） | 裸 | MCTS(20) | MCTS(100) | MCTS(500) |
  |---|---|---|---|---|
  | `flat` h128/d2（基线，3 seeds） | 33.8±1.2 | 46.7±2.2 | 53.5±1.1 | 64.8±0.9 |
  | **`factor` h128/d2（3 seeds）** | **39.8±2.1** | **51.0±0.4** | **57.8±1.7** | **67.3±1.5** |
  | `flat` **h256/d3/layer**（1 seed） | 36.0 | 48.0 | 54.0 | 66.5 |

  - **逐 seed 配对（同 seed 0/1/2 ⇒ 同数据同初始化）**：裸 +6.5/+3.5/+8.0 = **3/3**，均值 **+6.0pp**
    （配对 t=4.5, p≈0.045）；MCTS(100) +3.5/+5.5/+4.0 = **3/3**，+4.3pp（t=7.4, p≈0.018）；
    MCTS(20) 3/3（+4.3pp, p≈0.10）。MCTS(500) 只比得了均值（+2.5pp）—— 阶梯没留逐 seed 的 500 档。
  - 🎯🎯 **2×2 给出机制：瓶颈是策略头的「形式」，不是容量。**
    496,645 参数的宽 trunk（h256/d3/layer）只把裸网络推到 **36.0%**（vs 基线 35.5 = **+0.5pp**，同一 seed 0），
    而**同一个 trunk 下**只多 13k 参数的 `factor` 头给 **42.0%（+6.5pp）**。
    ⇒ **瓶颈是策略头的「形式」**：不是容量、也不是可学习能力，具体是**没共享打分函数**（见下）。
    ⚠️ `ab_cap` 只有 1 seed，且同时动了容量**和** `norm=layer` ⇒ 严格讲是「更大 trunk + LayerNorm
    都不值钱」，不是纯容量结论。
  - 🚨 **机制断言被推翻（`_diag_head.py`，`run.diag.log`）**：我原话是「扁平头算不出 牌 × 目标 交互，
    因子化修好了它」。实测 seed 0 留出集 138,976 行（与教师硬目标的一致率 / 交叉熵）：

    | 子集 | 行数 | `flat` | `factor` | `cap` |
    |---|---|---|---|---|
    | 所有普通行 | 135,428 | 68.12% / 0.730 | **69.88% / 0.701** | 68.87% / 0.717 |
    | **打牌·无目标** | 43,109 | 49.71% / 1.059 | **56.16% / 0.983** | 53.70% / 1.031 |
    | 打牌·指定怪 | 56,546 | **70.01%** / 0.769 | 69.28% / 0.761 | 68.69% / 0.769 |
    | END_TURN | 35,773 | 87.32% / 0.270 | 87.36% / 0.269 | 87.42% / 0.257 |
    | SELECT 行 | 3,548 | 39.21% / 1.213 | 38.64% / 1.215 | 40.05% / 1.211 |
    | 药水段 | **0** | — | — | — |

    ⇒ 增益**全在「打牌·无目标」**（+3.99pp），「指定怪」**反而 −1.31pp**。
    **真机制 = 把手牌槽的打分函数共享了**：扁平头是 10 行独立权重（每槽一行）各自学「这个槽里的牌
    好不好」；因子化后同一份打分函数 ⇒「一张牌值不值得打」**跨槽位泛化**。
    指定怪本来就有 70%（Act 1 Boss 只有 1 只怪 ⇒「打谁」根本不是难点）。
  - 🎯 **`select_head` 基本是废的**：39.21% 一致率，占决策点 **2.55%**（3,548/138,976）。
    而因子化赢的机制**恰好就是「跨候选槽共享打分」**，`select_head` 现在正是扁平
    `Linear(hidden, 60)` = 60 槽 60 行独立权重 ⇒ **同一个病，而且有刚验证过的解法**。
  - ⚠️ **一致率指标不追踪胜率**（第 7 次「拟合好 ≠ 打得好」）：`cap` 的分布
    （无目标 +3.99 / 指定怪 −1.32）与 `factor` 几乎一模一样，裸胜率却差 6pp（36.0 vs 42.0）。
    ⇒ 只能当「两个头在哪里不同」的显微镜，**不能预测强弱**。
  - ⚠️ 未解释：一致率 +1.76pp → 胜率 +6.0pp，**3.4 倍放大**。猜想是「槽位共享买的是泛化，
    而裸网络自我驱动会偏离教师轨迹（= 分布外），本指标在分布内测所以看不见」。
    3 seeds 的配对结果本身站得住（裸 p≈0.045 / MCTS(100) p≈0.018），只是机制按上面修正。
  - ❌ **`STS_HAND_KEY=full` 被诊断筛掉，不必烧 3 个 seed（`run.ab_hk.log` / `run.log`）**：
    手牌 key 里再加**该槽自己的数值**（费用/特殊位/可打）—— 假设是「无目标出牌本质是比较
    哪张牌值得打，而只给卡牌嵌入就看不到费用」。实测**方向对但太小**：

    | 子集 | `flat` | `factor`(min) | `factor`(full) |
    |---|---|---|---|
    | 所有普通行 | 68.12% | 69.88% | 69.74%（**−0.14**） |
    | 打牌·无目标 | 49.71% | 56.16% | **57.11%（+0.95）** |
    | 打牌·指定怪 | 70.01% | 69.28% | 68.54%（−0.74） |
    | END_TURN | 87.32% | 87.36% | 86.86%（−0.50） |

    ⇒ 目标子集只涨 0.95pp（而 factor 本身在那涨了 6.45pp）⇒ **那 6.45pp 来自槽位共享，
    不是缺费用信息**；其他子集反而退化，净 −0.14pp。胜率 37.0 vs 42.0 又落在 factor 家族
    的种子散布内（42.0/37.0/40.5, σ=2.1）⇒ 两个各自弱的信号同向，砍掉。
  - ✅✅ **机制在第二个头上复现了（`sel_arch=factor`，`run.diag3.log`）** —— 这是要找的
    独立确认。SELECT 行（3,548 行）：`flat` 39.21% → `factor`(sel 仍 flat) 38.64%
    → **`sel_arch=factor` 42.90%**（+4.26pp，SE≈1.2pp ⇒ **~3.7σ**），CE 1.215 → **1.186**。
    ⇒ **「共享一份打分函数」在 60 个候选槽上同样胜过 60 行独立权重**，与 10 个手牌槽一致。
  - ⚠️ **但它的胜率四档全负，且原因不是它自己**：SELECT 只占决策点 2.55%，机制再好也推不动
    整体胜率 −9pp（−3.0/−3.5/−1.0/−9.0）⇒ 那部分是种子噪声。
    **不过**它**不是**局限在 2.55% 里 —— select 头与策略头**共用 trunk**，梯度会流回去。
    证据：`sel_factor` 把策略头也推动了（无目标 56.16→57.07、指定怪 69.28→68.28），
    **val_ce 反而变差 0.714→0.717**，尽管 select 头自己更好（CE 1.215→1.186）。
    ⇒ 合理解读：**一个更大的头只吃 2.55% 的数据，会给共享 trunk 注入更噪的梯度**。
    净效果不正面 ⇒ **不上 3 seeds，砍掉**（机制问题已回答，收益问题不值得花算力）。
  - ❌ **`STS_PAIR=mul` 同样砍掉（`run.ab_pr.log`，1 seed）**：乘法项
    `score += m_card(手牌) * n_mon(目标)`。胜率 **3/4 档 −5pp**（裸 37.0 / 20:46.5 /
    100:55.5 / 500:64.5），**无一个正信号** —— 而单 seed + n=200 的分辨率本身就是 ±5pp。
    诊断（`_diag_head.py`）给出机制：**指定怪 +2.33pp，但「目标数≥2」只 +0.61pp（SE≈1.4pp ⇒ 噪声内）**，
    而**打牌·无目标 −4.09pp** ⇒ 它涨在「只有一只怪」那侧（那里**本来就没得选**），
    代价是打坏了无目标那支（乘法项也作用在 `none_tok` 上）。净 **−0.28pp**。
    ⇒ **它本该解决的「多怪对比」几乎没动**（多怪只占 0.35% 的行）。
    ⚠️ 它的 `val_ce` **0.719** 反而比 `factor` 的 0.714 差，而值头 RMSE 却**全面略好**
      （0.492/0.440/0.375/0.339 vs 0.497/0.449/0.382/0.344）⇒ 又一次「拟合更好 ≠ 打得更好」。
      注意**值头根本没改**（`pair` 只动策略头）⇒ RMSE 是通过共享 trunk 的梯度变的，
      **它比 `val_ce` 更容易被策略头改动带着走**，别单看它。
  - 📊 **三个 head 变体全部砍掉，`factor` 是唯一赢家**：`hand_key=full` / `sel_arch=factor` /
    `pair=mul` 的目标子集各有小幅改善（+0.95 / +4.26 / +2.33pp），**胜率均无正信号**。
    ⇒ 「在它该起作用的地方起作用」是**必要不充分**（`cap` 已证）；反过来这轮也成立 ——
      **局部改善 ≠ 整体改善**，三者都是局部涨、整体不动或更差。
  - ❌ **`STS_HAND_POS=pool`（手牌池化）砍掉（`run.ab_hp.log`，1 seed）** —— 这是我提的
    「无位置手牌编码」的实测。动机来自 `_probe_hand.py` 的量测：trunk 输出**确实**对手牌
    槽位敏感（`h` 的 |Δ|/std：`flat` **37.8%**、`factor` **19.3%**），而且 `factor` 的决策
    等变率只有 **85%**（`flat` 只有 31.6%）⇒ 那 110 维位置**看着像**伪相关面。
    **实测否掉**：裸 **42.0 → 39.0**、MCTS(100) 55.5 → 54.0、MCTS(500) **69.5 → 65.5**、
    MCTS(20) 持平（3/4 档负，都在单 seed ±5pp 内、无正信号）。
    🚨🚨 **结论已撤回 —— 这个实验混淆了两个变量**：① 位置信息（顺序） ② **信息容量**。
      `pool` 把 trunk 的手牌输入从 `10×8` 压成 `1×8`（80 维 → 8 维），**同时**抹掉顺序
      **和**丢掉表达力 ⇒ **「更差」不能归因于「位置有用」**，也可能只是容量不够。
      ⇒ **「位置有真语义」这句话我说早了。**
    🚨 另两处草率：**没有「置换无关字段」的对照**（「trunk 敏感 19~38%」没有基线 ——
      网络对任何一维都敏感，非零不等于「在用位置」）；**单 seed 不足以判砍掉**
      （seed 0 的配对差 −3.0pp，而单臂 n=200 的评估噪声就有 ±3.5pp）。
    ✅ **等容量对照**已完成**（`STS_HAND_SHUFFLE=1`；`run.ab_hs.log` 8 轮 / `run.ab_hs16.log` 16 轮）**：
      保持 `10×8` 逐槽表示**不变**，只在训练时随机置换槽位（连带置换动作下标前 60 维与掩码）。
      - 探针确认操作生效：等变率 **85% → 95.1% / 96.1%**，trunk 对手牌槽位的敏感度**减半**
        （0.0150 → 0.0053 / 0.0067）。
      - 同分布（3 seeds）：裸 **−1.67**（2/3）、MCTS(20) **−4.33**（3/3）、MCTS(100) 0.00、
        MCTS(500) **−3.33**（3/3）⇒ **只有代价，没有收益**。
      - 跨分布（`_gen_eval.py pair`，A=`abf3` / B=`ab_hs`）：**+3.0 / −10.0 / −14.0**
        （2/3 显著）⇒ 我提的「位置是记忆捷径」假设**被否掉**。
      - 但 **train ce 与 val ce 同幅变差**（两者都 +0.006）⇒ 不是泛化差异，是**欠训**混淆。
        16 轮对照：train ce **0.707**（优于 8 轮的 0.718），同分布裸 +2.0 / MCTS(500) +1.0，
        20 与 100 档仍 −3.0 / −4.5；跨分布 44.5% → 39.0%（−5.5pp, p=0.071，差值缩了 45%）
        ⇒ **大部分是欠训**。
      - 🎯 **最终裁决：不动 `slot`**（只有代价；那点「收益」已被欠训解释掉）。
    ✅ **站得住的只有**：实现确实生效（`_probe_hand.py` 对 `ab_hp_s0.pt` 残差 **0.0000**、
      等变率 **100.0%**，`slot` 版 85.0%）；以及「拟合更好 ≠ 打得更好」——`pool` 的**原始**
      值头 RMSE **0.4192 优于 `slot` 的 0.4267**、参数少 12,672，胜率却更低（与容量混淆无关）。
    ⚠️ 改动本身（`in_dim` 1230 → 1131）不带回归：`slot` 模式下 `factor` 参数量不变，
    本地用随机权重验证过：`pool` 的置换残差 **5.96e-08** = float32 精度，`slot` 是 2.7e-01）。
  - 🎯 **「指定怪」那个 −1.31pp 的结构比我说的更有意思**（同一次诊断）：

    | 子集 | 行数 | `flat` | `factor` | 差 |
    |---|---|---|---|---|
    | 指定怪·**目标数=1** | 54,086 | **70.60%** | 69.76% | **−0.84** |
    | 指定怪·**目标数≥2** | 2,460 | 56.99% | **58.78%** | **+1.79** |

    ⇒ `factor` 在**真的有得选**（2 只以上）时**是更好的**，退化全在「只有一只怪」那一侧。
    我先前说「−1.31pp 几乎全在单目标 ⇒ 跟选谁无关」**算术上对，但漏了结构**。
    ⚠️ 2,460 行 ⇒ SE≈1.4pp ⇒ +1.79pp 只有 **~1.3σ**，是线索不是结论（别再犯过度外推）。
  - 📌 **`run` vs `bg` 的第三个坑（本地杀终端 = 杀远端作业）**：`run` 走 `ssh -tt`（pty），
    本地 kill 掉那个终端会把 **SIGINT 顺着 pty 传过去**，远端作业以 **exit 130
    (KeyboardInterrupt)** 死掉（实测一次，白跑 2 分钟）。⇒ 不盯着终端时**一律 `bg`**。
    另：**heredoc 走 ssh 会被工具层搅乱**（实测 `PYEOFrint(...)` 这种拼接）——
    改用 `create_file` 写本地脚本再 `ssh ... 'python -' < script.py`。
  - 📌 **方法学收获（省了 3 小时）**：**单 seed + n=200 分辨率只有 ~±5pp**（因子家族 σ=2.1，
    n=200 的 SE≈3.5）⇒ 1-seed 筛选**只能**当粗筛，真正该先跑的是**零成本的分布内诊断**。
    诊断不能预测胜负（上面 `cap` vs `factor` 已证），但能回答「这个改动在**它该起作用的地方**
    起作用了吗」—— 若连目标子集都没动，就没必要花算力。
  - 🔁 又一次「**拟合更好 ≠ 打得更好**」（第 6 次）：`ab_cap` 的 val_ce **0.730** 优于基线 0.742
    （接近 `factor` 的 0.714），裸胜率却只 +0.5pp。
  - 搜索增益没被吃掉：裸→MCTS(500) 基线 +31.0pp、`factor` +27.5pp ⇒ `factor` **起点更高且全程更高**，
    不是「变得更好搜」。
  - ⚠️🚨 **arch 不记录在权重文件里**（26 个纯张量、无元数据）。用 `AZNet()`（默认 flat）去 load
    `ab_factor_s0.pt` 会**响亮报错**（不会静默给错数字），但重评**必须**带 `STS_ARCH=factor`
    —— `_gen_eval.py` 的 `AZNet(**REG)` 正是从 `STS_ARCH` 读的。
  - ✅ `data/abf3_s0.pt` 与 `data/ab_factor_s0.pt` **26 个张量逐位相同**（文件大小差 608 字节
    只是 pickle 布局）⇒ 两次独立提交的 run 可复现。
- 🎯 **多怪状态为什么少：按遭遇战归因（`_chk_enc.py`，2026-09-20，**全量 20000 场**）**：

  | Boss | 场次 | 占比 | 教师胜率 | 决策点/场 | 出现过怪的场 | 多怪行/该Boss行 | 占全部多怪行 |
  |---|---|---|---|---|---|---|---|
  | HEXAGHOST | 6683 | 33.4% | 56.8% | 36.9 | 0.0% | 0.00% | 0.0% |
  | THE_GUARDIAN | 6672 | 33.4% | 62.6% | 37.3 | 0.0% | 0.00% | 0.0% |
  | SLIME_BOSS | 6645 | 33.2% | **62.8%** | 30.3 | **57.2%** | **11.56%** | **100%** |
  | 合计 | 20000 | 100% | 60.7% | 34.8 | 19.0% | 3.34% | 100% |

  - ❌ **「史莱姆王胜率低 ⇒ 分裂看不到」被否掉，方向相反**：它 **62.8% 是三个里最容易的**
    （Hexaghost 56.8% 最难）；三个 Boss 出现次数**完全均等**（33.2~33.4%）。
  - ⭐ 真因是第三种：**分裂确实发生了，但很快打完**。**57.2% 的史莱姆王场次都会分裂**
    （多数场合！），而多怪行只占该 Boss 的 **11.56%** —— 分裂出的两只大史莱姆血量少，几回合就没。
    另：它决策点/场只有 30.3（另两个 36.9/37.3），因为**本体只有 140 血**（240/250）。
  - ⇒ **本场景（Act 1 Boss）天生 ~97% 是单怪**。要练多怪得换**场景**，不是换**架构**。
  - ⚠️ **对 `pair=mul` 臂的预期要下调**：多怪只占 **0.35%** 的行（2,460/696,945），
    修好了也动不了整体。那个臂的希望在于「乘法项给**单怪**情形也多加了一组逐卡特征」，
    不在于多怪 —— 这点我差点又搞错。
  - 工具：`_chk_enc.py <缓存.npz> [n_probe]`（并行；教师胜率直读 `t_win`，零重放成本）。
    ⚠️ 必须在**产出缓存的那台机器**上跑（`gc.boss` 由 C++ RNG 决定，跨编译器不保证一致）。

- ✅ **回合数确实在状态空间里（`_chk_turn.py`，三重验证）**：
  - **源码**：`bindings/slaythespire.cpp:168-176` 玩家块 push 顺序 =
    `[curHp, maxHp, block, energy, **turn**, inputState, cardSelectTask, outcome]`，
    `playerOff = 0` ⇒ **下标 4**；`_SCALE_SPEC` 给这一位的除数是 **60**。
  - **数据交叉验证**（全量 20000 场）：每场 `max(obs[:,4])` 与**引擎独立算出**的 `t_turn`
    （`terminal_stats` 走的另一条路）比 —— 差 0 占 **97.5%**、差 −1 占 2.5%（最后一次决策在
    第 t_turn−1 回合、之后终局，如玩家在怪物回合被打死）、**100% 满足 ≤**。
  - ⇒ 下标推导正确；而且值头**已经重度在用它**（`_probe_turn.py`：只改这一位，输出动 0.393
    = 76% 个标准差）。详见上面「值头缺回合信息被否掉」那条。
- ⚠️ **顺手挖出的数据小污点：4/20000 场卡到步数上限**（`_az_train.MAX_STEPS = 3000`）：
  它们的最大回合 min 46 / 中位 **280** / max **500**（= 3000 步 ÷ 每回合约 6 个动作），
  `t_win` 1 胜 3 负 —— **却贡献 3798 行 = 全部行的 0.545%**（单场约 950 行）。
  两个后果：① 标签是「步数上限」的产物，不是真实胜负；② `turn/60` 变成 **8.3**。
  ⚠️ 我跟着推的「正常回合 ≤ 0.37 ⇒ 25 倍离群特征 ⇒ 该改缩放」**是错的**，见下一条。
  ⇒ 修法：在 `_collect_battle` 里跳过「触到步数上限」的场次（要重采缓存），
  或训练时按行筛掉（便宜，但**改变训练集 ⇒ 必须留开关 + 默认关闭**）。
  ⚠️ **目前全仓没有任何地方统计这个** ⇒ 属于「沉默的污染」，和场景池那两类同类。
- ✅ **「`turn/60` 是 25× 离群特征」这个诊断是错的 —— 缩放表的锅已被洗清**（`_chk_scale.py`，
  2026-09-20）。体检 333 个「真正进线性层、又被 `SCALE` 除过」的字段（`SCALAR_IDX` 口径）：
  **去掉污染行后，0 个量程不够**。turn 正常值 p50=4 / p99=12 / **max=30** ⇒ 缩 max = 0.50
  （除数是 60，用了 **50% 量程**）⇒ 尺度完全正常。那些「8.3」**全部**来自 0.478% 的污染行。
  - ⇒ 结论：**该修的是污染，不是缩放**。若按旧诊断去改 `turn/60`，改的是一个**没病的字段**。
  - 🔑 判据教训：量程判据**只对 `SCALAR_IDX` 成立** —— 那才是被 `SCALE` 除过又进线性层的列。
    走 embedding/one-hot 的列（`CARD_ID_IDX` / `ACTIVE_CAT_IDX`）**根本不进标量段**
    （`AZNet._inputs` 里 `index_select(obs, 1, self._idx_scalar)`）⇒ 拿它们报警是误报。
    脚本第一版踩了**两次**：① 报 `cardId`「除数为 1 ⇒ 缩放后还是 700」（那列的 1 压根没被执行）；
    ② 把污染行算进了主表。**判据要只覆盖真正被执行到的路径。**
  - 📊 污染(0.478% 的行)让 3 个字段的缩 max 出格：`block` 37.0→2.38、`turn` 8.3→0.50、
    `exhaustSize` 17.2→1.37。⚠️ 别小看这 0.478%：离群 40 倍 ⇒ **平方后在梯度里权重相当**。
- 📐 **「回合惩罚」的量级被数据钉住（`_cal_turn.py`，2026-09-20）**：起因是想把「长局=差」
  做成奖励项。判据**不是拍数字**，而是：`z` 已含**终局血量比例** ⇒ **每回合挨的打已经
  收过钱了** ⇒ 回合惩罚只能是**平局裁决** ⇒ **在任何现实回合跨度上累计必须 < 1 HP**。
  - 实测回合分布（20000 场，**按场**）：mean **8.6**、p5=6、p50=8、p95=12、p99=15
    ⇒ **跨度（p95−p5）只有 6 回合**；1 HP = 1/80 = 0.0125（ratio 单位）。
  - ⇒ λ 上界 = 0.0125 / 6 = **0.167 HP/回合 = 16.7× 引擎那一项**。
  - 对照：引擎（`hp_turn`）0.01 HP/回合 → 跨度上 0.06 HP（**6%**，名副其实的 no-op）；
    3× 引擎 → 0.18 HP；10× 引擎 → 0.6 HP。
  - ❌ **我提的「第 20 回合 ≈ 15% 血」= 0.6 HP/回合 = 60× 引擎**，跨度上累计 **3.6 HP
    （360% 的 1HP）** ⇒ 会覆盖真实血量差。用户直接说「过于严重」⇒ **成立，且可量化**。
  - 🔑 **更根本的问题**：这个场景里**回合轴几乎不变**（跨度 6 回合）⇒ 小到合法的 λ 最多动
    1 HP、大到有效的 λ 就是换目标。**两者都不是我们要的** ⇒ 「惩罚回合」在本场景近乎空谈。
  - ⚠️ 顺带发现一个**聚合陷阱**：缓存 `t_turn` 的**行加权均值 = 11.4**（我以前一直引用），
    而**场均值 = 8.6** —— 长局贡献的行更多。引用这个数时**必须说清是哪种加权**。
  - 📌 教训：自己写下的标定工具也要检查单位 —— 第一版把「15% 的血量」写成了「15% 的 1 点血」，
    差了 80 倍；**存下来的工具带错公式是最坏的一种错**（会被以后的我当权威引用）。

  | 场景 | 教师500 | N0 裸 | N0 M500 | N1 裸 | N1 M500 | Δ裸 | ΔM500 |
  |---|---|---|---|---|---|---|---|
  | `boss` | 47.0% | 29.0% | 44.0% | 31.0% | **59.0%** | +2.0 | **+15.0** |
  | `boss_wk`（换卡组 start+5c3u）| 25.5% | 16.5% | 28.5% | 21.5% | **39.0%** | +5.0 | **+10.5** |
  | `gremlins`（4 怪，训练从未见 >1 怪）| 100.0% | 93.5% | 99.5% | 93.5% | 99.5% | 0.0 | 0.0 |
  | `elite2`（Act 2 精英池）| 15.0% | 1.0% | 8.0% | 1.0% | 12.5% | 0.0 | +4.5 |

  - ⭐ boss 的 44.0/59.0 与 `_az_train.py seeds` 那个阶梯**逐位一致** ⇒ 两个独立 harness 自洽
  - ⭐ **搜索增益能迁移，不是 Boss 专属过拟合**：`boss_wk` 换卡组后 N1 M500 从 28.5→**39.0%**，
    且 39.0% **超过教师 500 sims 的 25.5%**（只用网络先验+价值头，不用 C++ 引擎）
  - **收益仍吃在 value 头**：Δ裸 = 0/+2/+5/0，ΔM500 = 0/+4.5/+10.5/+15.0（预算越大收益越大）
  - ⭐ **多怪表示能泛化**：4 怪的 `gremlins` 裸网络 93.5%（随机 42%）—— `monster_slots=5` 但只训过单怪不是问题
  - `gremlins` 是天花板效应（93.5→99.5，教师 100）= **效能对照，不是证据**；`elite2` 仍**基本不会打**（12.5% vs 教师 15%）
  - ⚠️ `random_action` **未播种** → 随机基线每次不同（boss 0.5% vs 1.0%），只能当粗参照，**不能配对比较**
- 📁 **`data/` 只在远端**：本地 `data/` 是**空的**（`*.npz`/`*.pt` 被 gitignore，缓存是远端产物）。
  4 份数据都在 `/root/autodl-tmp/sts_lightspeed/data/` ⇒ 依赖缓存的脚本
  （`_parity_test.py`、`_az_train.py train`、`_pile_ab.py`）**必须在远端跑**。
- ❌ **GPU 不值得（`_bench_forward.py`，Release 构建）**：模型仅 **177,457** 参数，前向瓶颈是 **Python/逐层调度**而非 FLOPs
  ⇒ **CUDA 在实用的 batch 上更慢**：batch=1 CPU 735µs vs CUDA 1296µs（= 0.57×）；
  batch=4（MCTS(500) 用的）0.63×；只在 batch≥64 才 1.13×。训练单步 13.14ms vs 11.59ms
  （且 CPU 只用了 1 个线程）。CUDA 曲线几乎全平 = 被 kernel launch 开销卡住。
  ⚠️ 旧表里 CPU `2979µs` 是 **Windows/Debug 构建**，比生产慢 4 倍 ⇒ 当时
  「GPU 前向快 2.3×」是 Debug-CPU 比 Release-CUDA，**不成立**（CUDA 列则完全复现）。
  ⇒ 真正的杠杆是两个**非换设备**的手段：**任务级并行**（18 个单线程进程 7.4×、逐位可复现）
  与 **CPU 批量化**（batch=1→64 每样本 28×，但 batch>4 伤搜索质量）。
- 📉 **战损度量 + 零搜索重放（`_z_revalue.py`，2026-09-13）**：项目里**从没量过战损**
  （`_rollout` 只返回 `(win, steps)`）。`z` 只依赖终局状态，而终局由「访问数 argmax」决定 ——
  那个 argmax 就在缓存的 π 列里 ⇒ **逐局重放，不跑搜索**。自检：重放算的旧口径 z 必须与
  缓存逐位一致 ⇒ 教师段 **1000/1000** ✅。教师（8000 sims，1000 场）：胜率 **62.3%**；
  赢局平均/中位剩余 HP **29.5/28.0**（上限 83）；**战损率 63.2%**；赢局剩 ≤20/≤10 血占
  **37.2%/19.1%**，剩 ≥60 血**只有 5.0%** ⇒ 胜利多是**惨胜**，终局 HP 离散度大。
  自对弈段只 **875/1000** 能重放（`max(counts,key=)` 用字典序 tie-break，重放用 `np.argmax`
  ⇒ 12.4% 场次里「缓存的 π」无法唯一确定执行了哪个动作）。
  ⚠️ 坑：每个缓存的「前半场数」必须显式列（我错用总场数 ⇒ self-play 段拿 seed 1000 重放
  ⇒ 状态错位 ⇒ 执行非法 Action ⇒ **SIGSEGV 139**）；`dict(legal_actions_and_idx(b))` 是**反的**
  （要 `{i: a}`）；`idx_to_action` 对未映射下标返回**非法哨兵**而不报错，执行它会段错误。
  ✅ `rew` 已把终局列 `t_win/t_hp/t_max_hp/t_turn/t_enemy` + `zspec` 写进
  `data/boss_n1000_s8000_term.npz` ⇒ **以后换奖励口径是零成本**（只改 `z_from`）。
- 🎯🎯 **战损 × 搜索预算（2026-09-13，`_gen_eval.py damage 200 boss 500 2000 8000 32000`）**：
  胜率**逐位复现**历史阶梯（47.0/54.0/64.5/69.0）⇒ 战损接进脚手架无副作用。

  | 配置 | 胜率 | 战损 | 赢局剩血 | 回合 |
  |---|---|---|---|---|
  | 随机 | 1.5% | 79.2% | 16.7 | 7.7 |
  | 教师 500 | 47.0% | 65.3% | 27.7 | 8.9 |
  | 教师 2000 | 54.0% | 61.4% | 30.8 | 8.6 |
  | 教师 8000 | 64.5% | 61.2% | 31.0 | 8.6 |
  | 教师 32000 | 69.0% | **56.3%** | **34.9** | 8.4 |

  ⭐ **预算买的东西会换挡**：500→2000 胜率+7.0/战损−3.9；**2000→8000 胜率+10.5/战损−0.2**
  （只买胜利）；**8000→32000 胜率+4.5（饱和）/战损−4.9**（**只买血**）。
  回合数几乎不动（8.9→8.4）⇒ 战损改善是**每回合少挨打**，不是打得更快。
  ⇒ **要减少耗血不必改奖励**：加大搜索预算就是现成杠杆；也解释了「8000 sims 是甜点」
  那条结论在 **HP 目标下不成立**（它是用纯胜率算的）。
  ⚠️ 战损 SE ≈ 2.1pp（赢局 ~138 场）⇒ 61.2→56.3 约 2.3σ（边缘），但 4/4 单调 + 配对。
  ⚠️ 随机基线战损 79.2%（唯一赢的那场只剩 16.7 血）⇒「惨胜」是**结构性**的，下界被场景压着。
- ❌ **改 z 的 HP 权重 = 失败，且两种解释都被实验排除（2026-09-13 `hpw`）**：
  把 `z = r | −1` 改成 `z = −1 + 1{win}·(1 + 2.3r)`（A=2.3 由 `zsplit` 解出：让 HP 方差占比
  从 6.7% 升到引擎的 17.6%），**同数据同 seed 重训 N0**（零采集成本，靠已落盘的终局列）。
  三项全负：MCTS(500) 胜率 38.0→**31.5%**、战损 80.6→**87.4%**、回合 10.0→11.2。
  - **不是欠拟合**：`val R²` 尺度无关，+0.243 vs +0.249（一样好）；裸网络 24.0 vs 25.5（噪声内）
    ⇒ 策略头没坏、value 头也学得动 ⇒ **只有"用 value 做搜索"这一环变差**。
  - **不是 c_puct 尺度混淆**：`csw`（c=1.5/2.5/3.15/4.5）后 hpw 在每个 c 上都更差，且 N0 的
    最优在**更小**的 c（40.5%@1.5 > 34.0%@3.15）⇒"等效探索强度变小"这个解释方向是错的。
    **机制未查明**（不再编）。
  ⚠️ 也**不能**反推「HP 加权有害」：引擎自己更偏 HP（赢局 HP 跨度 7900 ÷ 赢输落差 3585 = **2.2×**，
  hpw 只有 1.13×）却打得最好 ⇒ 问题在「**学出来的 value + PUCT**」这个组合。
  📌 **通用教训**：`aznet.mcts_search` 的 `q = W/N` **未归一化** ⇒ **任何改 value 目标尺度的
  实验都必须同时重扫 `c_puct`**，否则结论不可解释。
- ⭐⭐ **战损 × 严格配对（`_gen_eval.py pair`）：网络「赢得一样多，但赢得脏」**：
  | # | A | B | 胜率 A/B | McNemar p | 交集局 | 剩余HP A/B | Δ(A−B) |
  | ① | 教师8000 | N1 MCTS(500) | 64.5/61.0 | 0.349 | 105 | 34.9/23.1 | **+11.8±1.5（7.9σ）** |
  | ② | 教师500 | N1 MCTS(100) | 47.0/47.5 | **1.000** | 66 | 33.0/23.1 | **+9.9±2.3（4.3σ）** |
  | ③ | N0 MCTS(500) | N1 MCTS(500) | 38.0/61.0 | 0.000 | 70 | 16.0/22.5 | **−6.5±2.1（3.1σ）** |
  ① 胜率差**不显著**却有 +11.8 血差；② 胜率**完全一样**却差 9.9 血 ⇒ **「赢」不决定「省」；
  战损是更高分辨率的强度读数**（7.9σ 的效应在胜率尺上是 p=0.349）。
  ⭐ **「打得慢所以多挨打」的混淆被排除**：② 的回合差 **−0.3（n.s.）**，HP 差仍 9.9（4.3σ）。
  闭环 N0→N1 是**双向改善**（+23pp 胜率 **且** +6.5 血）。
  ⚠️ 估计量是**条件平均**（"在两者都能赢的局上"），不是无条件平均效应 —— 但这正是要问的问题。
  ⚠️⚠️ **聚合「战损」被「赢了哪些局」混淆（选择效应）**：N0 自己就是例子（战损随预算
  73.1→79.4→80.6 变差而胜率 25.5→35.0→38.0 变好）。**跨胜率的战损必须用配对，不能比聚合。**
- **以下整块（闭环迭代 1/2、覆盖诊断、战损配对表）是 2026-09-13/14 的旧协议**：
  当时**种子池只有 0..999**、策略头是 `flat`（`factor` 头晚一周才出现）、选择候选块还是 30 槽，
  卡组规格 / z 口径 / `c_puct` / **评估脚手架在此之后全都改过**。
  ⇒ 按本文件自己的规矩（「重构评估脚手架后必须用哨兵验证」「不同 epoch = 不同协议」），
  **这些胜率与战损数字一律不可直接引用**。
  ✅ **仍成立的是结构/代码级事实**（与协议无关）：
  ① `selfplay()` 产出的 npz 与教师数据**格式完全互通**，训练侧零改动；
  ② 动作源默认 `argmax(counts)` ⇒ **确定性**；同一策略重跑同一批 seeds ⇒ **新状态数 = 0**；
  ③ 因此 **覆盖是「策略」的函数，不是「自对弈轮数」的函数**；
  ④ 「软目标」**已实现、测过、更差**（开关保留仅为溯源，不是待办）；
  ⑤ 混合缓存的 train/val 泄漏**已修**（按 seed 分组分割）。
  ⏳ **要引用闭环收益，必须在当前协议（20k 池 / `factor` 头 / 现布局）上重跑。**
- **闭环迭代 2 = 饱和（2026-09-13）**：N1 自对弈 1000 场 @MCTS(200) **56.2% 胜率**
  （迭代 1 的 N0 自对弈段只有 **35.6%**），混合 118129 样本/3000 场。
  配对 N1 vs N2 @MCTS(500)：61.0→59.0%（**p=0.636**）、交集局 HP −1.4±1.4（1.1σ）、回合 9.8/9.8
  ⇒ **零显著变化**。**标签质量大涨而下游不动**（第三次「拟合好 ≠ 打得更好」）。
  ⭐ **诊断 = 状态覆盖重复**：自对弈执行 `argmax(counts)`（**确定性**）且每轮同一批 seeds 0..999
  ⇒ 迭代 2 的 41k 行与迭代 1 的 42k 行高度重复；迭代 1 有效是因为 N0 与**教师**轨迹不同。
  ⇒ **闭环的燃料是「覆盖」，不是「重复标注」**；而可用种子池只有 0..999 这 1000 场。
- 🚨 **混合缓存的 train/val 分割泄漏** —— 已并入 `experiment-pitfalls.md` 的「按 `bid` 分割会泄漏」条
  （修法：**按 seed 分割**；⚠️ 只影响 `val ce`/`val R²`，所有胜率结论都在 held-out 段，干净）。
- 📌 **AZ 的形态（当前实现）**：三个线性头 `policy_head`(96 logits)/`select_head`(32)/`value_head`(1)。
  训练目标：策略 = **访问数 argmax 的 one-hot（最优动作）**、价值 = **每场一个常数 z**。
  **没有任何按动作输出优势的结构**；"动作好不好"由搜索（`q = W/N` + `c_puct·P·√N/(1+N)`）隐式产生，
  裸网络退化为 `argmax(logits)`。⇒ 严格说是「**搜索最优动作的行为克隆 + 状态价值回归**」，
  **不是** AlphaZero 的策略改进步骤（π_target 被从归一化分布丢成了 argmax）；
  策略改进靠**换更强的标签生成器**实现（教师 → 自对弈 MCTS），不是靠学 visit 分布。
- 🚨 **药水在这个项目里根本不存在**（2026-09-13，重放 100k+ 决策点：合法药水动作 **0** 个；
  引擎自述 `potionCount: 0`）。⇒ 我上一轮说的「药水在 z 里免费 ⇒ 价值头与教师冲突」
  **是空话，已更正**。另：策略头 96 维里 **35 维（[60,95)，36%）是结构性死区**，
  药水扩张虽正确但**从未被执行过**（药水槽也没绑到 Python）。
- 📐 **教师的败局项里没有玩家 HP**（代码级）：赢 = `100*(35+curHp+4*pot-0.01*turn)`，
  输 = `(1−敌血比)*10 − 存活怪数 + …`。在「本状态还没有任何胜局」时（boss 场景占
  **72.6%~79%**），`qualityValue` 排序完全由败局项决定 = **敌方进度 + 存活怪数，无玩家 HP**
  ⇒ 多数决策点上教师**看不到自己的血量**。这是「教师在高难度下表现不佳」的机制之一。
- ✅ **弃用场景池重测（2026-09-13，动作空间修复后）**：那些池原来**只活在注释里** ⇒ 无法重测。
  已改成数据（`_gen_eval.REJECTED`，`calibrate 200 rej_strong act1_full` 可跑），
  并纳入 `validate_tiers`（`rej_strong` 含 LOTS_OF_SLIMES，最该被守）。
  结果：`rej_strong` 教师 **81.0% → 100.0%**（四成员各 50/50）；对照 `rej_weak` 100.0% 不变。
  ⇒ 旧的 81.0% **完全由 bug 造成**：`(100+100+100+24)/4 = 81.0%`，24 就是被压低的 LOTS_OF_SLIMES。
  ⚠️ 原来写的弃用理由「只有 LOTS_OF_SLIMES 有梯度」**正好是反的** —— 修好后两个池都是纯天花板
  （教师 100%、随机 73~80%）。
- ✅ **动作空间 bug 已修（2026-09-13）**：统一索引 81 → **96**（目标字段 4→5；
  96 = 手牌 10×6 + 药水 **5**×7 + 1；`END_TURN` 80→95 并提成常量 `endTurnIdx`/`end_turn_idx`）。
  验证：`_chk_actions.py` 全 63 个遭遇战 **0 失败**；`LOTS_OF_SLIMES` 教师 **24.0% → 100.0%**
  （随机 82.0%）；`boss` 教师哨兵**仍 47.0%**（分解逐字相同）⇒ 重编没扰动 C++ 搜索。
  ⚠️ 两个附加坑（都是**静默**的）：① **药水**指向第 5 只怪会被记成「丢弃这瓶药水」
  （`pt=5` 与 `discard` 撞车，返回合法下标 → 查范围的守卫抓不到）⇒ 守卫必须查**单射性**；
  ② `potionCapacity` 默认 3 但 `POTION_BELT` 会 +2 ⇒ **最大 5**，按 4 槽算会得 89 → 第 5 槽越界。
  ⚠️ 权重作废（策略头形状变了），但**标签数据不必重采**（≤5 怪时是纯重映射）
- ⏱️ 机器吞吐随时间衰减（三次训练 183→196→210s，形状相同：1.0 → 0.15 ep/s，疑似云主机
  CPU 突发额度）。**用户定调：不调查（不可行动）。** 纪律：只信同一次运行内的相对比较
- ⚡ 前向是 batch 平坦的：CPU batch=1 要 **2979µs**，batch=64 只要 3597µs（每样本 28×）；
  **MCTS 里前向占 80.8%**，树操作+Python 只占 19%
- ❌ **batch 不是免费的**。同一网络内配对扫描（`_batch_sweep.py`，sims=100）：
  b=**1** 40.0%/185s | b=**4** **40.5%/73s（+0.5pp, 2.54×）✅** | b=8 35.0%/54s | b=16 41.5%/60s
  | b=**32** **21.0%/56s（−19pp）❌**。（裸网络前后对照都是 29.0%，配对干净。）
  ⚠️ **非单调**（8 掉 5pp、16 反涨），200 局噪声 ±3.5pp → 4/8/16 分辨不出；
  唯一超出噪声的是 **b=32 的 −19pp**。机制：展开新节点先给**均匀先验占位**，
  同批次后续路径就用它下探。污染 ∝ batch、被 sims 稀释 → **`b=4 @ sims=500` 严格弱于
  `b=4 @ sims=100`（实测无损）** ⇒ MCTS(500) 用 b=4 有依据。
  加速上限 ~3.4×（不是我估的 4.6×）。交叉验证旧笔记"batch=32 给 16.5%"——那是真的。
- 🚨 **多线程训练跨进程不可复现**（实测）：`STS_TRAIN_THREADS=8` 下两个独立进程
  同数据同 seed 给出 val_ce **0.841276 vs 0.833643**；`=1` 时两次都是 **0.838129**（逐位同）。
  同进程内重复调用是确定的（L2=0），**跨进程偶发不确定** —— 多线程 GEMM 归约拆分随调度变。
  val_ce 差 0.008 就足以让 200 局评估的裸网络胜率在 **25.0% ↔ 28.5%** 之间摆。
  ⇒ **跨进程 A/B 必须 `STS_TRAIN_THREADS=1`**（代价 71s→182s）。运行表头已打印该值。
  ⚠️ **2026-09-28 复核（本文顶部「续十三」）：上面这两句都不按字面成立** —— 换线程数确实改轨迹
  （1 vs 8 差 0.01~0.02 个 `val_ce`，三台机器实测），但**同线程数跨进程（含 3 路并发超订）逐位
  可复现**，跨 torch 版本（2.8.0+cu128 ↔ 2.12.1+cu130）也逐位相同。规矩保留、理由换成「线程数是
  配方的一部分 + 单线程数值不依赖调度」。
  - ⚠️ **2026-09-21 复核（`_bench_loop.py`，300 批/档）**：**同进程内** 1/2/3/4/8 线程的
    `loss`/`p0` **逐位相同**（含把 4 与 1 各重复一次）⇒ 至少同进程内**不漂**。
    上面那条"跨进程偶发不确定"**仍未测**，所以规矩不改；但配套的"代价 71s→182s（2.6×）"
    **没复现**：实测 8 线程只快 **1.46×**（1→35.4、8→24.3 ms/批）。
  - 机制确实存在且能直接量到：`xi.sum()`（31.5 万 float32 的归约）随线程数**非单调**变
    （1/2/4 → −63125.21875；3/8 → −63125.22265625，正好 1 ULP）——`at::parallel_for` 只在
    规模超 `grain_size` 时才分块、**块边界随 nthreads 变**。本网络的算子（小 GEMM / 96 维
    `log_softmax` / 256 元素 `mean`）都在阈值以下 ⇒ 走同一条串行路径 ⇒ 逐位相同。

### 教师（BattleScumSearcher2 = UCT + 随机 playout）
- `EvalFnc evalFnc` 可插拔，默认 `evaluateEndState`；`playoutRandom` 是**均匀随机**；`explorationParameter = 3√2`
- `evaluateEndState` 量级严重失配：**赢 = `100*(35+hp+4*potions-0.01*turn)` ≈ +3500**，
  **输 ≈ `(1-hpRatio)*10 - alive + ...` ≈ -5..+10**。几乎是二值，深层节点只能靠"随机 playout 恰好赢一次"
- 随机 playout 从开局赢的概率实测 **0.03-0.06%**（32000 次 playout 只赢 1-2 次，
  `root_win_rate` 就是它）。所以搜索在开局几乎拿不到信号
- `evaluateEdge` 两项量级失配：`quality = avg/(bestActionValue-minActionValue)` ≈ 1/n 的极小量，
  `exploration = 3√2·√(log(parentN+1)/(n+1))` ≈ 1.0 → 比值 ≈ 10.6√n，**探索项永远压倒 q 值**
  - 实测后果：开局根边访问分布 `H/lnN = 0.998~1.000`（完全均匀），sims 从 100 提到 32000 都不变
  - ⚠️ 但这**不等于** sims 无效（后段 q 值确实能起作用）。
    ❌ 但 n=30 扫描得出的「每 4× sims +6.7pp / 500→8000 是 33.3%→46.7%」是**噪声**，
    已被下面的 n=200 配对实测推翻 —— 不要再引用那组数字
- 🎯🎯 **`explorationParameter` 扫参结论：默认 3√2 已经很好，调小是灾难**（200 局同 seed 配对）：
  4.2426 → 34.5%；0.5 → 16.5%（**−18.0% ± 2.9%, p<1e-4**）；0.1 → 12.0%（**−22.5% ± 3.2%, p<1e-4**）。
  而 c=1.06（÷4）在访问分布上与 4.24 几乎无差别 → **「÷4」既无效又有害**。
  **向上也掉**：c=12 → 30.0%（−4.5%±2.8%, p=0.15）；c=40 → 24.5%（−10.0%±3.2%, p=0.003）。
  → **双向都更差，`3√2` 是局部最优，这个旋钮可以关掉。**
  失败模式不是提前结束回合（打牌占比 72/74/75% 几乎一样），就是选错动作。
  **原因：`avgEvaluation` 是随机 playout 下的终局评估，衡量「随机乱打能打到哪」而非「动作多好」。
  低信号状态下它是噪声，硬利用会锁定烂线；高探索让这些状态退化成近均匀随机，反而是保护。**
  ⚠️ **不要用单状态熵/访问分布判断搜索好坏**（我因此判错 3 次）。均匀 ≠ 坏。
- 🎯 单状态现象（真实，但**不代表**搜索坏）：`search_stats(500, c)` 只改 c，
  c=4.2426 → 访问数 [103,92,100,103,102]；**c=0.1 → [1,17,468,1,13]**（e2 拿 93%）。
  那只是说明 q 项存在，**不说明利用 q 更好**。
- 根因（仍成立，但不是调小 c 能修的）：`quality = avg/(bestActionValue - minActionValue)` 用**全局** range，
  无胜局时 range≈14，一旦出现胜局 bestActionValue 跳到 +3500 → range≈3505 → quality 瞬间缩小 250 倍
- 可用 `b.search_stats(sims, c)` / `teacher_action(..., exploration_parameter=c)` 覆盖
  （2026-09-12 已加绑定，`sts.DEFAULT_EXPLORATION_PARAMETER`）
- π = 根边 `simulationCount`（`search_stats` 返回），`teacher_action` 取 argmax
- **`randGen(bc.seed + bc.floorNum)` → 教师完全确定性**（同状态同 sims 必给同一答案）。
  所以标签噪声是确定性噪声：网络能背下来（train_ce 可到 0.02），只是背的是错的东西
- **策略标签质量（跨 120 状态实测，_teacher_diag.py labels）**：
  H/lnN 中位 0.968(500) → 0.835(8000) → **0.389(32000)**；
  top1-top2 中位 0.084 → 0.385 → **0.809**；argmax 一致率 500vs8000 **66.3%**、8000vs32000 81.1%（多动作状态）
  - ⚠️ **20.8% 的状态只有 1 个合法动作**（≤2 的占 30.8%）→ 对策略头零贡献。
    算 H/lnN 时这些返回 0，**均值被污染**（500 sims 均值 0.62 vs 中位 0.97），只能用中位
- ⚠️⚠️ **seed 基陷阱：`_boss.py: BOSS_SEED0 = 30000` ≠ `_az_train.py: TRAIN_SEED0 = 0`**。
  `_teacher_diag.py sweep` 跑 seed 30000+，`_az_train.py collect` 跑 seed 0+。
  两批场景完全不同 —— 我因此把 n=30 扫描的噪声当成了结论
  ⚠️ **2026-10-01 核实：`BOSS_SEED0` / `TRAIN_SEED0` 两个常量都不存在了**（`grep -rn 'BOSS_SEED0\|TRAIN_SEED0' *.py` 为空）⇒ 本条只剩历史意义，别再照它排查。
- **教师 sims 的真实效果（n=200 同场景配对，`_cmp_cache.py`）**：
  500 sims 34.0% vs 8000 sims 35.5%，**+1.5% ± 3.0%，McNemar p=0.74 → 胜率无差异**。
  但**双方都赢的 52 场里血比 0.310 → 0.370（配对差 +0.060 ± 0.022，t=2.70，p≈0.009）**。
  → 牌组强度封住胜负上限，搜索改进只体现为血量。16× sims 只买到这点东西
- 唯一确定的收益：策略标签一致率 500→8000 是 66%→81%（与场景无关的标签稳定性测量）
- 🚨 **训练对比（200 场 @500 vs @8000，同超参 `REG=8/128/0.2`）**：
  val_ce 1.44~1.56 → **0.80~0.89**（降 43%）、val_R² −0.25~+0.17 → **+0.23~+0.37**，
  **但裸网络胜率 8.0%±1.9% vs 8.1%±2.2% —— 完全一样**（MCTS(20) 8.3% vs 7.9%）。
  → **`val_ce`/`val_R²`/`train_ce` 在本任务是误导性指标，别用来衡量进展**。
  标签更自洽 = 网络更好拟合，但映射本身没变强。网络只是在**缩放教师强度**（两空间都是 25%）
- 网络目标（`_az_train.py`）：`loss = CE(policy81) + CE(select32) + MSE(value)`；
  策略目标由 `targets()` 选：默认 `hard_target(π)` = one-hot(argmax)，
  `STS_SOFT_TARGET=1` 时 `soft_target(π)` = 逐行归一化访问数（2026-09-13 加，默认 off）；
  `z` = 胜时血比 / 败时 −1.0。**`data["pwin"]` 是死数据**（采集了但 train 没读）
- 诊断脚本 `_teacher_diag.py`（`sweep` 模式 = 同 seed 配对扫描，消掉卡组质量方差）

  ⚠️ `_teacher_diag.py` 已删（2026-09-30）；同类离线诊断现在的入口是 `_imit_probe.py`。
### ⚡ 前向开销分解与 `torch.compile`（2026-09-19，远端，脚本 `_bench_compile.py` / `_bench_mcts.py`）
⚠️ 所有网络基准都是**单线程**的：`_az_train.py:81` 在 import 时 `torch.set_num_threads(ROLLOUT_THREADS=1)`。
这正是 rollout 的真实工况，不要"修"它。

**MCTS(20) 端到端（远端，boss 场景，µs/模拟）** —— 关键是有个「零前向」行做地板：

| 配置 | µs/模拟 | vs eager b=1 | 前向占比 |
|---|---|---|---|
| **零前向（NullNet, b=4）** | **116.1** | 7.90× | — |
| eager b=1 | 916.6 | 1.00× | 87% |
| eager b=4 | 374.1 | 2.45× | 69% |
| compile b=4 | 227.5 | 4.03× | 49% |
| compile b=8 | 196.9 | 4.66× | 41% |

⭐ **不跑网络也要 116 µs/模拟（2.32 ms/局）** ⇒ 它占最优 eager 配置的 59%。
⇒ **「把前向搬进 C++」的上限只有 ~1.3-1.4×**（因为 59% 根本不是前向）；
  要拿到 4-5× 必须把 **PUCT 树**也搬过去。而 `torch.compile` 两行代码就给了 4.03-4.66×。
⇒ 结论：**别先做 C++ 网络**，先吃 compile。

**`torch.compile` 细节**（远端有 gcc，本地**缺 `cl.exe` 用不了**）
- 前向加速 1.84×（b=16）~ 3.78×（b=1）；`dynamic=True` 比 `dynamic=False` 慢 1.1-1.3×
- 与 eager 差 **2.98e-08**（GEMM 归约顺序），**不是逐位一致**；实测 30 局 boss b=1 决策 **30/30 相同**
- ✅ **`recompile_limit` 不需要动**（我先前说「必须提高」，已更正）。实测（`_chk_recompile.py`）：
  真实环路里 `k = min(batch_size, sims-done)` 且每轮 `done` 恰好增 `k`
  ⇒ **形状只有 ≤2 个**（`batch_size` 和最后一轮的余数）：sims=500 b=4 → 125 次前向全形状 4，**0 次重编译**；
  b=8 → {8:62, 4:1}，只多编 1 张。而 limit 是 8。`STS_RECOMPILE_LIMIT=8` 与 `64` 跑出来**逐位同**（b=16: 45.34 vs 45.36）。
  撞 limit 的是**我的基准脚本**（故意喂 5 个形状），且真凶是 **grad 模式也算 guard**：
  1 形状 × 2 模式 = 2 张图；5 形状 × 1 模式 = 5 张；**5 形状 × 2 模式 = 9 张 → 超限**，
  最后一个形状（b=16）没编成 → 0.92× 的"compile 变慢"就是这么来的。
  `mcts_search` 里网络**始终在 `torch.no_grad()` 内**被调用 ⇒ 生产路径上不可能触发。
- 本地报错是 `InvalidCxxCompiler: Compiler: cl is not found`（不是 GBK，那个用 `PYTHONUTF8=1` 解决）

**拓扑读数（远端，b=4，单线程）**
- eager forward 875.3 µs，其中 `_inputs` **593.6 µs**（68%）
- `trunk + 3 个头`（纯 GEMM，无编码/切片）= **134.7 µs** = 11.5 GFLOP/s
- `card_embed(4×250)` = **19.1 µs**（纯查表、零浮点）
- 每次前向 ~200 次算子派发 ⇒ 瓶颈是**派发**不是浮点

**`torch.jit.trace`**：1.43-1.71× eager 且**逐位一致**，比 compile 慢但仍免费。
❌ 我先前报的「jit.trace 0.75× 更慢」**复现不出来，已撤回**（两次独立测量都是更快）。


> ⚠️ **本节多数数字产自 09-13~09-20 的旧协议**（快照 **1111/1136**、旧卡组规格、旧 z 口径）——
> 按本节自己的纪律（「换编译器 / 换卡组 ⇒ 全部绝对胜率作废」），这些**绝对数字对当前代码不可引用**；
> **结构性结论**（哪条路无效、哪个量纲错配、什么算子会漂）仍有效。要新数字就在当前协议上重跑。

# ④ 坑与教训（方法论 —— 最贵的资产）

### ⚠️ 进度条：长任务后面不要接 `| tail`
`tail -N` 是**汇（sink）**，必须读完 stdin 才知道最后 N 行 ⇒ 输出在进程退出时**一次性**到达。
`stdbuf` / `python -u` / `flush=True` **全都救不了**（`grep` 不加 `--line-buffered` 同样块缓冲）。
实测（20 步 demo 看每行**到达**时刻）：无 tail 逐行到达；有 `tail -3` → **全部 4.00s 才到**。
⇒ 正确姿势：`ssh -tt`（强制 TTY）+ **不接任何管道**；要过滤就远端落盘、本地 `tail -f`。

（`_prog.py` 与 `STS_PROGRESS` 已于 2026-09-23 删除，全部改用 `tqdm`；上面那段「`tail` 是汇」的教训与实现无关，**仍然成立**。）
- ⚠️ **别在 `_sync.sh` 运行期间编辑它**：bash 按**字节偏移**边读边执行，中文注释的半个字节
  会被当成命令（踩过两次，exit 127）。已结构性修复（`run`/`bg` 先把自己的副本 `exec` 出去）。

### 🚨 不要在对比中途重编 C++
`seeds` 输出里的 `教师 500sims` 那行是**基准哨兵**（确定性）。
2026-09-12 我重编后它从 24.0% → 31.5%（差 30/400 局），
因为我把 `3*sqrt(2)` 换成了字面常量 `4.242640687119285`（可能差 ≤1 ULP），
而 `selectBestEdgeToSearch` 用严格 `>`，近似平局处翻转 → 早期分叉整局。
**规矩**：「跑 A → 改代码 → 跑 B → 比 A/B」中间不能重编；非改不可则 A 要重跑。
哨兵一动，之前所有绝对数字作废。任何 C++ 浮点常量的「无害重构」都当行为变更。

### 🚨 杀作业要按「匹配 python 的 args」杀，杀 wrapper 无效

`_sync.sh bg` 的作业结构是 `bash -c` → `setsid nohup script -q -e -c '<py> …'` → `python`。
`pgrep -f <pat> | head -1` 拿到的是**最外层 wrapper**；杀它只会让真正的 python 变成孤儿
**继续跑**；若它已进入评估阶段，`ParRunner` 的 64 个 worker 一起被孤儿化、继续吃 CPU。
实测：以为杀掉了，其实 **66 个进程还在跑**，load 16。

- **杀法**：`pkill -f "<脚本名，其中一个字符加方括号>"`（一次匹配父 + 全部 worker）。
  方括号是为了不匹配自己的 ssh 命令行：`pkill -f "_az_train.py seed[s]"`
- **验证**要看 `ps -eo args | grep -c …`（应为 0），别只看命令退出码
- `ps` 的 `%CPU` 是**生命周期均值**：刚被杀的僵尸仍显示 18%，别据此以为还在跑；
  看 `stat` 列是否 `Z`

### 🚨 输出里的 ✅/❌ 在 Windows 上「终端正常、一管道就崩」

本地 Windows 控制台默认 **GBK**。直接跑在 VS Code 终端里正常（终端是 UTF-8），但一旦被
管道（`| tail`、`> log`）Python 就改用 locale 编码 + `errors="strict"` → `UnicodeEncodeError`
**当场崩**。已踩两次（snippet 一次、`_gen_eval.py tiers | tail` 一次）。

**修法**：`_env.py` 在 import 时对 stdout/stderr 调
`reconfigure(encoding="utf-8", errors="replace")` —— 凡 `import _env` 的脚本自动免疫。

### 🚨 不要同时启动多个作业（2026-09-13 我自己犯的）

每个作业**内部**往往已经开了 64 路 `ParRunner`（`_gen_eval` / `_az_train eval` / `_pile_ab`）。
再让 4-5 个作业重叠 ⇒ 互相拖慢（N1 训练 3.2 min → **16 min**）、计时完全不可解读，
而且直接违背「只信同一次运行内的相对比较」这条纪律。

- **规矩：一次一个作业。** 想「并行」就往**一个**作业里加维度（配置 × 种子），不要开第二条流水线
- 违反的代价不是慢，而是**结论不可归因**：分不清变快/变慢是配置还是资源争用
- 也别把未经确认的作业留在后台跑 —— 取消就真停
- 🚨🚨 **2026-09-21 发现的新代价：并发作业会被外部 SIGKILL（退出码 137）**。三个单 seed 作业
  （各 ~3.5 GB）并行时，其中 **2 个在同一分钟内被 SIGKILL**；日志**直接停在半路**、没有 `.pt`、
  **没有任何 traceback**。同一个坑早上已经以另一种形态出现过：3 个 `multiprocessing.Pool` worker
  里死掉 1 个（pid 2392），而 `Pool.map` 要集齐 3 个结果 ⇒ **永久挂起**，我白等了 20 分钟。
  - 判据：日志尾部 `Script done … [COMMAND_EXIT_CODE="137"]` = 128+9 = **SIGKILL**。
    ⚠️ 所以**日志的末尾不能只看有没有“结果行”**，要看退出码。
  - **`dmesg` 在这个容器里是 `Operation not permitted`** ⇒ 别再拿“没有内核崩溃记录”当证据
    （我犯过：`dmesg … 2>/dev/null | grep oom` 把权限错误一起吞了，看起来像“干净”）。
  - 我们 cgroup 的 `oom_kill=0`、宿主机可用 **338 GB** ⇒ **不是我们自己的 OOM**；
    杀人的在 cgroup 之外（平台/宿主），发送者查不到。
  - ✅ 缓解：**一个作业一个进程**（串行跑 3 个 seed，而不是 3 进程并行）。实测单作业 RSS 3.8 GB，
    而且教师评估从 ~80 局/s 升到 **238 局/s** —— 争用消失。

### 🚨 本地 ssh 长连接会被对端 reset —— 等待要放**远端**

2026-09-21：本地阻塞轮询（`ssh … 'for i in $(seq 1 75); do …; sleep 60; done'`）在 **14 分钟**后
`Connection reset by peer`（退出码 255）。作业是 `setsid` 脱离的、没受影响，但**等待白等了**。

✅ 对策：等待写成远端脚本（如 `_wait_logs.sh`），`setsid nohup bash … > 结果.txt &`；
本地只做**短连接轮询**那个文件。断线就只是重连，等待本身不受影响。
⚠️ 该脚本要**登记进 `_sync.sh` 的 `FILES`** 才会被 `push` 送过去 —— 2026-09-21 又踩一次：
没登记时远端跑的还是旧版（硬编码 `ab_e32`），于是"等 ab_dt"实际打印了 ab_e32 的结果，**静默交叉**。

### ✅ 2026-09-23 补充：守望也可以放**本地** —— 只要每轮新开一条连接

实测：循环里每轮一条**短连接**（`ConnectTimeout` 有界），单条被掐断只是丢掉这一轮，
下一轮换新连接继续；远端 screen 里的作业完全不受影响，**完成回调照常送达**。
⇒ **长连接才是问题，重连不是**。"等待要放远端"这条仍对**无人值守链**成立
（`_queue.py` / `_wait_logs.sh` 干的就是这个，已保留）。

### 🚨🚨 2026-09-21 同一条又踩一次：这次是「并发数」直接招杀

`_scale_run.sh` 起 **14 个单线程训练进程**（每份 RSS 实测 **5.1~6.2 GB**）⇒
**9 个在同一分钟内被 SIGKILL**，而且**连累**了另一个已跑了 3 小时的主实验
（`run.ab_dt3.log` 尾部 `Script done on … [COMMAND_EXIT_CODE="137"]`，停在 seed 0 的 87.5%）。
同一时刻的死法全是 137 ⇒ **同一个外部杀手**。

**不是 OOM** —— 这一点必须查清，否则会修错方向：
`memory.events` = `low 0 | high 245330 | max 0 | oom 0 | oom_kill 0`，
即 cgroup 的 `memory.max`(66.5 GB) **一次都没打到**、`oom_kill` **为 0**。
被打到的只有 `memory.high`(64 GB)，那是**节流**（reclaim）不是杀。
`dmesg` 是 `Operation not permitted` ⇒ **看不到内核记录，别把「没有记录」当证据**。
`/sys/fs/cgroup/` 下**没有子 cgroup**（`ls -d */` 为空）⇒ 被杀的进程就在根组里，
所以「死在别的子组里」这个解释也不成立。

**能用的结论**：`memory.high = 64 GB`、每份 ~5.5 GB ⇒ **并发 ≤ 10**（留余量）。
实测：**5 并发稳**（每份速度几乎不变，见下条），**14 并发死 9 个**。
⚠️ **2026-09-23 起这一档作废**：单臂 RSS 已涨到 **9~26.6 GB**（见下），并发上限必须按新量级重算。

### 🚨🚨 2026-09-23：同一杀手重演（6 臂死 4）

6 臂并行（各 8 epoch、共享同一缓存），四臂在 epoch 7/8 附近被杀：
`137 @00:47:35 / 00:48:10 / 01:00:12 / 01:02:38`；两臂正常（退出码 0）。
`oom_kill` 仍 0、`memory.max`(62 GiB) 未被击中、`dmesg` 无权限 —— 与 09-21 判据一致：
**杀人在 cgroup 之外**。两条新事实：

- **单臂 RSS 涨了一个数量级**：3.8（none，eval 期）→ 9.0 → **26.6 GB**；死亡的 bi_s1 就是全场最大
  进程。内存预算类结论都必须按这个新量级重算。
- **判完成要看退出码的数值**：`grep -l COMMAND_EXIT_CODE` 会把 137 当"完成"（据此误报过）。

### ⚡ 并行要开在「任务」这一层，不要开在 BLAS 里

可复现性由 **torch 线程数**决定，**不是进程数** —— 单线程进程的 GEMM 归约顺序与 CPU 争用无关。
所以「N 个单线程进程并行」与「串行跑 N 次」给出**逐位相同**的结果（fork 后各进程独立）。

- 实测（`_pile_ab.py`，18 个「配置 × 种子」）：**56 min → 7m34s（7.4×）**，且控制值逐位相同
  （串行 `k4_sum` seed 0 = `val_ce 0.838 R2 +0.262`，并行版一模一样）—— 用控制值验证并行化是标准做法
- 反之 `STS_TRAIN_THREADS=8`：据称同配置同种子**跨进程**都会漂（val_ce 差 0.008 → 200 局胜率摆 ±3.5pp），
  而典型待测效应只有 3pp 量级 ⇒ 若成立，多线程是拿精度换速度，不值。
  ✅ **2026-09-21 定论：这条是对的，而且是大幅漂移** —— 用 `_cmp_ckpt.py` 比**完整 8 轮训练**的
  存盘权重（同代码 / 同缓存 / 同 seed，**只改** `STS_TRAIN_THREADS`）：
  **25/26 个张量不同，`trunk.0.weight` 最大 |Δ| = 0.87** ⇒ 不是“1 ULP 漂”，是**轨迹分叉**。
  ⚠️ 我先前用 `_bench_loop.py` 的 300 批探针说“没复现”，**那是坏判据**：探针在**计时分段里跑
  `net.train()`**，dropout 消耗 RNG ⇒ 探针值随脚本结构变；而且它在 1..8 线程下逐位相同，
  **却在完整训练里漏掉了 0.87 的分叉**。
  ⇒ **判“线程/机器是否改变结果”的唯一可靠手法：跑完整训练、比 `.pt`。**
  - 机制是**真**的，我直接量到了：`xi.sum()`（31.5 万个 float32 的归约）随线程数**非单调**变
    （1/2/4 → −63125.21875；3/8 → −63125.22265625，正好差 1 ULP）。因为 `at::parallel_for`
    只在规模超 `grain_size` 时才分块、**块边界随 nthreads 变**，而浮点加法不结合。
    ⚠️ **更狠的一点**：`xi.sum()` 在**同一线程数**下跨运行也会变（两次运行里同为 8 线程，
    一次给 −63125.21875、一次给 −63125.22265625）⇒ 分块还取决于**线程池状态/调度**，
    不只是 `nthreads`。**这解释了原作者看到的跨进程漂移**，所以“漂移”那条不是错的，
    是我先前“没复现”说过头了 —— 准确表述是：**它没传染到这个网络的输出**。
  - ⇒ **必须钉住 `TRAIN_THREADS`**；要用多线程就得**整套基线重跑**。
    （先前我写「这个网络的算子都在阈值以下 ⇒ 逐位相同」是**错的**，已被 25/26 的硬证据推翻。）
  - 📌 **2026-09-28 复核（三台机器 / torch 2.8・2.12・2.13，见 `.agent/memory/sts-lightspeed-rl.md` 续十三）**：
    手法与这里要求的一致（跑完整训练、比 `.pt`）。**「换线程数 ⇒ 分叉」复现**（1 vs 8：
    `max|Δw|` 0.65/0.65/0.89，与这里的 0.87 同量级）；**「同线程数跨进程偶发漂」没复现** ——
    定住线程数后，两个独立进程（以及 **3 路并发 × 8 线程、24 线程挤 16 核**）的权重 sha 逐位相同，
    跨 torch 版本（2.8.0+cu128 ↔ 2.12.1+cu130）也逐位相同。⇒ 规矩不变，理由换成
    「线程数是配方的一部分」；守卫已落成 `_chk_threads.py`（在 `_chk_all.py` engine 档）。
- **持续跑会掉到 base 频率（2026-09-21 实测；解释了“bench 偏乐观”与“越跑越慢”）**：
  同一个 1 线程作业的 epoch 耗时是 **76 → 148 → 134 → 188 s**（8 线程版 41 → 68 s，同样的 1.65×；
  两个作业**同机同时**跑，所以不是互相干扰）。查频率：持续负载下各核停在 **2000 MHz**，
  而 lscpu 的 base/max 是 **2100/3500** ⇒ 比值 1.67×，与实测吻合 ⇒ **短跑吃 turbo、持续跑落回 base**。
  ⇒ ① `_bench_loop.py`（~10 秒）**必然偏乐观**，不能拿它报“每 epoch 分钟”；
  ️② 跨机/跨时的吞吐比较**无意义**；③ 同一作业的**第 1 个 epoch 总是快**。
- 🔻 **评估时 `STS_WORKERS` 别打满配额**：`cpu.stat` 里 `nr_throttled=2049`、`throttled_usec=111.7 s`
  —— 16 worker 正好顶满 16 核配额 ⇒ 任何额外线程都触发节流。留一点余量（如 14）**反而更快**。
  ⚠️ 旋钮现在叫 `--workers`（`_env.workers()`），「别打满配额」这一点不变。
- 📌 **线程的真实收益（同机同时配对，2026-09-21）**：8 线程 vs 1 线程的**第一个 epoch**
  **41 s vs 76 s = 1.85×**（两个作业同时在跑 ⇒ 不是互相干扰的假象）。代价是 8 核换 1.85×，
  而“按 seed 分进程”是 **3 核换 3×** ⇒ 每核收益差 ~5 倍。
- ⚠️ **小样本计时不能当数**：同一 bench 用 30 批（每档约 **1 秒**）得 1→33.4 / 8→26.9 / 16→29.2，
  我据此说过"只快 1.24× 且 16 线程更差"；换成 300 批后变成 **1.46× 且 8=16**。
  **1 秒的样本连 10% 的差异都定不住。**
- ⚠️ `_az_train.train()` 内部会调 `torch.set_num_threads(_env.train_threads())`，**覆盖**你在
  worker 里设的值 → 必须走后缀参数 `--train-threads`（旧 `STS_TRAIN_THREADS` 已废）；
  **2026-09-28 起默认 1**。`_train_seed` 有硬守卫拦「并行训练却用多线程」这种静默过载
- 注意：`_env.train_threads()`/`rollout_threads()` 是**函数**，不是 import 期常量 ——
  后缀参数在 `parse_args()` 之后才解析得到（运行期改 `os.environ` 早就不生效了）
- 🚨 **`_env.workers()` 在 `workers='serial'` 时返回 `None`** —— 使用方必须处理它，
  ⚠️ **2026-10-01**：旋钮已改成 `--workers serial`（或 `0` / `off`）；「返回 `None`、调用方必须处理」
  这条**仍然成立**。
  否则是两种不同的事故（2026-09-21 一次扫出来两处）：
  - `min(n_seeds, _workers())` ⇒ **TypeError**（`_az_train.py` 的 `PAR_TRAIN` 分支，Pylance 一直在报）
  - `Pool(_workers())` ⇒ **静默**退回 `os.cpu_count()`（宿主机 **128**）⇒ **静默超载**，
    正是「降级参数不要静默兜底」那类坑（`selfplay()` 就是这写法）
  - 已修：`PAR_TRAIN` 归一成 0 后落回串行分支并**明说**为何没并行；`selfplay()` 直接**拒** `serial`
    （要单进程就写 `STS_WORKERS=1`，语义清楚），并把 `Pool(w)` 改成 `Pool(min(w, n_battles))`
  - 正确写法参照 `collect()`：`if w and w > 1 and …` —— 它一直是对的，两处是漏网

### 🚨 评估场景是「现场生成」的，不存进缓存
`_boss.make_boss_battle` 在评估时**现算**卡组/Boss；缓存里只有快照。
所以改了 `_boss.build_deck` 之后，旧缓存训练的网络会被拿去在**新卡组**上评估 ——
数字全变但**没有任何报错**。实测同一权重（val_ce 六位小数不变）：
裸网络 14.5% → 19.0%、MCTS(100) 16.0% → 23.5%。
**防线**：`_boss.scenario_fp()`（前 8 个场景的 SHA1），`collect` 存、`train()` 比对，
不符直接 `RuntimeError`；旧缓存无该字段则告警。

### 🚨 「按结果分组的聚合指标」会被选择效应污染（2026-09-13）

「胜局平均战损」看着无害，实际**被「赢了哪些局」决定**：胜率越高，边际胜局越难（惨胜），
平均值被拉低。实测 N0 自己就是例子 —— 战损随预算**变差**（73.1 → 79.4 → 80.6）
而胜率**变好**（25.5 → 35.0 → 38.0），方向完全相反，纯属选择效应。

- **规矩**：凡指标是「条件在某个受处理影响的变量上」算的（只对胜局、只对成功样本…），
  **跨条件比较必须配对**（取两条件都满足的子集），**不能比聚合值**。
- 本仓库的修法：`_gen_eval.py pair` —— McNemar（胜率）+ **交集局配对差 ± SE**。
- ⭐ 回报：教师 vs N1 的差在胜率尺上是 **p=0.349**（看不见），在交集局战损尺上是 **7.9σ**。
  **指标选对了才看得见。**

### 🚨 按 `bid` 分割 train/val 会**泄漏**（2026-09-13 发现）

`battle_split` 按 `bid` 随机分 train/val。当缓存里**同一个 seed 出现多次**（教师数据 +
自对弈数据、或多次自对弈迭代）时，两批都用 `off` 偏移保证 bid 不撞，**但两边都从 seed 0 开始**
⇒ **同 seed 的两行会一个进 train、一个进 val** ⇒ 泄漏。
`assert got == want` 只保证 **bid 唯一**，**不保证 seed 唯一**，因此没有任何东西挡它。

- 后果：混合缓存的 `val ce` / `val R²` **乐观偏高**（实测 N2 的 val R² 跳到 +0.381 而下游零变化）
- ⚠️ 好消息：**下游结论不受影响** —— 胜率评估用的是 held-out seeds（1000+），**从未进过训练**
- **规矩**：分割按「**场景身份**」（seed）而不是按「行/批次编号」；同 seed 的所有行必须同侧

### 🚨 npz 加新键 = 一次「grep 所有读者」的动作
- `np.load` 的通用读法 `{k: d[k] for k in d.files}` ⇒ **加键无害**。
- 但 `selfplay(mix_with=)` 是**显式循环拼接**：
  ```python
  for k in data:
      out[k] = np.concatenate([mx[k], data[k] + off]) if k == "bid" else ...
  ```
  ⇒ 旧格式缓存没有这个新键就是 `KeyError`；**0 维标量**（`space`/`fp`）则直接
  `ValueError: zero-dimensional arrays cannot be concatenated`。
- ⚠️ **迭代 1 不走这条路所以没暴露，闭环迭代 ≥2 必经** —— `space` 就是这样到第二次迭代才炸的。
- ✅ 已加护栏：跳过 0 维键 + 断言两端 `fp`/`space` 一致 + 缺键时抛**可读的** RuntimeError
  （并说「重跑生成它的命令即可」）+ 对「只在一侧存在的键会被丢弃」发警告。
- 📌 **一般化**：加键时不仅要 grep **读取点**，还要找有没有「按另一个文件的键集遍历」的代码。

### 🚨 跨命令的「隐性协议差异」= 伪差异（2026-09-13）

`csw` 第一版把 `batch` 硬编码成 `1`，而 `netdamage`/`damage` 从 `STS_MCTS_BATCH` 读（4）。
于是同一网络同一 sims 的胜率**跨命令差 ~5pp**（40.5% vs 35.0%），我差点当成真实差异。
（batch 对搜索质量本身就有 ~5pp 的影响。）

- **规矩**：凡会影响结果的参数，**所有命令都从同一个 env 读**，并在输出里打印出来
- 参考：`_az_train.py seeds` 打印 `MCTS batch=` 与 `train_threads=` 就是为此

### 🚨 改 value 目标的**尺度**必须同时重扫 `c_puct`（2026-09-13）

`aznet.mcts_search` 里 `q = W/N` 是**原始** value 均值、**未归一化**，而探索项
`c_puct·P·√N/(1+n)` 尺度固定。所以把 value 目标整体放大 s 倍（等价于 q 放大 s 倍）而 c 不变，
就等价于把**探索强度除以 s** ⇒ 搜索突然变成重度利用一个噪声 value
（实测表现：胜率降、回合涨、战损涨）。理论上 `c' = s·c` 恰好还原原决策规则。

- 应用：任何改动 `z` 尺度的实验（加项、换口径、分布化）都必须**同时**扫 `c_puct`
- ⚠️ 但这条**可能不够**：hpw 实验在把 c 调对之后仍然更差 ⇒「尺度」不是唯一因素

### 🚨 重构评估脚手架后必须用**哨兵**验证（2026-09-13）

我把 `_collect` 的返回值从 3 元组改成 dict（涉及 **13 个调用点**），改完立刻跑
`damage 200 boss 500`：**教师 500 sims = 47.0%**（历史记录逐位一致），且**战损四列
65.3 / 27.7 / 13.0 / 8.9 逐位相同**。

- **规矩**：这类机械重构**必须**有「已知值」哨兵，否则改动引入的偏差会被当成实验效应
- 更强的做法：多要一个**未加工的**派生量（这里就是战损四列）一起核对，不只主指标

### 🚨 SSH 断线 ≠ 远端作业失败（2026-09-13）

`_sync.sh run` 中途报 `Connection reset by peer` / `client_loop: send disconnect`，
**但远端作业正常跑完、退出码 0、日志完整**（实测一次）。

- **规矩**：先 `bash _sync.sh status`（看退出码与日志尾），**不要**急着重跑
- 也不要急着「重跑一遍」—— 那会真的启第二个作业（见上面的并行纪律）

### 🚨 样本量门槛
- 熵 / 分布 / 标签一致率类指标：必须**跳状态采样**（我在单个开局状态上判错过 3 次）
- 胜率类结论：**n≥200**，且用**同场景配对** + McNemar
- 优先找**连续量**（血量、z）而不是二值量（胜负）—— 后者功效差得多
- 单状态访问分布**不能**用来判断搜索好坏：均匀 ≠ 坏（高探索在低信号状态下是保护）

### 🚨 「只挑有用的」= 选择偏差：我手写的遗物白名单夸大了 6 倍（2026-09-21）

给场景加遗物时，我手挑了 **18 件「对战斗有影响」的强遗物**（因为「全枚举 180 件里
大量对铁甲战斗无效，抽进来只会得到恒 0 的死特征」——这个理由本身听着很硬）。
后果是**场景直接饱和**：3~5 件 → 教师 **85~92%**，胜负失去分辨度。

量化后的偏差：

| | 取值 | 池子总量 |
|---|---|---|
| Ironclad common | **12** | 33（36%）|
| uncommon | 2 | 30 |
| rare | 1 | 28 |
| **boss** | **1**（`COFFEE_DRIPPER`）| 22 |

- 而 **common 池里有 11 件（1/3）对战斗完全无效** —— 我一件都没要
- 还塞了 `COFFEE_DRIPPER`：那是**打完 Act 1 Boss 之后**才拿的 Boss 遗物，
  而且它恰好是单件最强（+70pp）
- 实测平均收益：手挑 **+28.9pp** vs 均匀抽样 **~+5pp** ⇒ **夸大约 6 倍**
- ⚠️ 另一个假信号：`NEOWS_LAMENT` 单件 +95pp —— 它**根本不在任何遗物池里**，是我强塞进去的
  （效果是「前三场敌人 1 血」⇒ 在我们的场景里等于自动获胜）。
  **异常大的单件收益要先去查「它是不是根本不该出现在这个场景里」**。

⭐ **结论：场景里的随机元素要抽引擎自己的池子，不要手写白名单。**
`GameContext::returnRandomRelic(RelicTier)`（已绑 `obtain_random_relic`）会顺手把抽到的
**从池里移除**（不重复），tier 混合也是真实游戏的。Potion 侧没有 `returnRandomPotion`，
就把 `PotionPool::potionPool[cc]` 给 Python 自己抽。

⚠️ 同类陷阱的通用形态：**任何「我把它筛成有用的那些」都会把消融变成自证。**
手写列表本身不是错 —— 错在拿它当**场景分布**用。筛出来的东西只能用于**诊断**。

### 🚨 快照里的「残留字段」—— 字段存在 ≠ 当前状态有意义（2026-09-21）

这一类跟下面的「预言机泄漏」是**同族**：都是**不该有值的时候有值**，于是变成历史指纹。
共同点：`_dump_battle.py` 一帧一帧打出来就看得见，而只看断言脚本（`_chk_snap.py`）永远发现不了。

### ① `cardSelectTask` **从不清零**（21 维 one-hot ⇒ 纯历史指纹）

`BattleContext::cardSelectInfo.cardSelectTask` 全仓 **13 处赋值、0 处清零**
（`src/combat/Actions.cpp` 11 行 + `src/combat/BattleContext.cpp` 2 行，全是 `= CardSelectTask::X`）。
选牌结束、回到正常出牌之后它**保持不变** ⇒ 余下每一步都在说「这一场曾经发生过哪种选牌」。

实测（seed 30000，`_dump_battle.py`）：

| 步 | inputState | cardSelectTask | 说明 |
|---|---|---|---|
| 0 | 1（正常） | 0 | 打 `Headbutt` |
| 3 | **2（CARD_SELECT）** | 10 | 真在选牌 |
| 13/14 | 1（正常） | **仍然 10** | `CardSelectTask(10) == HEADBUTT` ⇒ 残留 |

而 `azlayout.CAT_SPEC` 把它当 **21 维 one-hot** 喂进网络（`("card_select_task", [P_SELECT_TASK], 21, "onehot", False)`）
⇒ 训练时它能区分「这局打过 Headbutt 吗」，而真实决策不需要这个信息 —— 只会喂过拟合面。

✅ **已修（2026-09-21）—— 而且比“改引擎”便宜得多：`STS_GATE_SELECT=1`**

先把**真实范围**量准（这决定了要用哪种修法）：

| 选牌区的部分 | 非 select 帧里到底是什么 | 危害 |
|---|---|---|
| **候选块** 60×3 | **已经是全 0** | 无 |
| `candidateCount` | = **当前牌堆大小**（每帧重算）| 冗余（`counts` 块里已有）|
| `pickCount` / `canPickZero` / `canPickAnyNumber` | 残留**常量** | 无信息 |
| **`cardSelectTask`** | **残留**（如 `HEADBUTT` 一直到终局）| ⚠️ **唯一真正的泄漏**（21 维 one-hot）|

- 候选块为什么自己是 0：`appendCardSelectCandidates()` 只按 `cardSelectTask` 分支、**不看 inputState**，
  但它逐槽调 `isValidAction()`，而 `isValidSingleCardSelectAction` **开头就**
  `if (bc.inputState != InputState::CARD_SELECT) return false;` ⇒ 每槽都被涂成「非法」；
  而 `candidateCount = n` 照样计数 ⇒ 所以**有值但槽全 0**。
- ⇒ **残留只是观测问题，不是逻辑 bug**（`selectCard` 与动作枚举器同样只认 inputState）。

**修法**：`azlayout.GATE_SELECT`（`STS_GATE_SELECT=1`）——“非 `CARD_SELECT` 帧把整个选牌区
（候选块 180 + task 1 + meta 4 = **185 列**）置 0”，在 `AZNet._inputs` 里一次 `masked_fill`。
- 实测：非 select 帧里该区非零元素 **63 → 0**；select 帧**逐位未动**；`in_dim` 不变（仍 1230）；
  `az_boss.pt` 在 gate 开/关下都能 `strict=True` 加载；`state_dict` 仍 14 键（**没加参数**）。
- 差异只落在 **4 列**（`card_select_task` one-hot 的“从/到”两列 + `candidateCount` + `pickCount`）
  —— 候选块 180 列一个字未动 ⇒ 没越界。

⭐ **为什么在 Python 端做而**不**改 C++ 快照生产者**：改生产者 ⇒ 缓存里的 `obs` 就错了 ⇒
**得重采**（20k 场 @8000 sims 是小时级）；在编码器里做 ⇒ **缓存完全仍可用，只是权重要重训**
（分钟级）。相差约三个数量级。

⚠️ 但它**与 `PILE_POOL` 同型的坑**：不改 `in_dim` ⇒ 旧 `.pt` **仍能 strict 加载**，
只是输入语义变了 ⇒ **新旧权重不可比**。所以它进了存档配置（`_ckpt`）、进了训练表头、
进了 `_sync.sh` 的 `FWD`。默认**关**（`STS_GATE_SELECT` 未设）⇒ 走原路径、**逐位不变**。

⚠️ 还剩一个**只能改引擎**的：想让“复位”也修好 C++ 侧的语义（而不只是观测），
仍然要在选牌结束时复位 `cardSelectInfo` —— 那是另一件事，且要作废缓存。

### ② 死怪**不压实**且**仍带意图**

怪物块的三个槽位状态不能混：`id=0`（空槽）/ `id>0 & curHp=0`（**已死但仍在阵中**）/ 存活。
实测（seed 30000 的同一场）：`monsterCount=4` 而 4 个槽里 3 个已死；且 `id=52 curHp=0` **依旧报 `intentDmg=8`**。

- ⇒ **`monsterCount` 不是活怪数**；任何「总威胁」类特征/显示都必须按 `curHp>0` 过滤。
  （`_dump_battle.py` 已改成区分三态 + 单独打「存活总威胁」。）
- ⇒ 观测里同一行的 `curHp` 与 `intentDmg` **互相矛盾**。网络能学会「curHp=0 就忽略其它列」，
  但这是白送的困难 —— 同样属于「不该有值的时候有值」。

### 验证手法（通用）

找一个**只发生一次**的事件，看它的标志位**之后还在不在**：在就说明是残留字段。
再问一句：**这个字段在「本次没有该事件」时是不是 0**。这两问比读断言脚本有效。

### 🚨🚨 预言机泄漏：这类特征**测不出**价值（2026-09-13，本会话最重要的一条）

我们的评估（含所有 MCTS）都跑在 **clone 了整个 `BattleContext` 的引擎**里
⇒ **抽牌序列在搜索开始时就已经定死**。于是「**抽牌堆顶顺序**」对搜索而言是**预言机信息**。

- 后果：消融会告诉你「这个特征无害，甚至略有益」——
  **恰恰因为评估本身带着同一个泄漏**。它在部署（CommunicationMod）时值 **0 或负**。
- ⇒ **在带预言机泄漏的评估里，测不出「预言机特征」的真实价值。**
  这类特征必须按**部署语义**裁决（能不能拿到 / 机制上有没有用），不能按消融裁决。
- 本例的裁决：① discard 抽空会被**洗牌** ⇒ 顺序不进后继状态；② exhaust 是**单向出口**
  ⇒ 顺序不再被读；③ 只有 draw 堆顶有意义，而它**看不见**（除非 `Frozen Eye`）
  ⇒ 12 维里 8 维机制无用 + 4 维部署拿不到。**结论：改成 `PILE_TOP_K = 0`。**
- 配套纪律：**凡是"训练时能拿到、上线拿不到"的输入，都要单独列出来审一遍**，
  而不要指望指标发现它（本例该特征在 6 配置消融里全在噪声内，27.8~29.8%）。

### 推论：顺序类输入 = 「出牌历史指纹」
牌堆**顺序**等价于"该局怎么打到这里的"，可被网络用来反推 **battle 身份**
（≈ 变相泄露本局是哪个 seed）⇒ 纯过拟合面，对**未来**没有预测价值。
**只编码集合的内容与计数，不编码顺序**（除非机制真的读顺序）。

### 🚨 截断是静默的
固定布局里 `pileSlots`：超限时**堆底被丢**，而网络**无法区分**「就这些」和「下面还有」
（我们不喂整局牌组）⇒ 最贵的那类失败模式（静默信息丢失）。
- **触发条件 = `max(单堆) > pileSlots`**，**不是**「牌组 > pileSlots」
- 🚨 **31/41 的真实数字（2026-09-13 实测，训练种子段 0..999 / 1000 局·场景）**：
  原来 `pileSlots=30` 时 `boss` 最大单堆 **31**（1 个截断点）、`boss_wk` **41**（16 个）、
  `elite2` 恰好 30。**机制：史莱姆 Boss 的 `Goop Spray` 反复往抽牌堆塞 `Slimed`，分裂后继续塞。**
  ⇒ 📌 **「牌组只有 20 张所以堆不会超」是错的** —— 会**加牌**的遭遇战才是真风险，
  而 `slimes5`/`LOTS_OF_SLIMES` 就在场景集里。**必须实测，不能靠算术推。**
  ⇒ 已把 `pileSlots` 提到 **60**（重测 0 截断 / 余量 19）。
- 永久不变量检查：`_chk_trunc.py [每场景局数] [sims] [seed0]`。
  ⚠️ **默认扫的是 `EVAL_SEED0` 段，但采集用的是 `TRAIN_SEED0=0` 段 ⇒ 必须显式给 `seed0 0`**，
  否则你量的是"评估场景"，而训练数据被截断你根本不知道。
- ✅ 同类问题（选择候选块）**2026-09-14 已修**：`cardSelectSlots` 从写死的 30 改成
  **`= pileSlots`**（同一个常量）+ `static_assert(cardSelectSlots >= pileSlots)`；
  Python 侧 `SELECT_DIM` 从 `snapshot_layout()` **推导**。
  **症状**：`boss` seed 20495 `HEADBUTT` @ step 35，弃牌堆 33 张 ⇒ 第 33 个候选下标越界，
  抛 `ActionEncodingError`（1/7000 局）。**更坏的是 31/32 那个窗口**（`SELECT_DIM=32 > 30`）：
  动作下标**合法**但快照里**看不见**候选 ⇒ 网络盲选 + 标签静默污染，**范围守卫抓不到**。
  查它的脚本：`_chk_select.py`（`scan` / `cache` / `repro` 三模式）。

### 🚨 布局常量：字面量会静默错位
`bindings/slaythespire.cpp` 的 `Snap` 里的块偏移原来是硬编码字面量（`discardOff = 143`）。改 `pileSlots` 这类常量时
**字面量不会跟着变**，而错误的偏移**不会报错**，只会让网络读到错位的数据。
- ✅ 正确做法：偏移**全部写成由常量推导的表达式** + 在 `.cpp` 里加 `static_assert` 链
  （原本只断言了后半段 ⇒ 手牌～怪物段靠眼算）。现在改任何槽数常量，**编译一次就知道对不对**。
- ✅ `bindings` 里面向 Python 的 docstring 也改成 `std::to_string(常量)` 拼出来的
  （那块写死过一次，加怪物槽位后数字全过期而无人发现）。
- 📌 一般化：**凡是"两处各写一份同一个数"的地方，早晚会漂。** 要么推导、要么断言。

### 🚨 换实例 = `/root/autodl-tmp` 全空
AutoDL 的 `/root/autodl-tmp` 是**实例私有**的（只有 `/root/autodl-fs` 跨实例持久）。
换实例后实测：项目目录都不存在、`data/` 里所有 npz/pt 全丢。
- 新实例清单：推 submodule（`json`/`pybind11` **不在同步清单**）→ `cmake -S . -B build` →
  写 `site-packages/sts_lightspeed_build.pth` → `_chk_snap.py` → **重测教师哨兵**。
- ⭐ 好消息（前提是没有 `-march=native`/`-ffast-math`）：**教师哨兵跨实例逐位复现**
  （47.0% / 65.3% / 27.7 / 8.9）⇒ 绝对数字可沿用。**但必须实测确认，不能假设。**

### 🚨 「文档里写着有守卫」≠「代码里有守卫」
`_az_train.py` 的模块头自称：「seed 纪律 …… 两处会强制检查：`check_scenario_fp`（场景）与
`selfplay` 的 `start_seed`」。实测**只有第一处存在**，`selfplay`/`collect` 里**没有任何**
种子边界检查 ⇒ 自对弈/采集可以静默跑到 held-out 评估区间上，
**此后的所有评估都是在训练集上做的，而且不会报错**（胜率会“变好”一点，你会把它归因于训练改进）。
- ✅ 已补 `_assert_train_seeds(start_seed, n_battles, where)`，`collect`/`selfplay` 都调用；
  越界时抛出**可执行**的错误（提示 `STS_EVAL_SEED0` 上调）。
- ✅ 顺带把 `EVAL_SEED0` 改成可覆盖（`STS_EVAL_SEED0`）并加进 `_sync.sh` 的 `FWD`。
- 📌 一般化：**文档里的「会强制检查」「不会发生」这类说法，必须能找到对应的代码行**。
  找不到就补上，或者把文档改掉 —— 这个项目已经因为这个问题踩过两次（动作空间那次也是
  「以为有守卫其实没有」）。**补守卫之后要故意越界跑一次，验证它真的会拦。**

### 🚨 n=200 的非配对比较会骗人
同一个问题用两种仪器读，结论能反过来：
- n=200 非配对：`MCTS(20)` 上 det 比 stoch **好 3.5pp**、`MCTS(50)` 上 stoch 好 1.5pp
  ⇒ 看起来「互有胜负，说不清」。
- **n=1000 配对**（`_gen_eval.py pair`，McNemar）：MCTS(20) 两臂独赢场数 **96:96（p=1.000）**，
  裸胜率 p=0.750 ⇒ **完全一样**。
- 📌 规矩：胜率类结论**只用配对**（`pair`），且 **n ≥ 1000**。n=200 的 SE 就有 3.4pp，
  比要分辨的效应还大。

### 🚨 一个函数返回 0 会让 `&&` 链继续跑
`_sync.sh` 的 `run` 在 SSH 断线（rc=255）时**打印警告但仍返回 0** ⇒
`A && echo "===A 完成===" && B` 会把「A 失败」当成成功继续。
- 本次实测：`n1_stoch` 训练时报 `Connection reset by peer`，但作业其实**跑完了**（纪律再次应验），
  而 `&&` 链也照常继续 ⇒ 表面上「看起来都对」，但**是巧合**。
- 📌 判据永远看**产物**（`ls data/*.pt`）与日志摘要，不要看 shell 退出码。

### 🚨 「逐行」的数组被当成「逐场」用
`z`（以及 `pwin`）在一场内是**常数**、按**行**存（每个决策点一行）。
- ❌ `(z > 0).sum()` = **行数**，不是胜场数 ⇒ 我算出过「胜率 1500%」。
- ✅ 胜场 = `len(np.unique(bid[z > 0]))`；局均步 = `np.unique(bid, return_counts=True)`。
- 📌 一般化：拿到一个数组先问「**它的元素对应的是行还是场**」。这个项目里两者混在一起
  （`obs`/`pi`/`mask` 是行，`z`/`pwin` 是"行上的场常数"，`fp`/`space` 是全局标量）。

### 🚨 跨进程统计：配对要按**训练种子**配，不是按评估局
`_pile_ab` 的 5 配置共用同一批训练种子 ⇒ 每配置的裸胜率可以**逐种子配对**，
共同的种子方差被消掉。但**消不掉「两个网络各自的评估噪声」**。
- 实测 `k0_mean` vs `k4_mean` 的逐种子差是 **变号**的（+4.0/+3.0/−1.5/−1.0/+1.5/−1.0）
  ⇒ 均值 +0.83pp、SE ≈ 0.98pp ⇒ **n.s.**。**不要**把 +0.8pp 的正号读成"k0 更好"。
- ✅ `_pile_ab` 现在会打印配对 Δ ± SE + t 值和 95% 判定（`_t_crit` 手写，不依赖 scipy）。
- 📌 读法：**看区间，不看正负号**。要缩小区间就加训练种子（瓶颈是训练方差 ±2pp，不是评估局数）。

### 🚨🚨 **裸胜率是最不灵敏的读数** —— 本会话最大的方法论错误（2026-09-14）

整个会话的 A/B 几乎都用「裸网络胜率」当判据：编码类（k / pool / skip_empty / pileFields）、
覆盖类（随机自对弈）、数据量、步数 …… 结论统统是「零结果」。

2026-09-14 用了**四格单变量**重新测，同一批对比在两种读数下**结论相反**：

| 变化（2200 局配对，n=2000） | 裸胜率 | **MCTS(20)** |
|---|---|---|
| **4× 数据**（同步数 4400 步） | +0.5pp（p=0.581） | **+2.1pp（p=0.033 ✅）** |
| **4× 步数**（同数据） | +0.1pp（p=0.909） | **+2.9pp（p=0.003 ✅）** |
| 两者叠加 | +0.7pp（p=0.499） | **+5.0pp（p=0.000 ✅）** |

- 机制：**裸网络 = 策略头的 argmax，完全不用 value 头**。而这两个干预主要改善 **value**。
  ⇒ 用裸胜率测它们，等于**把要测的东西从读数里剔掉了**。
- 📌 **规矩**：凡干预涉及 **value / 搜索 / 目标函数**，判据必须是
  **`pair` 的 MCTS 胜率 + 交集局配对战损**，**不能**用裸胜率。
  裸胜率只对「**策略头容量/策略标签**」类干预灵敏（软化目标、策略头维度）。
- 📌 **推论**：以前所有「零结果」里，凡涉及 value 的都**必须重测**。
- 💡 旁证：`val_R2` 与 MCTS(20) 胜率**单调同向**（0.490→38.3 / 0.502→40.4 / 0.576→43.2）
  ⇒ `val_R2` 其实是**搜索强度的预测器**，之前写「val_ce/val_R² 是误导性指标」是**说过了**
  （那条结论来自「教师 500 vs 8000」的**标签质量**变化，与「同一教师下的拟合程度」是两回事）。

### 🚨 「同一批数据 + 不同 epoch」= 不同协议，不是同一模型

见上一条的姊妹错误：我用 `1k@8ep` 去比迁移前的 `1k@40ep` 基线，得到「布局把 1k 打掉 12pp」，
实际是**欠训 5 倍**。根因是原表里 1k 与 5k@8ep 是**刻意配成同步数**的（1k×40ep ≈ 5k×8ep ≈ 4400 步），
我照抄了「8ep」这个词却没照抄它的**意图**。

- 📌 **规矩**：比模型前先把两边的 `(缓存, epochs, train_threads, 目标口径)` **列出来对齐**。
- 📌 凡是「A 比 B 差很多」而机制解释不通 ⇒ **先怀疑协议不同**，别急着编机制。

2026-09-14：我改了布局（选择候选块 30→60 槽）后重训基线，拿 **1k@8ep（880 步）**
去和迁移前的 **1k@40ep（4400 步）** 比，得到 `val_R2 +0.379 → +0.160`、
胜率 **29.4% → 17.4%** —— 我据此写了「加宽把 1k 打掉 12pp」，**全是假的**：
那是**欠训 5 倍**，跟布局无关。而真正同 recipe 的 5k（−1.1pp）与 20k（+0.6pp）
说明布局代价 ≈ 噪声。

- 根因：原表里「1k ↔ 5k@8ep」是**刻意配成同步数**的（1k×40ep ≈ 5k×8ep ≈ 4400 步），
  所以 1k 的基线**本来就是 40 epoch**；我照抄了「8ep」这个词，没照抄它的**意图**。
- 📌 **规矩**：`_az_train.py train <缓存> <epochs> ...` 的第一个数字是**结果的一部分**。
  比模型前先把两边的 `(缓存, epochs, train_threads, target)` 列出来对齐。
- 📌 **更一般**：凡是「A 比 B 差很多」而机制解释不通时，**先怀疑 A 和 B 的协议不同**，
  不要急着给机制。本项目已经因为「隐性协议差异」踩过两次（`csw` 的 batch 硬编码）。
- 💡 副产品：这同时给出了「步数」维度的干净读数 —— 固定数据、**4× 步数**（20k@2ep → 20k@8ep）
  `val_R2 +0.502 → +0.576` 而胜率 **32.5% → 32.6%（p=0.909）**
  ⇒ **「拟合更好 ≠ 打得更好」第 4 次复现**。

### 🚨 `np.savez` 会偷偷给你加后缀

`np.savez("x.npz.new", **d)` 因为名字不以 `.npz` 结尾，**自己补成 `x.npz.new.npz`**。
于是「写出成功」但 `os.path.getsize(out)` 找不到文件 ⇒ 报 `FileNotFoundError`，
而磁盘上多了一个你没想到的文件（实测三个缓存都中招）。

- ✅ 修法：用**文件对象**写 —— `with open(out, "wb") as f: np.savez(f, **d)`
- 📌 一般化：**「写出去的路径」和「我以为的路径」必须核一次**（写完 `assert os.path.exists(out)`）。

### 🚨 「指标到达上限」不等于「被截断」 —— 得去查产生它的那个量的真实值

`candidateCount = min(容器张数, 槽数)`。看到 `candidateCount == 槽数` 时，可能是
「容器正好这么多」（**没截断**）也可能是「容器更大」（**被截断**）—— **靠这个数分不出来**。

- 我第一版检查直接把 `== 槽数` 判为「被截断」，于是对 20k 缓存下了「必须重采 50 分钟」的结论。
  实际去查 `counts` 块发现那一行**弃牌堆正好 30 张** ⇒ 根本没截断 ⇒ 可以**零成本迁移**。
- ✅ 判定要靠**独立的第二个量**（容器真实张数），不要靠被截断的那个量自己证明自己。
- 💡 更强的一路：缓存里的**标签**（`pi_sel`）如果从没落在「合法但不可见」的下标上，
  那是**铁证**（比 `candidateCount` 硬）。迁移时顺手就查了。

### 🚨 快照加宽的**零成本迁移**（纯插入）

改动落在快照**尾部最后一个块**时，加宽 = **纯插入** ⇒ 旧缓存可以就地迁移，**不必重采**。
`_migrate_obs.py` 做这件事，`check` / `run` / `inplace` 三模式。
⚠️ **前提必须证明**（新增槽位语义上为空），证不出来就拒绝 —— 见上一条。
⚠️ **标签数组也要迁**：`pi_sel`/`m_sel` 的宽度绑的是 **`SELECT_DIM`（动作空间）**，不是快照，
两者是**独立的两个迁移**。
⚠️ 从 `.bak` 一次性迁移才能拿到「旧槽数」这个信息（只迁 obs 会丢掉它，跳过最关键的那 2 列检查）。

### 🚨 「改一个字段看输出动多少」只能测**灵敏度**，测不了**信息价值**

`_probe_turn.py` / `_probe_hand.py` 这类探针很好用（零成本，直接回答「网络有没有在用它」），
但它们**分不出**下面两件事：

- 这个字段**携带信息**（改了它就损失精度）
- 这个字段**没被用上**，但网络**没见过这种输入** ⇒ 改了就是 **OOD**，输出**必然**变

实测（`_probe_hand.py`，20000 行留出集）：把手牌 10 个槽**置换**（语义等价的同一个局面），
值头 RMSE **0.4184 → 0.4402**（`flat`）/ **0.4267 → 0.4566**（`factor`）—— 看着像
「顺序携带信息」，其实**网络只在原始顺序上训练过**。⇒ 判「池化会不会损失信息」**只能训练 A/B**。

📌 **用法边界**：探针回答「**有没有东西可改**」（灵敏度非零 ⇒ 有），不回答「**改了会不会更好**」。
📌 副产品：`p[60:]` 与 `v` 同源于 trunk 输出 `h`，敏感度却差 20 倍（`flat_rest` 与
`value_head` 学到的权重不同）⇒ **不能用「某个下游输出没变」推断「上游没变」**，
要量哪一层就 hook 哪一层。

### 🚨 远端脚本现场用单引号拼串时，引号失衡会**伪造出正常画面**（2026-09-29）

`ssh host 'cmd1 ⏎ cmd2 ⏎ …'` 的远端执行体是 `bash -c <整串>`，而 bash 对 `-c` 字符串是
**逐条解析执行**：引号失衡时报 `unexpected EOF`（行号指向失衡那行），但**失衡之前的命令
照样跑完**。

⇒ 后果不是「全空」，而是**半真半假**：本次本地看板显示 `A 挡 98%` 有数据、`B 挡` 空白 ——
看着像「B 还没启动」，实际是脚本从第 7 行起全废。**第一帧就把错误画面渲染出来**，
不接 stderr 根本看不见。

✅ 防法：
- 看板/采集循环必须 **`2>&1` 抓进变量**再打印（本次正是靠它才在面板里看见 bash 报错）；
- 别每次现场拼长单引号串 —— **远端落一个 `.sh` 跑**（`_sync.sh push` 后 `bash f.sh`）：
  引号只出现一次，且能用 `bash -n f.sh` 静态检查。

### 🚨 `_sync.sh start` 的日志默认是**共用**的 `run.bg.log`；`status` 的进程列表会截断（2026-09-29）

① **日志共用**：`BLOG="${STS_BGLOG:-$RDIR/run.bg.log}"`（`_sync.sh:32`）⇒ 不显式给
`STS_BGLOG` 时，**每个新作业都往同一个 `run.bg.log` 追加**，把上一个作业的日志（含它的
`COMMAND_EXIT_CODE` 完成标记）与新输出混成一份。后果：旧作业的「已完成」证据被埋；
本地看板读 `run.bg.log` 的那一行语义错乱（实测：本以为是「A 挡 5000sims 结果」的那行
变成了训练进度条）。✅ 起作业一律显式 `STS_BGLOG=$RDIR/run.<会话>.log`。

② **`status` 截断**：`_sync.sh status` 的进程表是 `ps … | head -12`。16 个采集 worker 就能
把训练进程整个挤出列表 ⇒ 看着像「训练没起来」，实际在跑。**查进程别用 status**，
直接 `ps -eo pid,stat,pcpu,etimes,rss,args | grep <脚本名>`。

③ **配方队列陷阱**：`--device` 默认 `cpu`（`_env.py`：cuda 是「独立配方队列」）。
起训练前**先抄历史命令**（`--epochs/--device/--save`），否则会静默落到 cpu 队列 ⇒
与历史 cuda 结果**不可比**。本日即漏了 `--device cuda` 白起一次。

# ⑤ 计划（任务清单 / 优先级）

> ⚠️ **状态：搁置中（2026-10-01）** —— 这条「整局通关」线当前不推进（`_run_train.py` 一起搁置）；
> 下面内容**仍是有效方案记录**，重启这条线时从这里读。当前主线只看 ① 与 ② 的最近几条。

### 🎯 通往「整局通关」的任务清单（2026-09-21）

**目标**：从当前「单场（Act 1 Boss / 满血开局 / 固定 ~20 张卡组）」提升到**能打完整一局 Act 1–3 并通关**。

**先认清当前真实水平**
- 单场：`factor` 架构 **裸 39.8% / MCTS(500) 67.3%**（3 seeds × 200 局）—— 已**超过**教师 500 sims。
- 整局：`_walk.py` 实测**随机策略整局通关 0/200**；网络版能摸到 Act 2，**未训练网络的裸网络第 1 层就死**。
- ⇒ **单场强 ≠ 能走完一幕**。整局瓶颈不是「打不过某场」，而是**局面经济**（血量 / 卡组成长 / 资源）。

### 里程碑（每一步都要能测）
| # | 判据 | 现状 |
|---|---|---|
| M1 | 整局能走完 Act 1（不靠 `full_heal` hack），0/200 → 非零 | 卡死 |
| M2 | Act 1 通关率接近同协议教师 | 未测 |
| M3 | Act 2 精英接近教师（15%） | 裸网络 **1.0%** |
| M4 | 整局通关率 > 0 且可统计（≥5%，held-out seed 段） | 0/200 |
| M5 | 通关率接近新手人类（~10-20%） | — |

### P0 —— 机制缺口（不做这些，通关率在数学上恒为 0）
1. **卡牌奖励**：`_walk.py` 目前 `random` 选、`rlenv.py` 恒选选项 0。历史结论已写明：
   **跳过卡牌奖励 ⇒ 卡组永远 10 张 ⇒ 整局通关率 0% 是机制性必然**。需要「选牌」决策 + 主卡组进观测。
2. **篝火（REST_ROOM）**：现在恒 `choose_campfire_option(0)`（休息）。**升级（smith）** 是在大卡组里
   把关键牌翻倍的唯一来源 ⇒ 必须变成二选一决策。
3. **商店（SHOP_ROOM）**：现在直接把 `screen_state` 置回 `MAP_SCREEN` 离开（**购买接口没绑到 Python**）
   ⇒ 删牌/买卡/买遗物全部丢失。要么绑 C++ 购买接口，要么在任务书里明确「放弃商店」。
4. **事件（EVENT_SCREEN）**：必须**不进**（个别事件的个别选项是 `assert(false)` ⇒ C++ abort，
   Python 完全抓不到）。这条限制要不要解除，取决于是否愿意改引擎。
5. **遗物**：宝箱 / Boss 遗物现在恒跳过（`choose_boss_relic(3)`）。**能量遗物**是整局强度的主要来源之一。
6. **药水**：当前场景 0 药水、药水槽**没绑到 Python**（只有 `drink`/`discard` 两个动作）⇒
   96 维动作空间里 **[60,95) 共 35 维（36%）是结构性死区**。整局里药水是白送的战力。

### P1 —— 建模缺口（决定「能不能学会」）
7. **run-level 观测向量目前不存在**。`azlayout.py` 里记着跑图观测是 412 维（hp/gold/floor/boss
   one-hot/deck 计数/relics），**语义与战斗快照对不上**，不能直接接。需要新建：
   主卡组（集合+计数）、遗物、金币、当前/最大 HP、幕·层、下一层房间预测。
8. **跨场 HP 经济** —— 笔记里反复点名的**唯一**能表达「整局战损最小化」的路：
   现在训练/评估**每场满血开局** ⇒ 战后 HP 没有价值 ⇒「省血」只能当强度的代理。
   改法：run-level 的 return 用**终局存活**，而不是单场胜负。
9. **遭遇战覆盖**：训练数据只有 Act 1 Boss（1 怪）。缺口已量到 —— `elite2`（Act 2 精英池）**1.0% vs 教师 15%**
   （`_chk_enc.py` 可按遭遇战归因）。
10. **卡组分布外**：网络只见过 `_boss.build_deck` 的规格；整局卡组从 10 张长到 ~30 张且组成随机。

### P2 —— 工程与算力（决定迭代速度）
11. **进程级并行几乎线性（2026-09-21 实测）**：K=1 → **190.7 s/epoch**；5 进程并发 → **198.4~201.3 s/epoch**
    （**1.04×**，即每份几乎没变慢）。⇒ 要并行就在**任务层**铺进程（配置 × 种子），
    **不要开线程**（8 线程只 1.85× 且会把权重打漂，25/26 张量不同）。
12. 🚨 **但并发有平台天花板**：14 并发（每份 5.1~6.2 GB）⇒ 9 个被同时 SIGKILL，
    还连累了一个跑了 3 小时的主实验。`oom_kill 0` ⇒ 不是 OOM，是外部杀。
    内存预算：`memory.high = 64 GB` / 每份 ~5.5 GB ⇒ **并发 ≤ 10**。
13. **`torch.compile` 是整局自对弈的前置条件**：已实测 **4.03~4.66×**（`_bench_compile.py`）且不改数值；
    整局一局要几十场战斗、MCTS(500) ≈ 2 s/场 ⇒ 不做 compile 就跑不动自对弈。
14. **评估协议**：先建 `_walk.py` 的分层基线（random / teacher / 网络）＋固定 held-out run seed 段，
    否则「通关率涨了」无从谈起。注意：整局随机源要**每局独立播种**（`random.Random(seed)`）才可配对。

- ❌ **`STS_DROP_TRUNC`（丢弃撞步数上限的场次）—— 按预登记判据判定为否定（2026-09-21）**
  动机：撞上限的场次是**被静默截断**的，它们的 `z`（败局 −1）会把「本来可能赢」的场记成输。
  预登记判据：**3/3 同向 且 裸 > 3pp**，否则记为 null。
  实测（`run.ab_dt3b.log`，3 seed **并行**，污染 5 场 / 3916 行 = **0.562%**）：

  | | 裸 | MCTS(20) | MCTS(100) | MCTS(500) |
  |---|---|---|---|---|
  | 基线 `abf3`（同 20k / 8ep / `STS_SEED0`=0 / factor） | 39.8±2.1 | 51.0±0.4 | 57.8±1.7 | 67.3±1.5 |
  | `DROP_TRUNC=1` | 36.8±0.5 | 49.0±1.5 | 57.3±0.9 | 64.0±2.3 |
  | **Δ** | **−3.0** | −2.0 | −0.5 | **−3.3** |

  ⇒ **判据不成立（方向相反）**。污染只有 0.56% 行，本来就不该指望它动 —— 这是第 6 次
  「单场层面的调参不动棋」（前面：训练量 32ep、软目标、容量、牌堆编码、z 口径）。
  - 📌 顺带确认：**3 进程并行把这份实验从「3 小时没跑完 seed 0」压到 49 分钟**
    （3 seed 各 ~28 min，同时跑）。`STS_PAR_TRAIN=1` 这条路是通的。

### 下一步优先级（2026-09-20 更新）

> ⚠️ 本节是**单场层面**的优先级，仍然有效；但「整局通关」的路在上面那一节，别在本节里找答案。

✅ **2026-09-13 那版已基本做完**：`battle_split` 改按 seed、种子池扩到 20000、Act 2 tier +
卡组规格、z 口径阶梯、`c_puct` 扫描、第二道场景绊线。

⏳ **仍待办**
- **跨场 HP 经济（run-level 连战）** —— 唯一能表达「整局战损最小化」的路
  （当前每场满血开局 ⇒ HP 战后无价值 ⇒「省血」目前只能是强度的代理）
- `evaluateEdge` 归一化（要重编 C++，**作废全部绝对数字**；该文件对 1 ULP 敏感）
- ~~**噪声自对弈**（`_selfplay_battle` 改成按 π 采样 / 加 Dirichlet）~~ → ✅ **已实现并已否掉（2026-09-14）**：
  `STS_SELFPLAY_SAMPLE`（按 π 采样）与 `STS_SELFPLAY_EPS`（均匀 eps）两个开关都在 `_explore_idx` 里，
  默认关 = 逐位不变。`_sp_ab.py` 三臂（det / det 重复 / stoch）实测：**同一策略重跑自对弈，新状态数 = 0**
  （det 臂连跑两次 obs 行哈希集合**完全相同**）；而**更宽的状态分布并没有换来更强的棋**
  ⇒ **「覆盖是瓶颈」被否掉**。措辞已改为：**覆盖是「策略」的函数，不是「自对弈轮数」的函数。**
  （唯一仍未实现的是 **Dirichlet 根噪声**；但那属于「换策略」而非「多转轮」，与上面同向，优先级低。）

❌ **GPU 不值得**：训练只快 9%（17.6 万参数，单步被词度开销主导）；MCTS 前向的瓶颈是
**词度次数而非算力**，换设备救不了。⇒ 这个负载不是 GPU 形状的，要优化走 batching。

**工具速查**（2026-10-01 核实；旧表里的 `_gen_eval` / `_deck_var` / `_cmp_ckpt` 均已删）
```
_eval_walk.py     整局漫游胜率主评（分层基线 + 按幕/房间/怪群拆表 + ±CI）
_imit_probe.py    npz + ckpt 离线：KL / top1 / NLL硬 / 校准KL*（无引擎）
_probe_capacity.py 容量探针（固定数据 × 宽度 × seeds）
_z_revalue.py stats|zsplit                零搜索重放（z 只依赖终局）
_dump_battle.py   一场战斗的**全流程快照**（逐步按块解码 + 原始向量）
```

# ⑥ 附录（已作废方案 / 旧数字作废清单 —— 只在追溯时读）

> ⚠️ **本节提到的这些脚本都已在 2026-10-01 前删掉**：`_gen_eval.py` / `_cmp_ckpt.py` /
> `_bench_loop.py` / `_z_ladder.py` / `_deck_var.py` / `_chk_difficulty.py` / `_flags.py`。
> 读到时按「历史工具」理解；现行工具清单以 `.agent/SCRIPTS.md` 为准。
> 已作废子命令（`_sync.sh run` / `bg`）的机制描述已删，只留教训。

### 🔁 c_puct / 战损扫描与复现手法（2026-09-19~20）

- **判缓存是否作废：必须用逐位比对，不能用启发式**（`_chk_cache.py`，常备检查）。
  ❌ 我第一版是「怪物块 field0 全是 ≤65 的小整数 ⇒ 新布局」——**结论对，但论证不成立**：
  前提「旧 field0 = curHp」是从记忆取的，而 `git show HEAD` 里根本没有 `monsterFields`
  （`init` 提交时快照在别处）⇒ 旧字段顺序**查不到权威版本**。
  ✅ 决定性判据：**每场第 0 行 == 当前二进制在同 seed 上 `get_snapshot_np()`，逐位相同**
  （缓存记的是「动作前状态」⇒ 第 0 行 = 初始状态）。实测 1k 缓存 **10/10 一致**。
  与 `rew` 的自检互补：本检查管 obs **布局**，`rew` 管 `pi_*` 的**动作语义**。
  守卫自检的坑：「交换两场的行」测的是**行错位**（种子 0/1 同怪不同卡组，差异在 counts/draw/hand），
  **不是布局**；真正要防的用例是「把怪物块 field0 写成 curHp」→ 抓在 `monster_off`。
- ✅ **终局列改为随 `collect` 落盘**（2026-09-19）：`_az_train.terminal_stats(b)` 是唯一一份计算，
  `_collect_battle`/`_selfplay_battle` 返回 4 元组，`_assemble` 可选写出 `t_*` 五列
  （**要么全有要么全无**，混着写会得到长度对不上的静默错）。
  `_z_revalue.terminal` 改成包一层（只换键名）。⇒ **新缓存不需要 `rew`**，`rew` 只用于升级旧缓存。
- 🚨 **训练步速随数据集大小塌缩 2.7×，别拿小数据外推**（2026-09-19）：
  batch=256、同模型同线程，1k 缓存（34.7k 样本）23.8 步/s，20k 缓存（696.9k 样本，
  `obs` 2.15 GB）只有 **8.8 步/s**（42 ms → 114 ms/步）。原因是每步要从大数组里 gather，
  缓存/页表局部性崩。⇒ 「12 min/网络」的估计错了 2.4 倍（实际 **28~29 min**），
  把 1.7 h 的阶梯说成了 5 h。**排计划前先跑一个 seed 量一下**（第 1 个网络存盘就有数）。
- 🐛 **`_sync.sh` 三个坑（2026-09-20 修了两个）**：
  - `_newest_log` 原来只认 `$LOG`/`$BLOG` 两个固定名字，而 `bg` 作业各用 `STS_BGLOG`
    （`run.abf3.log` / `run.diag.log` …）⇒ **把早就结束的老作业报成当前作业**
    （实测把昨天的 z-阶梯报出来了）。已改成扫 `run*.log` 取最新，并回**绝对路径**
    （调用方拿到 `$lf` 后不去 `$RDIR` 就直接 `[ -f ]` / `tail`）。
  - 🚨 **`./_sync.sh bg ... | tail -N` 会挂住，而且作业可能根本没起来** ——
    `tail` 是汇、要等 EOF，而 `setsid` 那行不关 ssh 通道 ⇒ 双双卡住；实测那次 kill 掉之后
    远端**没有任何进程**（`setsid` 那行没执行到）。提交 `bg` **不要接任何管道**。
    （这条是「长任务别接 `tail`」的第二次踩，第一次在 `run` 上。）
  - ⚠️ `run` 也一样别接管道：`./_sync.sh run _diag_head.py ... | sed | head` 全程无输出，
    因为 `head -N` 也会提前关掉读端。要看输出就**不接管道**（`_prog` 是 TTY 感知的）。
- 📊 **`lossshape` 与 `old` 分辨不出来**（20k/8ep/3seeds，seeds 20000+）：
  MCTS(100) 53.5% vs 52.3%（σ1.3/2.8）、MCTS(20) 46.7 vs 45.2、裸 33.8 vs 34.5 ——
  差值全在种子间 std 内。⚠️ `lossshape` 的 `val_R2` 反而更高更稳（+0.63~0.66 vs +0.27~0.61）
  而胜率无差异 ⇒ 又一次「val_R2 是误导性指标」。
- 🎯 **z 口径阶梯完整结果**（20k/8ep/3seeds，评估 seeds 20000+；高阶档用
  `_z_ladder.py eval` 拿已存权重重评，不重训）：

  | 口径 | 裸 | MCTS(20) | MCTS(100) | MCTS(500) |
  |---|---|---|---|---|
  | `old` | 33.8±1.2 | 46.7±2.2 | 53.5±1.1 | **64.8±0.9** |
  | `lossshape` | 34.5±1.8 | 45.2±2.7 | 52.3±2.8 | **65.0±2.7** |
  | `engine` | 33.2±1.2 | 43.0±1.1 | 48.7±1.2 | **61.5±1.5** |

  - **`lossshape` 零结果，500 档仍零**（+0.2pp，1/3 同向）⇒ 我猜的「低预算杠杆小」**被证伪**：
    改**败局分支形状**不动搜索强度，这个负结果在预算上是稳的。
  - **`engine` 6/6 同向更差**（100 档 −4.8pp、500 档 −3.3pp）⇒ 「value 目标换哪个都一样」是**错的**。
  - ⚠️ 但 `engine` 的解释被卡住：它把**输赢落差** 1.34→0.79，而 `q = W/N` **未归一化**
    ⇒ 改尺度 = 偷偷改探索强度。**先扫 `c_puct` 才能解释**，不能读成「这个目标更差」。
  - 📌 站得住的判断：①「拟合更好」≠「搜索更强」（5 次复现）；②改**形状**不动强度；
    ③改**尺度**会动强度且目前**总是变差**（`hpw` 两个方向 + `engine`）—— 但是探索代理效应。
    ④搜索本身很强：裸 33.8% → MCTS(500) **64.8%**（+31pp），且胜过教师 500 sims 的 41.5%。
- 🔬 **c_puct 扫描推翻了 `engine` 的负结果，并挖出「c 是省血旋钮」**（2026-09-20，
  `_gen_eval.py csw <net> 200 boss 500 0.25 0.5 0.9 1.5 2.5`，MCTS(500) 200 局 seeds 20000+）：

  | c | `old` 胜率/战损/赢局剩血 | `engine` 胜率/战损/赢局剩血 |
  |---|---|---|
  | 0.25 | 62.5% / 54.2% / 36.7 | **64.0%** / 56.7% / 34.7 |
  | 0.50 | 63.5% / 58.8% / 33.0 | **65.0%** / 58.1% / 33.5 |
  | 0.90 | 63.0% / 62.7% / 29.8 | 58.0% / 62.0% / 30.4 |
  | 1.50 | 63.5% / 66.1% / 27.2 | 61.0% / 70.2% / 23.9 |
  | 2.50 | 62.5% / 71.0% / 23.3 | 58.0% / 70.1% / 24.0 |

  - 🚨 **`engine` 的 −3.3pp 是尺度混淆**：它在低 c 下**反而略好**（65.0% @ c=0.5）。
    ⇒ 关于「`engine` 这个 z 目标更差」**一个字都不能说**，那条从表里划掉。
    同时这也解释了 `hpw` 为什么也「失败」—— 两者都动了**尺度**，而 `q = W/N` 未归一化。
  - **`old` 的胜率对 c 几乎平**（62.5~63.5，全在 1pp 内）⇒ 我们没在次优 c 上跟人比。
  - ⭐ **c_puct 是「胜率—省血」旋钮，不是胜率旋钮**：两个网络**同向单调** ——
    c 越小 → 战损越低、赢局剩血越多、回合越多，而**胜率几乎不动**。
    `old`：c 1.5→0.25 战损 **66.1%→54.2%**（−11.9pp）而胜率 63.5→62.5（噪声内）；
    `engine`：70.2%→56.7%（−13.5pp）而胜率 61.0→64.0。
    ⇒ 若目标是省血，**c 从 1.5 降到 0.25~0.5 近乎零成本**。与历史「8000→32000 sims 只买血」
    是同一根轴；也与旧笔记「N0 的最优在更小的 c（40.5%@1.5 > 34.0%@3.15）」一致。
  - ⚠️ **与教师的结论不矛盾**：教师是 UCT + 随机 playout + `evaluateEdge` 归一化坏掉
    （c=3√2 最优、调小是灾难），两套搜索的 c 语义不同。
  - ⚠️ 待确认：战损差 12pp（战损 SE≈2.1pp）看着很硬，但项目纪律是「跨胜率的战损必须用配对」。
    这里胜率基本相等所以聚合比较相对可辩护，**要下结论应加大 n 或用配对**。
- 🎯🎯 **`c_puct` 是「胜率—省血」旋钮（n=1000 钉死）**（2026-09-20，`_gen_eval.py csw
  data/zl_old_s0.pt 1000 boss 500 0.1 0.25 0.5 1.5`，MCTS(500)，seeds 20000+）：

  | c | 胜率 | 战损 | 赢局剩血 | 全局剩血 | 回合 |
  |---|---|---|---|---|---|
  | 0.10 | 64.6% | 65.1% | 27.9 | 18.0 | 11.0 |
  | **0.25** | **65.8%** | **58.3%** | **33.4** | **22.0** | 11.0 |
  | 0.50 | 65.5% | 60.7% | 31.4 | 20.6 | 10.7 |
  | **1.50**（默认） | **65.6%** | **68.4%** | 25.3 | 16.6 | 10.3 |

  - **胜率四档全平**（64.6~65.8，n=1000 的 SE≈1.5pp ⇒ 分辨不出来）
  - ⭐ **战损差 10.1pp**（68.4% → 58.3%），战损 SE≈0.9pp ⇒ **~10σ**；赢局剩血 **+8.1**
  - ⚠️ **曲线是内点最优，不是单调**：`c=0.1` 战损反弹到 65.1% ⇒ 最优在 **0.25~0.5**。
    （n=200 只扫到 0.25，看不到拐点 —— 我一度说成「越小越好」，是过度外推。）
  - **排除「打得慢所以多挨打」**：c=0.25 回合更多（11.0 vs 10.3）却总战损更低
    ⇒ 每回合伤害 5.30%/turn vs 6.64%/turn（少 ~20%），不是时间混淆。
  - ⇒ **这是目前最便宜的省血杠杆**：不用重训、不用加搜索预算，只改一个搜索常数。
  - ⚠️ 待验：① 只在 `zl_old_s0` 一个网络上测过。
  - ✅ **「只有 MCTS(500) 测过」已补齐（2026-09-20，n=1000，`csw data/zl_old_s0.pt 1000 boss <sims> 0.25 1.5`）**：

    | sims | c | 胜率 | 战损 | 赢局剩血 | 回合 |
    |---|---|---|---|---|---|
    | 20 | 0.25 | 42.8% | 76.1% | 19.1 | 10.3 |
    | 20 | **1.50** | **46.0%** | 76.2% | 19.1 | 9.0 |
    | 100 | **0.25** | **56.9%** | **67.2%** | 26.3 | 10.6 |
    | 100 | 1.50 | 52.5% | 73.0% | 21.6 | 9.7 |
    | 200 | **0.25** | 60.4% | **62.6%** | 29.9 | 10.7 |
    | 200 | 1.50 | 59.6% | 71.7% | 22.6 | 10.1 |

    🚨 **c 的最优随预算移动，`sims=20` 是唯一的例外且方向相反**：
    20 档 `c=0.25` **胜率 −3.2pp**（战损相同）；≥100 档 `c=0.25` **胜率不降反升**（100 档 +4.4pp）
    且战损大幅改善（−5.8 / −9.1 / −10.1pp）。
    ⇒ **`c=0.25` 只在 MCTS(100) 以上用**；主评估阶梯的 20 档仍应用 1.5。
    机制：低预算下 q 估计噪声大，硬利用 = 锁定烂线（与教师那边的结论同源：「均匀 ≠ 坏」）。
  - ⭐ **c 与 sims 在「省血」轴上是互补而非替代**：20→500 档，
    `c=0.25` 战损 76.1→**58.3%**（−17.8pp），`c=1.5` 只 76.2→68.4%（−7.8pp）。
    胜率回报也略大（+23.0pp vs +19.6pp，500 档收敛到同一点 65.8/65.6）。
    ⇒ 要动「省血」这个目标，**降 c 的收益比加预算更大**（且零成本）。
  - ⚠️ **与教师的结论不矛盾**：教师是 UCT + 随机 playout + `evaluateEdge` 归一化坏掉
    （c=3√2 最优、调小是灾难），两套搜索的 c 语义不同。
- ⚠️ **别在长作业跑着的时候另起评估**：实测会抢满 16 核，MCTS(50) 掉到 0.5 局/s，读数不可引用。

### ⚠️ 旧数字作废清单
- **换编译器 / 换卡组 ⇒ 全部绝对胜率作废**（跨机器不可比：同源码教师 MSVC 46.2% vs gcc 44.8%，
  搜索对浮点混沌敏感；换卡组把网络/教师比从 25% 推到 44.4%，不是普适常数）。
- ⚠️ **评估种子段也是分母的一部分**：seeds 20000+ 上教师 500 sims 只有 **40.0%**，
  seeds 0..999 则是 62.3% ⇒ **比较强弱必须同时限定「种子段 + 教师预算」**。
- ✅ **换机器：两边都复现（2026-09-21 实测，已证到位）** —— 我先前那条「换机 ⇒ torch 侧不复现」
  **是错的**，错在拿 `_bench_loop.py` 的 `loss` 探针当判据：那个探针在**计时分段里跑 `net.train()`**，
  dropout 会**消耗 RNG** ⇒ 探针值随**脚本结构**变（v1/v2 之间就变过），根本不是可复现性信号。
  **真正的判据是比存盘的权重**（新工具 `_cmp_ckpt.py`）：
  - `ab_factor_s0.pt`（旧机）vs `base_westc_s0.pt`（新机）⇒ **26 个张量，0 个不同**
  - `abf3_s0.pt`（旧机）vs `base_westc_s0.pt`（新机）⇒ **26/26 逐位相同**
  - 旁证：新机跑出的 `factor@8ep seed0` 是 **42.0 / 51.5 / 55.5 / 69.5**、`val_ce 0.714`，
    与旧机 `ab_factor` 那一行**完全一致**；教师哨兵两台机都是 **41.5%**。
  - ⇒ **旧机的全部基线在新机上有效，不需要重建。**
  - ⚠️ 唯一量到的跨机 FP 差异是**裸 `Tensor.sum()`**（315k 元素，差 1~2 ULP）—— 而**模型路径上没有这个算子**。
  - 📌 **判"某改动是否复现"的正确手法：跑两次、比 `.pt`**（不是比胜率，也不是比 bench 探针）。
    顺手把 bench 的副作用修了：计时分段改跑 `net.eval()`，探针只反映训练循环。
- ⚡🚨 **吞吐不要跨机/跨时比 —— 共享宿主机上单线程能漂 2~3×（同一天内实测）**
  - `_bench_loop.py`（300 批 ≈ 10 秒窗口）说新机**更快**：单线程 31.5~31.7 ms/批
    （旧机 36.2）、8 线程 17.0~17.8（旧机 24.15）。
  - **但同一台新机的真实 8 轮训练**每 epoch 实测 **85 → 139 → 134 → 168 → 213 → 220 → 243 s**
    （累计 1m25s / 3m44s / 5m58s / 8m46s / 12m19s / 15m59s / 20m02s / 27m47s），
    而旧机是 **~85~102 s/epoch** ⇒ **真实训练慢 1.6~2.7×，且同一轮里越跑越慢**。
  - 旁证：跑的时候宿主机 `load average` 从 13.6 升到 **16.75** ⇒ **邻居在吃核**。
    `cpu.max` 只是**配额**、不保证独占物理核。
  - ⇒ 「新机更快」是 **10 秒窗口的假象**。**`_bench_loop.py` 只适合同一台机、同一时间、
    交替着比**线程数/配置**；拿它做跨机吞吐结论是无效的（这也是为什么线程那条要把 1,8,1,8 交替测）。
- **机器指纹**（写死在这，免得再搞混）：
  - 旧机 = westb `:44969`，EPYC 9654，**32 核**配额，内存 377 GB。
  - 新机 = westc `:32328`，Xeon Platinum 8352V（Ice Lake），**16 核**配额
    （`cpu.max=1600000 100000`），内存 503 GB。torch 两机同为 **2.8.0+cu128**。
  ⚠️ 新机是**克隆**过来的：2.7 GB 缓存、全部 `.pt`、`run.*.log`、已构建的 `build/*.so` 都在。
  **缓存里是"旧机教师"产的标签**（教师搜索对浮点混沌敏感）⇒ 沿用它是**有意的**（省 1~2 小时重采），
  但要知道这一点：学生在新机上评估、标签来自旧机教师。
- **闭环（自对弈迭代）的胜率 / 战损数字全部作废**：那是 2026-09-13/14 的协议 —— 种子池
  **只有 0..999**、策略头是 `flat`（`factor` 晚一周才有）、选择候选块还是 30 槽，且评估脚手架
  在那之后重构过。**结构/代码事实仍成立**（见「闭环」那节的横幅）：`selfplay` 与训练侧格式
  互通、默认 `argmax` ⇒ **覆盖是「策略」的函数而非轮数的函数**、软目标已测否、泄漏已修。
  ⇒ **引用闭环收益前必须在当前协议（20k 池 / `factor` 头 / 现布局）上重跑。**
- **仍成立（与场景无关）**：`val_ce` / `val_R²` / `train_ce` 是误导性指标，只看胜率；
  熵 / 一致率类指标必须跨状态采样；**均匀 ≠ 坏**；长任务别接 `| tail`。

### 🚨 起长作业前先确认「通知通道」存在

⚠️ 说过两次「跑完会自动收到通知」，两次都是**用户催**才去查的 —— 因为当时的启动方式
（`setsid nohup`，已删）把作业与 ssh 会话**解耦**，完成时没有任何通道回来。

- 现行做法：`./_sync.sh start`（远端 tmux 守护）+ 本地 `./_sync.sh wait <日志>` **阻塞轮询**
  ⇒ 轮询本身返回时才叫「有通知通道」。
- ⚠️ **别 kill 掉那个超时被移入后台的终端** —— 那正是唯一的通道。
- ⚠️ 估等待上限要留余量（例如 3 seeds × 46 min ⇒ 一轮 105 min 不够，**补一轮是正常的**）。

### 🗂 发牌表换到 A20 口径，**两套卡组形态（big/lean）删掉**（2026-10-02）

- **用户定调**：「扔掉大小卡组设计」+「修改卡牌/遗物曲线，基于 A20 的数据调整」。
  `devshape.py` 从 `VARIANTS`/`VARIANT_P`/`variant_of` + `DECK{形态}`/`REMOVALS{形态}` 收回**单套**
  （`DECK`/`RELICS`/`UPGRADES`/`REMOVALS` 四条元组），`scene.build_shaped_deck`/`shape_floor`
  去掉 `variant` 形参（三个调用点本来就没传），`_chk_shape` 的双形态检查删掉、发放池检查顺位上来。
  160 行 → 134 行。
- **口径**：`_winrarity.py --asc 20` 的**胜利局**终局分布。牌组末段 = 白 5.63 / 蓝 9.71 /
  金 4.81 / 特殊 1.51 / 诅咒 0.13（合计 26.73 张/局）、遗物 20.70 件/局。
  ⚠️ 只有终局一个锚点 ⇒ **段间形状**是按上一版（上游 bot A0 实测阶梯）**等比缩放**出来的，
  不是实测。要真正的逐层曲线，得先让 `_winrarity.py` 能按层切（它现在只算终局）。
- **实测核对**（采集口径 A20、200 seed、floor 45）：牌组 26.09（目标 26.73）、遗物 20.36（20.70）、
  初始 4.79（4.95）、白 5.87（5.63）、蓝 10.04（9.71）、金 4.78（4.81）、特殊 0.61（1.51）、诅咒 0。
  差的 ~0.6 张全在特殊牌那桶（`RED_POOLS` 没有 SPECIAL 档）。
- ⚠️ **遗物总数修了一个 off-by-one**：旧 `relic_total(段)` 名义含初始那件，但 `grant_shaped_relics`
  用的是 `总数 − act + 1` 再加 Boss 件数 ⇒ 实发 = 表值 + 1。现在 `relic_total` 就是**总数**，
  `n_drop = 总数 − 1(初始) − n_boss` ⇒ 实发 == 表值。
- ⚠️ **遗物从 ~10 件/局抬到 ~20.7**（实测值的两倍，不是三倍）⇒ 战斗整体变简单。
  旧值来自「上游 bot 1000 局 A0」—— 那个 bot 98.7% 死在 act1/2，幸存者只得 ~9.5 件，被低估了。
- ⚠️ 顺手发现（**未改**）：`walk.ASCENSION = 0` 而 `scene.ASCENSION = 20` —— 漫游/评测那条线
  与 `collect.make_stage_battle` 用的难度不同（A20 初始牌组多 1 张 `ASCENDERS_BANE`，
  稀有度是 `SPECIAL` 而不是 CURSE；`max_hp` 75 vs 80）。要统一得单独决策 + 重采。
- 缓存失效：`collect.check_cache_obs` 用 `seed`/`enc`/`enc_floor` 重放场景逐位对账 ⇒ 换表
  **自动**挡掉旧缓存，不需要升任何版本常量（`WALK_VER` 那套已经随库化删掉了）。
- 验证：`_chk_shape.py 3` 全通过（①–⑥）；`coverage_walk.py` 只读 `STAGE_EDGES`，不受影响。

#### 上一节的两个决定被推翻（同日，用户追问）

- **「诅咒恒 0」错。** 用户的理由：「你不加诅咒怎么训练牌效识别？」—— 0.13 张/局意味着 ~8 局
  才见 1 张，网络几乎见不到死牌，学不出「牌组里多一张废牌 ⇒ 值更低」。改成末段 0.8 的阶梯
  （覆盖优先于保真，实测 0.72）；`RED_POOLS[CURSE]` 是 Clumsy/Decay/… 那 10 张，
  事件渠道还没做 ⇒ 只有数量对得上、身份是随机的。
- **「删牌不碰进阶之灾」也没做到。** `build_shaped_deck` 里那句 `rng.choice(整个牌组)` 会把
  进阶之灾一起删掉（floor 45 只剩 57% 的局有它）。"三、
  我第一遍又改过头了（把删牌池收成 `is_starter_strike_or_defend`，连**痛击**一起保护了）
  —— 用户的原话只是「删牌不删进阶之灾」，已改成「牌组里除进阶之灾外的任意一张」。
  后果：痛击 48% 的局被删（末段只剩 0.52 张），初始牌 4.49（目标 4.95）。
- **进阶之灾的掉队：不要手写概率**（用户定调「#BANE_KEEP 不要这个，改成收集时直接随机
  进阶等级」）：`BANE_KEEP`（先写成 6 段内插表、再改成常数，两次都被否）已删；改成
  `scene.ascension_of(seed)` 逐场抽进阶等级，`collect.make_stage_battle` 用它建 GameContext
  ⇒ `ASCENDERS_BANE`（与 max_hp）由**引擎按等级**给，不再有第二套隐式来源。
  ⚠️ 现在三条线的难度口径不同：采集 = 随机抽（`ASCENSION_RANGE = (0, 20)`），
  漫游/评测 = `walk.ASCENSION = 0`，整局 = `rlenv.ASCENSION = 0`。
  ⚠️ 进阶等级也不再是固定 A20 ⇒ 牌组曲线（A20 胜局实测）叠在**混合**难度上，
  特殊那列随抽法变（`(0,20)` 时 `ASCENDERS_BANE` 出现率 55.8%）。

#### 特殊牌 + 特殊遗物接进发放池（同日，用户指示）

- **用户定调**：「把其他可以进牌组的特殊牌和特殊遗物也加进来」。枚举：`CardRarity::SPECIAL`
  共 20 张，能进铁甲牌组的只有 **4 张事件牌**（Apparition / Bite / Ritual Dagger / J.A.X.）
  —— 其余是别角色的衍生牌（Insight / Smite / Shiv / …），诅咒色那 3 张由对应遗物带。
- 新增：`scene.SPECIAL_POOL`（4 张，抽到就往牌组里塞）+ `devshape.DECK` 第五列（末段 0.95）
  、`devshape.SPECIAL_RELICS`（末段 1.11）+ `scene.SPECIAL_RELIC_POOL`（17 件）。
- 实测（随机进阶等级、400 seed、floor 45）：特殊牌 0.62 → **1.56**（实测 1.51 ✓）；
  特殊遗物 0 → **1.00 件**（实测 1.11）；牌组 26.38 → 27.23。
- ⚠️⚠️ **`obtain_random_relic(RelicTier.SPECIAL)` 会段错误**（引擎那张池没内容，不是抛异常）。
  特殊遗物只能按 id 显式发：池 = tier 表的 SPECIAL 减去 `NEOWS_LAMENT`（涅奥起点）与
  `CIRCLET` / `RED_CIRCLET`（奖池耗尽的填充件，`GameContext.cpp::returnRandomRelic` 兜底）。
  BOSS / SHOP / COMMON 的 `obtain_random_relic` 正常（返回 bool）。
- ⚠️ 引擎的 `obtainRelic` **不执行拾取效果**：NECRONOMICON 不会带进 Necronomicurse、
  CALLING_BELL 不会带 Curse of the Bell ⇒ 3 张诅咒色 SPECIAL 卡实际拿不到。
- ⚠️ 还没做：**商店档（实测 1.27）**没渠道 ⇒ 被摊进掉落池，末段白 8.5 vs 实测 7.16。
- 可执行性扫描（一次性探针）：4 张特殊牌 + 20 件特殊遗物逐个塞进牌组/持有后打一场，
  全部无异常；`NILRYS_CODEX` 会在回合末出选牌屏，但引擎枚举成 `SINGLE_CARD_SELECT`、
  我们的 SELECT 头能编码 ⇒ 不列禁用。`_chk_shape.py 1` 全通过。

#### 遗物按档分级 + `SPECIAL_POOL` 并进 `RED_POOLS`（同日）

- **用户指示①**「现在的遗物没有稀有度分级吗？加上完整的分级」⇒ `devshape.RELICS` 从「一条
  总数阶梯」换成**六个档各自的阶梯**（白/蓝/金/Boss/商店/特殊），末段分别对齐 A20 实测的
  7.16/5.08/3.14/2.24/1.27/1.11；总数由分档表派生（`relic_total` = 1 + Σ）；`scene` 六个档
  各自抽件数，不再让引擎的 `returnRandomRelicTier` 定档。实测末段：
  白 6.90/蓝 5.13/金 3.06/Boss 2.14/商店 1.38/特殊 1.01 ✓（之前白 8.5 偏多）。
- **用户指示②**「`#SPECIAL_POOL` 为什么不加入 `#RED_POOLS`」⇒ 并成第五档
  （`RED_POOLS[SPECIAL] = SPECIAL_POOL`）。原来分开是因为那档是**手挑**的（SPECIAL 全是
  无色，哪些能进铁甲牌组推不出来），而 `RED_POOLS` 是从引擎里**推**的；但那个 dict 对诅咒
  那档本来就已不「红」了 ⇒ 合并后发牌只剩一条五档循环，另加长度闸（`DECK_TIERS`）。
- ⚠️ 换档位发放暴出一个**之前够不到**的洞：`TOOLBOX`（商店档）开局三选一无色牌，引擎给
  `CARD_SELECT`（**能编码**，与 `GAMBLING_CHIP` 不同）⇒ 按「开局必须是 `PLAYER_NORMAL`」
  的口径列入 `BANNED_RELICS`。要放开就删它 + 放宽 `_chk_shape` ⑥。
- ⚠️ 排查中踩到两个引擎坑（都不在发放路径上，但要知道）：
  · `obtain_random_relic(RelicTier.SPECIAL)` **段错误**（那张池没内容）；而且把**任何**档
    的池抽空（`returnRandomRelic` 走到 `CIRCLET` 兜底）也会段错误 ⇒ 探针不能抽干池。
  · `THE_SPECIMEN` 在任何一只怪死掉时 `SetState(SELECT_ENEMY_THE_SPECIMEN_APPLY_POISON)`，
    而**没人消费**这个状态 ⇒ Debug 下 `enumerateActionsForNode` 直接 assert。但它
    **不在任何角色池里**（池共 130 件 vs 档位成员 156 件）⇒ 发放路径拿不到，不必禁。
- 可执行性扫描（真实池口径）：五档 130 件逐件「持有 + 打一场」，零异常。
- 修完后末段（floor 45，400 seed）：牌组 26.64、遗物 20.32、初始 4.62（目标 4.95）、
  白 5.67（5.63）、蓝 9.98（9.71）、金 4.79（4.81）、特殊 0.85（1.51）、诅咒 0.72（0.13 超采样）。
- ⚠️ **分桶纠错**：我上一节写「进阶之灾在真实 StS 里算诅咒、两边不同桶」是**错的** ——
  `_winrarity.py` 是用**我们的** `Cards.h` 给日志里的牌定档的，那份表里它就是 `SPECIAL`
  ⇒ 实测那 1.51 **含**它，两边同桶。旁证：全难度「特殊」0.81 < A20 的 1.51，差值≈低难度没它。
  正确的账：实测 1.51 = 进阶之灾 ≈ 0.8 + 事件无色牌 ≈ 0.7；我们 0.85 ≈ 只有前者。
- ⚠️ 副作用：删牌改成只删打击/防御后，**BASIC 反而更低了**（4.79 → 4.62，目标 4.95）——
  因为以前删牌有时落到痛击/进阶之灾头上，现在每一删都打在打击/防御上，再叠加 Boss 遗物的
  转化 ⇒ 缺 0.33。要贴到底得把 `REMOVALS` 末段从 5.05 降到 ~4.7。
- `_chk_shape.py 2` 全通过；`evaluation` 与采集同口径仍为待办（`walk.ASCENSION`）。

### 🐞 缺格挡上限 999 ⇒ `int` 溢出 ⇒ 壳寄生兽吸血量为负（2026-10-03）

采 `SHELL_PARASITE@20` 时 SIGABRT（`src/combat/Monster.cpp` 的 `heal` 断言）。链路：

1. `Actions::EntrenchAction` 是 `block += block`（翻倍）。Entrench 在真实 StS 里**不是**
   Exhaust（查 `desktop-1.0.jar` 的 `Entrench.class`：构造函数无 `exhaust = true`）⇒ 反复
   打出是**合法**行为。我最初「引擎漏标 Exhaust」的判断是错的。
2. 真正缺的是**格挡上限 999**：`Player::block` / `Monster::block` 都是 `int`，翻倍 32 次
   即溢出变负（实测 `1106621912 + 1106621912 = -2081723472`）。
3. `Player::attacked` 的 `damage = calculatedDamage - savedBlock`，负 `savedBlock` 把伤害
   抬成正 20 亿（实测 `damage=2068070791 block=0 calc=7`）。
4. `lastAttackUnblockedDamage` 是 `int16_t` ⇒ 窄化截断为 `-32335`。
5. `Actions::VampireAttack` 拿它当吸血量 ⇒ `Monster::heal(-32335)` ⇒ 断言。

**修复**：`constants/Misc.h` 加 `MAX_BLOCK = 999`，施加在**四个增加格挡处**：`Player::gainBlock`、
`Monster::addBlock`、`Actions::MonsterGainBlock`（原为直接写 `arr[idx].block += amount`，绕过
`addBlock`，已改走封装）、`BattleContext` 的 `R::ANCHOR`（原为直接 `p.block += 10`）。
`int16_t` 保持原样 —— 上限 999 后该字段装得下。`Monster::heal` 断言恢复原状（期间我一度把它
改成 clamp 到 0 + 一次性警告，那是**掩饰**，已撤）。

- 本地 build **不定义 `sts_asserts`** ⇒ 断言被编译掉，本地「不崩」不能当证据；必须在远端验。
- 引擎一改，数据与引擎的逐位一致性（`check_cache_obs`）就要重验。`23@20`（= `ENCS` 表第
  23 项 `SHELL_PARASITE@20`）删掉重采后，全量校验 **63 项 × 1000 场通过**（1109747 行；
  修复前 1109745 行 —— 那 2 场的战斗都在 `MAX_STEPS=1000` 处收场，长度几乎不变但路径已变）
  ⇒ 新上限**只波及这一项**，其余 62 片分片可原样复用。
- **弯路**：先猜「漏标 Exhaust」、又反编译 jar 取全局 Exhaust 清单，都属假设驱动的长链探索；
  其实在 `gainBlock` 里打一行「`block` 超过 1e6 就打印」，第 2 步即可定位。副产物：引擎确实
  漏标 3 张 Exhaust 卡（`CRIPPLING_POISON` / `SEEING_RED` / `TERROR`；`IMPULSE` /
  `UNRAVELING` 是未实装的废弃卡）—— 与本次崩溃无关，属独立数据 bug，待修。

#### 补漏标 Exhaust：只补 `SEEING_RED` + `TERROR`（同日）

按用户指示「标上」：`doesCardExhaust` 的 `return true` 组补进 `SEEING_RED`（红、罕见）、
`TERROR`（绿、罕见）。

- ⚠️ **判据修正**：先前拿「jar 的 Exhaust 清单 − 引擎 `doesCardExhaust` 里的 case」当「漏标
  清单」，它没排除**引擎根本没有这张卡**。`CRIPPLING_POISON` 不在 `CardId` 枚举里（Silent 卡，
  未实装）却被判成漏标，编译时才暴露；`IMPULSE` / `UNRAVELING` 同理。
  ⇒ 正确做法：先在 `CardId` 枚举范围内核对该卡存在，再判是否漏标。
- ⚠️ **已知债务**（用户知情，先不重采）：`SEEING_RED` 是**红卡**，teacher 的牌组会发到它
  ⇒ 标上 Exhaust 之后，「打出过 See Red」的战斗与**已采数据**不再逐位一致
  （`check_cache_obs` 将来会失败），而重训正是用这批数据 ⇒ 那部分数据带的是旧语义。
  要清这笔账得重采含该项的分片。

#### ✅ 整局（run）线退役（2026-10-06，用户：「把 run 系列内容删除了，这些东西已经过于过时」）

删 7 个文件（2524 行）：`runenv.py` `rundata.py` `runlayout.py` `runnet.py` `runpolicy.py`
`collect_run.py` `train_run.py`；同批退役 `multi_hot.py`（整局网专用，全仓零 import）、
`_chk_bossrelic.py`（候选空间随 `runlayout` 走）。

- ⚠️ 这批文件**未被 git 跟踪** ⇒ 删除不进任何提交，`git log`/`git status` 都看不出回退过。
  备份在仓库外 `d:/tmp/sts_run_series_backup_1006/`（9 个文件原样）；`bak1006` 分支里另有
  一版，与之差 2~10 行（全是 `rlenv`→`runenv` 改名注释）。
- `_chk_res.py`：摘掉「全局侧逐槽」节（原第 0 节：`runlayout`/`runnet` 口径对拷）+ `import
  multi_hot`；其余 4 节守**战斗侧** `azlayout`，保留。改完跑过：**24 通过 / 0 失败**。
  `_chk_all.py` 清单同步摘掉「Boss 遗物候选」一项。
- **保留**（名字带 run 但是活件）：`runtime.py`（`collect_encs`/`collect_selfplay`/`train`/
  `eval_encs`/`metrics` 的缓存命名与 `DATA_DIR`）、`walk.py`（`eval_encs.py` 取 `_init_worker`/
  `make_choose`/`parse_spec` 的地方，`_chk_shape.py` 也用）。
- 顺带清理：`sync.sh` 的作业正则里 `train_run`/`probe_imit`/`probe_capacity` 三条替代名对应的
  脚本都已不在 ⇒ 正则改成 `(collect_[a-z]+|train|eval_walk|watch)`；6 处指向退役模块的注释也
  改了（`azlayout` `aznet` `ckpt` `runtime` `walk`×2 ⇒ 「已于 2026-10-06 随整局线退役」）。
- 复核：`ls rundata.py runenv.py runlayout.py runnet.py runpolicy.py collect_run.py train_run.py
  multi_hot.py _chk_bossrelic.py` 应全部「No such file」；`grep -rn 'runlayout\|runnet\|runpolicy\|rundata'
  --include=*.py .` 只应剩注释里的历史提法。
