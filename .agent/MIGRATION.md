# 迁移清单（迁到高性能环境）

## 结论先行

- **数据可以丢，知识不能丢。**
- 数据缓存（`data/*.npz`）全部可再生产；而且 2026-09-13 卡组规格变更后，**旧缓存本来就全部作废**。
- 真正贵的是 `.agent/`：那里有踩过的坑、被实验推翻的假设、以及必须遵守的复现规矩。
  其中 **Copilot 的记忆文件原本不在工作区里**（在 VS Code 的 workspaceStorage），
  已导出到 `.agent/memory/`，详见该目录下两个文件开头的说明。

## 要带走 / 不要带

| 带走 | 说明 |
|---|---|
| `src/ include/ bindings/ apps/ CMakeLists.txt` | C++ 引擎与 pybind11 绑定 |
| 根目录 `*.py` | `train.py` `aznet.py` `azsearch.py` `azlayout.py` `runtime.py` `collect_selfplay.py` 等 |
| `pyproject.toml` `uv.lock` `.python-version` | 依赖与解释器版本（`requires-python >= 3.14`） |
| `json/` `pybind11/` | **是 git submodule**。整目录拷贝即可；用 `git clone` 则必须 `--recursive` |
| `.agent/` | 笔记 + 记忆导出（**最贵的东西，务必带走**） |
| `Card_data.csv` `check_card_data.py` | 卡牌数据与检查脚本（`test.py` 已于 2026-09-21 退役到 `_old/`） |
| `_old/` | 已退役脚本。⚠️ 根目录 `.py` **未被 git 跟踪** ⇒ 只能**移**不能删 |

| 不要带 | 原因 |
|---|---|
| `build/` | MSVC 生成，绝对路径写死为 `D:\tmp\sts_lightspeed` |
| `.venv/` | 绝对路径 + 平台绑定，`bin/` vs `Scripts/` 也不同 |
| `__pycache__/` `*.pdb` | 运行/构建产物 |
| `data/*.npz` | **可丢弃**（已在 `.gitignore`） |

## 目标机器上重做

```bash
uv sync                                     # 依赖（venv 内没有 pip，用 uv）
cmake -S . -B build && cmake --build build --config Debug --target slaythespire
python -m pybind11_stubgen slaythespire -o build/Debug   # 构建后再刷桩
python _chk_all.py                          # 守卫总检（engine 档，秒级）
python _profile_state.py                    # 状态空间体检
```

## ⚠️ 必须在新机器上重测一切

`train.py seeds` 输出里 `教师 500sims` 那行是**基准哨兵**。它确定性，但**依赖二进制**：
本机把 `3*sqrt(2)` 换成差 **1 ULP** 的字面常量后重编，它就从 24.0% 跳到 31.5%（400 局差 30 局）。

换机器必然重编，而且编译器可能从 MSVC 变成 GCC/Clang —— 浮点行为必然改变
→ **所有绝对胜率数字作废**。

迁移后第一件事：

```bash
python train.py seeds <新采的缓存> 1 200    # 记下「教师 500sims」作为新基准哨兵
```

之后任何「跑 A → 改代码 → 跑 B → 比 A/B」的实验，中间**不能重编**；非改不可则 A 要重跑。

## 重采基线的配方（照这个做，结果才可比）

```bash
# 1) 采集（按怪组表逐项，每项一个子进程 ⇒ 可并行）
python collect_encs.py 100 500
# 2) 训练 + 多种子评估
python train.py seeds data/encdata_n100_s500.npz 3 400 20
```

必须记住的固定量（写死在代码里，改动等于换任务）：

| 量 | 值 | 位置 |
|---|---|---|
| 卡组规格 | 5 白 + 3 蓝 + (1-2) 金 + (0-1) 诅咒；均值 20.08 张 | `_boss.build_deck` |
| 场景指纹 | `scenario_fp()` 前 8 个场景的 SHA1 | `_boss.py`；`train()` 会比对 |
| 训练 / 评估 seed 基 | `TRAIN_SEED0 = 0` / `EVAL_SEED0 = 1000`（不重叠） | `train.py` |
| ⚠️ 另一个 seed 基 | `BOSS_SEED0 = 30000`（`_teacher_diag.py` 用它） | `_boss.py` |
| 教师预算 | 500 sims（默认） | 命令行 |
| 网络超参 | `card_rank=8, monster_rank=8, intent_move_rank=8, hidden=128, dropout=0.2`，Adam 1e-3 / wd 1e-4 / 40 epoch / batch 256 | `train.py:REG` |
| 快照布局 | 1111 维，唯一事实来源是 C++ 头文件 | `bindings/slaythespire.cpp` 的 `struct Snap` |

## 旋钮：**没有环境变量**（2026-09-30 定案）

配方（架构 / 优化 / 口径 / 线程 / 设备 / 打点）全是**命令行 flag**，由各脚本自己的 argparse
解析后一路传给库函数（`collect_jobs(workers=…)` / `train(threads=…, device=…, tb=…)`）。
`runtime.py` 只剩纯工具函数（`workers(v)` / `mp_context(mp_start)` / `cache_path(name, dir)` / …），
**没有任何读取点**；唯一还读 env 的地方是 `train._BGLOG`（`STS_BGLOG`，作业日志路径，
由 `sync.sh` 注入 —— 不是配方）。整局（run）线已于 2026-10-06 退役（`train_run.py` 等已删）。

不常变动的量就直接写成**模块级常量**（例如评估段起点 `collect.EVAL_SEED0`、并行度常数
`train.PAR_TRAIN`），不要包装成旋钮。

| flag | 默认 | 说明 |
|---|---|---|
| `--workers` | 按 cgroup 配额 | 并行 worker 数；`serial`/`0` = 关闭并行（逐位复现路径）。**大机器上设成物理核数**（GIL 型负载吃不到超线程） |
| `--mp-start` | 自动 | `fork`/`forkserver`/`spawn`。Linux 上 `fork` 让建池开销归零（也是 `seeds` 并行训练的**前提**：缓存靠写时复制共享）；fork+OpenMP 卡死则用 `spawn`。⚠️ **`collect_encs.py` / `collect_walk.py` 没有这个 flag**（2026-09-30）：那里的并发是**线程池**（launcher 只阻塞等子进程），与 multiprocessing 无关 |
| `--cache-dir` | `<repo>/data` | 缓存目录。**以前默认写 `%TEMP%`，导致缓存不在工作区里** |
| `--threads` | **1** | torch 线程数。**它是配方的一部分**（换线程数就换轨迹，实测 `max|Δw|` 0.65~0.89；跨进程只在**同**线程数下逐位相同 —— 见 `.agent/memory/sts-lightspeed-rl.md` 续十三）；守卫 `_chk_threads.py` |
| `--epochs` / `--batch` / `--lr` / `--wd` / `--hidden` / `--depth` / `--arch` / `--sel-arch` / `--dropout` | 64 / 256 / 1e-3 / 1e-4 / 128 / 2 / flat / flat / 0.2 | 训练配方（`train.py`）。**默认值只在 `train()` 签名里一份** |
| ~~`--pair`~~ | **已删**（2026-10-02） | 交叉矩阵（`FactorPolicyHead` 里的 `key ⊙ target`）**常开**、不再是开关。⚠️ 它只在 `arch="factor"` 里存在，`flat` 臂不受影响；旧存档的 `config` 留着 `pair` 也能加载（`net_kwargs` 打一行警告后忽略） |
| `--z-spec` | `enginenorm` | 值目标口径：`enginenorm`/`hp`/`hp_turn`（定义在 `zspec.py`；`engine`/`lossshape`/`hpw` 已删）。`enginenorm` = **引擎自己的终局分**（`sts.evaluate_end_state`）÷ `100*max_hp` ⇒ 赢 ≈0.48~1.24、败 ≈0；引擎落差 0.477（`hp` 是 1.0+）⇒ 搜索退化先扫 `c_puct`。**缓存不存 `z`**：一律由终局列现算（`zspec.z_of`），换口径只是换个 flag |
| `--soft-target` | 关（硬 argmax） | 策略目标口径（四个场景上四次更差，见 notes） |
| `--device` | `cpu` | 训练设备；`cuda[:k]` = 逐 batch 上卡 |
| `--tb` / `--tb-dir` | 开 / `data/runs` | TensorBoard 打点（训练曲线 + **故障告警**，见 `tb.py`）：`tensorboard --logdir data/runs`。打点是**纯旁路**（实测同一配置权重 sha 不变）；`tensorboard` 没装或 `--tb 0` ⇒ 只留 `runs/faults.jsonl` |
| `--job-timeout` | 1800 s | 看门狗阈值（日志 mtime 静默判定）；`0` = 关 |
| `--seed0` / `--save` | 0 / 不存盘 | `seeds` 的种子起点与存盘模板（模板要带 `{seed}`） |

⚠️ **评估段起点 `EVAL_SEED0`（= 1e8）是 `collect.py` 里的常量**，不是 flag：`eval_walk.py` 拒绝低于它的
`seed0`（被训练见过的种子不能当评估）。训练种子池的上界就是它。

## 大机器上的并行现状

| 环节 | 状态 |
|---|---|
| 采集（C++ 教师） | **已并行**（逐局独立）。这是原来唯一卡住 re-baseline 的地方 |
| 评估（裸网络 / PUCT） | **已并行**，与串行逐位一致（`_mp_test.py` 验证） |
| 训练 | 单进程（torch 多线程）。样本上万后可考虑 DataLoader 并行 |

`batch_size>1` 的批量叶评估能把单核成本再降 ~3×（917→306 µs/sim），
但它**会实质改变搜索行为**（同一次运行里 batch=32 给 16.5%、batch=64 给 36.5%），
所以要用必须单独重测下游胜率。`batch_size=1` 与旧实现逐位等价（`_parity_test.py` 30/30）。
