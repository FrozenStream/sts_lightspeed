# 远端环境（seetacloud）

## 连接

```bash
ssh -p 48068 root@connect.westb.seetacloud.com    # ← 现役（2026-09-30 起，纯 CPU / 32 核）
```

⚠️ **旧实例 40282 已释放**（2026-09-28 白天：ssh 超时）。
⚠️ **2026-09-28 晚复核：两个实例都连不上** —— `-p 34549` 超时、`-p 40282` 拒绝
（复核：`ssh -o ConnectTimeout=8 -p <PORT> <HOST> true`）⇒ 当时本地有**一批文件没推上去**：
`train.py`、`collect_walk.py`、`collect_dagger.py`、`.agent/memory/sts-lightspeed-rl.md`，
加上 2026-09-28 的**单文件存档改造**（新建 `ckpt.py` + 14 个读侧文件）
（其中远端那份 `collect_dagger.py` 还带失效的 `ROLLOUT_THREADS` import，一跑就 ImportError；
远端 09-25 后的权重若还在，存档格式要从两文件迁到单文件 —— 跑 `python ckpt.py <档>` 看，
旧格式会自己报 ⚠️，`python -c "import ckpt;ckpt.migrate('data/xxx.pt')"` 就地迁移，权重逐位不变）
⚠️ 这些全是**纯 Python 改动 ⇒ 不用重编引擎**。
⇒ **实例一起来第一件事是 `./sync.sh push`**；若是全新盒子，先走本节下面「换实例」那套。
下面各表里「旧实例 / 新实例」的对照按此读。

⚠️ 换实例/重启后端口会变，域名也会变（历：`westc` → `weste` → `westb`，差一个字母，别拄错）。
复核一条：`ssh -p <PORT> <HOST> '/root/miniconda3/bin/python -c "import torch;print(torch.__version__)"'`
`sync.sh` 顶部的 `PORT`/`HOST` 默认值要跟着改，或用 `STS_HOST`/`STS_PORT` 覆盖。
（2026-09-30：默认值已改成现役的 `westb:48068`。）
⚠️ 换实例 = `/root/autodl-tmp` **全空**（代码、`build/`、`data/` 全没）。重来一遍的顺序：
`sync.sh push` → 单独 tar 推 `json/`+`pybind11/`（不在同步清单）→ configure+build → 写 `.pth`。
⚠️ **可选依赖 `tensorboard`**（`pip install tensorboard`）：只影响训练打点（`data/runs/`），
不装就只有警告 + `data/runs/faults.jsonl`，训练与守卫全不受影响。

## 规格与工具链

| 项 | 值 |
|---|---|
| OS / 内核 | Ubuntu 22.04.5 / 5.15 |
| CPU | 宿主机 `nproc=128`（假象），**cgroup 实测 16 核**（2026-09-28 复核：`/sys/fs/cgroup/cpu.max = 1600000 100000`，两个实例一致；旧记 12 核已推翻）⇒ `STS_WORKERS=16` 即可；**超订无益**（实测 64 workers 吞吐与 16 相同，~2.3 局/s，瓶颈 = 配额）。旧实例曾记 16 核。⚠️ **2026-09-30 现役 westb**：cgroup **32 核**（`cpu.max = 3200000 100000`），同款 `Xeon Platinum 8352V @ 2.10GHz`（单核仍比本地慢 2.5~2.8×）|
| 内存 | 随实例变：旧 **1007 GB**、新 **503 GB**（`free -g`，2026-09-28）；**westb 251 GB**（2026-09-30）|
| GPU | 两个旧实例都是 **RTX 4080 SUPER 32 GB**，但本项目用不到（CPU 口径）。⚠️ **westb 是纯 CPU 机**（`torch.cuda.is_available()==False`，虽装的是 `+cu130` 轮子）|
| 编译器 | gcc/g++ **11.4.0**（C++17 ✓）|
| cmake / make | 3.22.1 / 4.3（**无 ninja**）|
| Python | **conda base `/root/miniconda3/bin/python` = 3.12.3** |
| 已装包 | 旧实例：torch **2.8.0+cu128**、numpy 2.3.2；新实例：torch **2.12.1+cu130**、numpy 2.4.6（两个实例都装了 gymnasium / pybind11_stubgen）。⚠️ CPU 数值：**2.8 与 2.12 逐位相同**（见 `.agent/memory/sts-lightspeed-rl.md` 续十三）|
| 关键路径 | 项目 `/root/autodl-tmp/sts_lightspeed`（**2026-09-23 实测 `df -h /root/autodl-tmp`：`/dev/md0` 50 GB、可用 ~46 GB**——旧实例记过 195 GB ⇒ 换实例后先 `df` 复核，万局 npz ≈ 7 GB 要预留）|
| ⚠️ 空间注意 | 系统盘 `/` 只有 **30 GB**，所以 venv/构建都放 `autodl-tmp`，别用 `/root` 根目录装大包 |

`.condarc` 用的是清华镜像；pip 也建议加 `-i https://pypi.tuna.tsinghua.edu.cn/simple`。

## 已经做过的适配（本地代码里也保留了）

| 改动 | 原因 |
|---|---|
| `CMakeLists.txt` 里 `/EHsc` 加了 `if(MSVC)` 守卫 | 那是 MSVC 专有 flag，GCC 会 `unrecognized command-line option` 直接报错 |
| `pyproject.toml` `requires-python` 3.14 → **3.12** | 远端只有 conda 的 3.12 |
| `runtime.py` 统一缓存目录/worker 数/mp 启动方式 | 见 `.agent/MIGRATION.md` |

## 模块搜索路径（和本地不同的地方）

本地靠 `.venv/Lib/site-packages/sts_lightspeed_build.pth` 指向 `build/Debug`。
远端同样用 `.pth`，但指向单配置的 `build/`：

```
/root/miniconda3/lib/python3.12/site-packages/sts_lightspeed_build.pth
  内容: /root/autodl-tmp/sts_lightspeed/build
```

## 构建

```bash
D=/root/autodl-tmp/sts_lightspeed; PY=/root/miniconda3/bin/python; cd $D
cmake -S . -B build -DPython_EXECUTABLE=$PY -DCMAKE_BUILD_TYPE=Debug     # ⚠️ 必须 Debug，理由见下
cmake --build build -j 32 --target slaythespire        # 全量首编 ~1 min（32 核）
$PY -m pybind11_stubgen slaythespire -o build          # 构建后刷桩（远端没 venv ⇒ 这一步手动）
$PY _chk_all.py                                        # 守卫总检（engine 档）
```

⚠️ **构建配置 = Debug（2026-09-30 定案，别改回 Release）**：`include/sts_common.h` **无条件**
`#define sts_asserts`，但那些 `#ifdef` 块体里写的是 **C 的 `assert`** ⇒ Release（`-DNDEBUG`）下
全仓自检**静默失效**。更危险的是两处「断 assert = UB」——
`enumerateActionsForNode` 的 `default:` 与 `playoutRandom` 的 `edges.empty()` 分支，后面都紧跟
`uniform_int_distribution(0, size-1)`。改 Debug 后全绿，且代价为零（见下条）。

产物：`build/slaythespire.cpython-312-x86_64-linux-gnu.so`（LTO 开启）。

### 换实例清单（2026-09-30 westb / 32 核纯 CPU 实测）

```bash
D=/root/autodl-tmp/sts_lightspeed; PY=/root/miniconda3/bin/python
HOST=root@connect.westb.seetacloud.com; PORT=48068        # 已是 sync.sh 默认值
./sync.sh push                                            # 166 个受管文件
tar czf - json pybind11 | ssh -p $PORT $HOST "mkdir -p $D/data && tar xzf - -C $D"
tar czf - data/run_mapstats_smoke.npz data/res_prior.pt | ssh -p $PORT $HOST "tar xzf - -C $D"
ssh -p $PORT $HOST "cd $D && cmake -S . -B build -DPython_EXECUTABLE=$PY -DCMAKE_BUILD_TYPE=Debug"
ssh -p $PORT $HOST "cd $D && cmake --build build -j 32 --target slaythespire"    # 全量首编
ssh -p $PORT $HOST "echo $D/build > /root/miniconda3/lib/python3.12/site-packages/sts_lightspeed_build.pth"
ssh -p $PORT $HOST "$PY -m pip install -q -i https://pypi.tuna.tsinghua.edu.cn/simple pybind11_stubgen tensorboard"
ssh -p $PORT $HOST "cd $D && $PY _chk_all.py"              # 期望 15/15
```

⚠️ 换实例只有才踩得到的四条：

1. **`data/` 里有两份守卫依赖文件不在同步清单里**（`data/` 整体不同步）：
   缺 `run_mapstats_smoke.npz` ⇒ `整局回报层` + `整局网络` 红；缺 `res_prior.pt` ⇒ `整局网络` 红。
2. ⚠️ **千万别用 `Release`**（理由与实测见上面「构建」节）：`-DNDEBUG` 会把引擎全部 `assert`
   编译掉 ⇒ ① `_chk_encs.py` 那条前提检查不成立（`_chk_all` 恒 14/15）② `enumerateActionsForNode`
   的 `default:` / `playoutRandom` 的空边分支变成 **UB** 而不是响亮崩。
3. **`.pyi` 自动刷新在远端不会发生**：`POST_BUILD` 找 `<repo>/.venv/bin/python`，远端没 venv ⇒
   只打一行 CMake 警告。不影响运行（Pylance 只在本地用）。
4. **worker 数不用手设**：`runtime.workers()` 读 cgroup 配额 ⇒ 新实例自动得 **32**
   （`sync.sh` 里那个 `STS_WORKERS` 是死旋钮，真正传参用命令行 `--workers N`）。

#### ⭐ Release ≡ Debug 逐位相同（2026-09-30 实测）

同参数采同一批（`collect_encs.py 5 500`，315 场 / 4405 帧），两种构建产物产出的缓存：
**17 列全部 0 差异** —— `obs` 7,061,215 项、`pi_act`/`pi_sel`、`m_act`/`m_sel`，连 `t_escore`
这个 double 列也逐位相同；总胜率都是 **67.0%**、耗时都是 8 s；自对弈 32 worker 下
0.21 s/局（Release）vs 0.19 s/局（Debug，噪声内）。
⇒ 本仓引擎的 `-O0` 与 `-O3+LTO` 在这类负载上**不产生浮点分歧**（无 fast-math、无 FMA 收缩），
**换配置不需要重采数据**。
⚠️ **但跨编译器不适用**：本地 MSVC 与远端 gcc 的教师胜率是 44.8% vs 46.2% ——
「远端多一个二进制就多一条配方队列」的纪律只针对编译器/机器，不针对 `-O` 等级。

## 同步本地改动到远端（`sync.sh`）

**工作流：本地改 → 推送 → 远端跑。本地是唯一真相来源，单向推送。**

```bash
./sync.sh              # 等价 check：比对两端 md5，列出漂移（有漂移则退出码 1）
./sync.sh push         # 推送全部受管文件
./sync.sh push _queue.py       # 只推指定文件
./sync.sh start <脚本.py> [参数...]    # 先推再在远端 tmux 起（STS_SESSION 必填；`run` 子命令已移除）
```

受管文件由 `sync.sh::managed()` 通配决定（当前 **157** 个：根目录 `*.py`/`*.sh` + `CMakeLists.txt` + `pyproject.toml` + `.agent/**/*.md` + 四个自研 C++ 目录）。
`build/` `pybind11/` `json/` `data/` **不参与同步**，远端自己构建。
⚠️ 换实例后 `json/` + `pybind11/` 必须**单独**推（它们是 submodule，不在同步清单里）：

```bash
tar czf - --exclude=.git json pybind11 | ssh -p 48068 root@connect.westb.seetacloud.com \
  'mkdir -p /root/autodl-tmp/sts_lightspeed && tar xzf - -C /root/autodl-tmp/sts_lightspeed'
```

⚠️ `sync.sh run` 只能接**一个**字符串参数（内部会自己切词）。别写成多个 shell 参数。

底层仍是 tar 流（本地没有 rsync）。只推一两个文件时直接点名更快：

```bash
tar czf - collect_walk.py | ssh -p 48068 root@connect.westb.seetacloud.com \
  'tar xzf - -C /root/autodl-tmp/sts_lightspeed'
```

### 仓库记忆的位置

Copilot 的仓库记忆存在**本机的 VS Code workspaceStorage** 里，不在仓库内：

```
%APPDATA%\Code\User\workspaceStorage\<workspace-hash>\GitHub.copilot-chat\
    memory-tool\memories\repo\*.md
```

`.agent/memory/*.md` 是**权威副本**，随 git 走 —— 不需要任何导出/导入步骤。
（记忆工具那边只剩一个指针，就在上面这个位置，不随仓库迁移。）

## 关机（省机时）—— 2026-09-25 实测

**容器里 `shutdown` 真的能用**，不需要去控制台点按钮：

```bash
shutdown -h now        # ← 用这个
```

- PID1 是 AutoDL 自己的 `bash /init/boot/boot.sh`（不是 systemd），`CapEff` 里也没有
  `CAP_SYS_BOOT` —— **但照样能关**（init 接管了）。别被 "Failed to talk to init daemon" 的
  常识误导（那是 systemd 版 `shutdown` 的行为）。
- ⚠️ **别用 `shutdown -h +N` 延迟形式**：实测 `+10` 当场就切断了所有 SSH 会话，
  于是**来不及** `shutdown -c` 撤销（本人 2026-09-25 就这样误关过一次实例）。
- 关机前先把产物 `close` + `sync`：会话一断，后面的输出就再也不会有地方可读。
- 完成判据也**不能**依赖 `script(1)` 写在日志尾部的 `COMMAND_EXIT_CODE`（`shutdown`
  会把它吹掉）⇒ 包装脚本要**自己把退出码打进行**（见 `collect_train_off.sh` 的 `[pipeline] rc=`）。
- 重启后 **SSH 端口不变**（2026-09-25 关机→开机，40282 仍是 40282）；但换实例/迁移仍会变。
- 持久盘 `/root/autodl-tmp` 关机不丢数据 —— 日志与权重下次开机直接读。

## 性能实测

| 任务 | 本地（8 物理核） | 远端（16 核配额） | 加速 |
|---|---|---|---|
| C++ 构建（slaythespire，全量） | 分钟级 | **~1 min（32 核）** | — |
| 采集 1000 场 @500 sims | 1310 s | **58 s** | **22.6×** |
| 教师评估 400 局 @500 sims | ~600 s | 十几秒（已并行化）| ~40× |
| **自对弈 16 场 @sims=200**（Python PUCT，每 worker 单线程） | 12.0 s（16 worker） | **4.5 s（16 worker）/ 3.0~3.3 s（32 worker）** | 2.7× / 3.8× |
| 同上，吞吐 | 4800~8100 局/h | 12780 局/h（16 worker）/ **17375~19407 局/h（32 worker）** | 2~3.6× |

⚠️ 自对弈这一行说明一件事：**本地并行效率只有 ~25%**（单决策串行 106 ms，16 worker 实测只降到
73 ms/决策），所以尽管远端单核慢 2.5~2.8×，**总吞吐反而更高**。自对弈/`c_puct` 扫描这类
Python 侧负载**放远端**；本地只做小规模摸底。
⚠️ 任何自对弈基准都要先 `torch.set_num_threads(1)`（远端默认 **32**）：忘了就会得到
「32 worker × 32 线程 = 1024 线程」的假读数（实测 **46 s/局**，差 100×）。
生产路径不受影响（`collect_selfplay._init_worker` 会设成 `rollout_threads()` = 1）。

⚠️ `--workers` 应设成**物理核数**（当前实例 = **32**），不是逻辑核数（`nproc` 报 128，
那是宿主机的）——这是 GIL 型负载，超线程基本吃不到（本机 8 物理核上实测：12 worker 与
15 worker 几乎同速）。不给 `--workers` 就按 cgroup 配额自动定。

⚠️⚠️ **远端单核比本地慢 2.5~2.8×**：`Xeon Platinum 8352V @ 2.10GHz`（云 vCPU，时钟受限）
vs 本地 `Ryzen AI 7 H 350`。同一批观测、同为 b=1 的 eager 前向：本地 **265 µs** / 远端 **676 µs**。
⇒ 采集的 22.6× 是**并行度**换来的，单核速度反而是负的；报绝对耗时必须带机器名。

## ⚠️ 跨机器数字不可比

同一份源码，远端教师胜率 **44.8%**（gcc 11.4 -O3 + LTO）vs 本地 **46.2%**（MSVC Debug）。
编译器与优化等级一变，浮点行为就变，而这个搜索对微小扰动**混沌敏感**。

所以迁移后第一件事是重测基准哨兵，并在新基准下重采所有数据：

```bash
$PY -u collect_encs.py 100 500 $D/data/encdata_n100_s500.npz --workers 32   # 按怪组表逐项采
$PY -u train.py seeds $D/data/encdata_n100_s500.npz 3 --epochs 8 --threads 1 \
        --save '$D/data/encdata_n100_s500_s{seed}.pt' --tb 0
```

**不要把本地的任何绝对胜率与远端的混着比。**

## 🖥️ 跑长任务：怎么才能真的看到进度

### 铁律：长任务后面不要接 `| tail`

```bash
# ❌ 进度条会憋到最后一起吐出来，完全失去意义
ssh ... 'cd $D && $PY -u train.py seeds ... 2>&1 | tail -30'

# ✅ 强制 TTY + 不接管道（终端里直接看单行原地刷新）
ssh -tt ... 'cd $D && $PY -u train.py seeds ...'
```

**为什么**：`tail -N` 必须读完整个 stdin 才知道"最后 N 行"是哪几行，它是**汇（sink）**，
不是过滤器。这不是缓冲区大小问题，`stdbuf -oL` / `python -u` / `flush=True` **全都救不了**。

实测（一个 20 步 × 0.2 s 的 demo，用打时间戳的读取器观察每行**到达**时刻）：

| 管道 | 各行到达时刻 |
|---|---|
| `... 2>&1 \| tsread` | `1.25s 1.65s 2.05s 2.45s 2.85s ...` ← 真·流式 |
| `... 2>&1 \| tail -3 \| tsread` | **全部 `4.00s`** ← 挤在结束那一刻 |

`grep` 也有同样问题（stdout 是管道时会块缓冲），要过滤至少加 `--line-buffered`，
但**只要链里还有 `tail` 就依然是全有全无**。

### 需要过滤时：远端落盘 + 本地 `tail -f`

```bash
# 远端写日志 + 本地跟读 = 现在的标准流程（作业跑在 script pty 里，日志 job-*.log）
STS_SESSION=sts ./sync.sh start train.py train data/xxx.npz 8 0 data/out.pt   # 起作业（脚本与会话名都必填）
./sync.sh log 40               # 看尾部（自动 tr '\r' '\n'，进度条帧不会粘行）
./sync.sh wait job-xxx.log     # 静默触发器：只等完成（退出码/⛔ 即退），不输出内容
```

### 进度条（2026-09-23 起：直接用 `tqdm`，`_prog.py` 已删除）

作业一律跑在 `script` 的 **pty** 里（tmux 会话 + `script -q -e -f`）⇒ python 看到的就是 TTY
⇒ tqdm 原地刷新：pane 里 / `attach` 时就是标准进度条；日志文件拿到的是同一份 pty 字节流（含 `\r` 帧），
而工具链（`status`/`log`）**本来就**统一 `tr '\r' '\n'` 再读 ⇒ 无需特殊处理。

- 本地 `.venv` 与远端 conda 都有 tqdm（本地 `pip install tqdm`；远端 4.66.2 自带）。
- 调用形态：
  `with tqdm(total=n, desc=..., unit="局", disable=not verbose, dynamic_ncols=True, mininterval=0.4) as pg:`
  —— 附加信息用 `pg.set_postfix_str(s, refresh=False)`，读当前量用 `pg.n`。
- ⚠️ 仅当「**直接重定向落盘**、不经过 `script`/pty」时才要留神：那种场景 tqdm 会把日志写成一行 `\r` 帧。
  要么套一层 `script`，要么读日志时先 `tr '\r' '\n'`。

### 为什么 2026-09-13 没用 tqdm、09-23 又改用了

09-13 的实测结论：tqdm **非 TTY 下坚持用 `\r`**（20 步输出到文件 → 1 个 `\n`、12 个 `\r`，
整条日志被压成一行），而当时的流程是「远端直接 `> run.log` 落盘 + `tail -f`/`grep`」⇒ 淘汰、
保留自研。**当时的两个前提现已变化**：
1. 流程改成**一律 `script`（pty）** ⇒ 本来就命中 tqdm 的 TTY 分支；
2. 工具链（`status`/`log`）统一 `tr '\r' '\n'` 之后再读。

加上本地 venv 也装上 tqdm（`pip install tqdm`），自研那层（流选择 / 非 TTY 逐行 / 宽度处理 /
速率平滑）就没有维护价值了 —— 2026-09-23 全部迁到 tqdm（7 个文件 20 处调用点），`_prog.py` 删除。
（`| tail` 那个坑与进度条实现无关，见下。）

### 四个坑（全部实测过）

1. **`| tail` 是汇** —— 见上，`stdbuf`/`-u`/`flush` 都救不了。
2. （历史：自研实现时期）**超宽行会被终端折行，`\r` 随即失效 → 帧堆积。** 原实现用空格补齐擦上一帧，
   但 `_fit` 只保证内容宽 ≤ `列数-1`，再无条件补 4 格就成了 `列数+3` → **必然折行**。
   实测（pty）60 列下 **61/61 帧超宽**（最大 63），120 列下 51/61 超宽。
   改成 `ESC[K`（擦到行尾）后：40/60/120 列全部 **0 帧超宽**。
   终端**缩放**时最糟：上一帧 120 列、新帧 59 列 → 补 60 个空格 = 119 字符塞进 60 列。
   现在宽度变化时先补一个 `\n` 放弃旧行（旧帧按旧宽度写过，大概率已折行）。
3. **`\r` 帧会污染被捕获的输出。** 长任务的终端转储能到几十 KB 的粘连帧
   （实测一次采集 = 8KB+），既毁日志也污染上下文。⇒ 读日志/终端回显前先 `tr '\r' '\n'`（`status`/`log` 已内置），
   不要把进度条字节整段吞进上下文。
4. **累积平均速率会在开头算出荒谬的 ETA。** 并行任务的第一波 `workers` 个是**同时开始**的，
   要等其中**最快的一个**完成才有第一个结果。期间 `done=0` 而 `elapsed` 在涨，
   而速率用的是 `done/elapsed` → **恒为 0** → ETA 爆炸。
   实测 16000 sims 时显示 `ETA 2h55m`（实际总时长 363s），32000 时 `ETA 5h16m`（实际 696s）。

   受控实验（40 任务 / 8 worker）：等长任务死时间 = 单个任务时长；
   不等长任务下累积速率要跑完一大半才贴近稳态值。**瞬时速率全程都是平的，根本没有加速。**

   已改为**瞬时速率的指数平滑**（样本 <2 个时显示 `ETA --`）。
   ⚠️ 实现上有个坑：**采样被拒时不能推进基线** —— 并行任务会**成批同时返回**
   （实验里 8 个 worker 全在 t=1.00s 一起回来），推进基线会把一批拆成 `dn=1` 的样本，
   速率被压成 1/N（第一版就踩了：显示 1.0/s 而真值 8/s）。

### 附：终端会往输出里注入光标应答
实测在远端长时间运行时，输出中间出现过一次 `^[[41;1R` —— 那是终端对
光标位置查询（`ESC[6n`）的**应答**被注入了流（VS Code 终端集成的产物，不是我们的代码）。
无害，但会让那一行错位。

### ⚠️ 别在 `sync.sh` 运行期间编辑它

bash 按**字节偏移**增量读取脚本文件；文件在运行中被改动，它会从旧偏移继续解析。
实测撞在多字节 UTF-8 字符中间：

```
sync.sh: line 168: 道: command not found      # exit 127
```

本次无害（采集已完整落盘），但改脚本要么等它跑完，要么先 `cp` 一份再跑。

## ⚠️ SSH 断连：“把状态放到磁盘上”，而不是祈祷网络稳

实测断过**三次**（`Connection reset by peer`）。三次进程都活着（是被 pty 断连，
不是被 kill），但**本地这一侧彻底失去信息**：进度条停在半截、不知道进程死活、
不知道有没有跑完、不知道退出码。事后只能靠远端 `pgrep` + 产物文件去猜。一次是
自对弈跑到 200/1000，一次是回归训练跑到 epoch 30/40，一次是在 `bg` 提交的瞬间。

更糟的是**断线在终端里是无感知的**。根因是 ssh 默认没有连接超时：网络一断，
它会一直挂着等 TCP 超时（可达数分钟），表现就是“终端卡住不动” ——
既不报错也不退出，让人以为任务还在正常跑。

### 对策：状态不放在连接里

```bash
./sync.sh run train.py seeds data/x.npz 1 200 20 100 500   # ⭐ 默认：实时进度 + 落盘日志
./sync.sh status                                               # 跑完了吗？退出码？日志多久没动？
./sync.sh log 60                                               # 只看日志尾部（已去 \r）
./sync.sh bg  train.py ...                                 # 只在明确要脱离终端时用（无实时进度）
```

**`run` 同时给你实时进度和落盘日志** —— 这是关键的一条设计：

远端命令跑在 `script(1)` 的伪终端里 →
- `script` 给 python 一个 pty → **tqdm** 用**原地刷新**的进度条，`ssh -tt` 把它传回本地终端
- 同一份输出**落盘**在远端 `$STS_LOG`
- `script -e` 把退出码写进日志尾部的 `COMMAND_EXIT_CODE="N"` → `status` 据此判断结束没结束

⇒ **不需要在“看得见进度”和“断线不丢结果”之间二选一。**
⚠️ 曾经误选过后者：为了避免偶发断线，把所有任务都推到 `bg`，代价是**每次**都看不到进度。
为罕见故障牺牲常态体验，是明确的坏权衡。

`status` 只看**日志**一个事实来源：日志不存在 = 从没跑过；有退出码 = 已结束；没有 = 仍在运行。
它还会报**日志最后写入时间**，超过 5 分钟没动会警告 —— 光看进度条分不清“在算”和“已经死了”。

其余几条：

- 所有 ssh 都带 `ConnectTimeout=10`（可调 `STS_SSH_TIMEOUT`）——
  **这一条才是“断线无感知”的解药**：网络死了 10 秒内就显式报错，不会挂着假装还在跑。
- `run` 结束时会**区分三种结束**：正常（rc=0）/ 网络断（255，远端可能仍在跑）/ 任务真失败（其它码）。
  ⚠️ `ssh -tt` 正常收尾也会打印 `Connection to ... closed.`，它**不是**断线信号。
- `log`/`status` 会**显式打印“我在看哪个日志”**。实测踩过一次：`status` 默默看了默认的
  `run.log`，而真正在跑的是 `run_soft.log`，于是把**上一个作业的报告当成了当前作业的结果**——
  这种默默看错文件比直接报错危险得多。

### 三个实现坑（都实测踩过）

**① `set -e` 是全局的，函数里改完必须还原。**
（出自已删除的 `_ssh_run`，但教训通用。）函数里为了捕获退出码做了
`set +e ... set -e`，返回时把调用方的 `set +e` 保护**覆盖掉了** → 调用方
`_ssh_run ...; rc=$?` 在返回非零的瞬间被 errexit 打死。症状是**断网时不打印回溯快照、
直接静默退出**，恰好毁掉了这个机制自己的目的。
修法：记下进入时的 errexit 状态并还原；调用方一律写 `cmd || rc=$?`
（`||` 左侧失败不受 errexit 影响）。

**② 别用反斜杠转义写远端脚本。**（**仍在生效**）
原来是 `local cmd="... \$var ... \"..."` 一整个双引号字符串手工转义，
结果 `\\$((now - mt))` 在**本地**就被当成算术展开，而本地没有 `now`，
`set -u` 下直接 `now: unbound variable`。
改用 `read -r -d '' body <<'REMOTE'`（**引号定界符 = 不做任何本地展开**）：
远端脚本原样书写，一个反斜杠都不用，再用 `__PLACEHOLDER__` 替换路径。

**③ 解析远端输出不要按行号。**
`status` 原先取“输出第 2 行”当退出码，而**没有退出码时 `grep` 一行都不输出**，
行号整体前移 → 把运行中的作业报成“状态未知”。改成按**内容**过滤
（`grep -E 'COMMAND_EXIT_CODE|退出码'`）。

## ⚠️ `pkill -f <pattern>` 会杀死你自己

实测：远程命令里写了 `pkill -f -- "--help"`，而那个 shell 的**命令行本身就含 `"--help"`**
→ 把自己杀了，exit 255 且输出截断。
要杀进程先用 `pgrep -af` 看清楚，或按 pid 杀。

## ⚠️ 未知参数不要静默兜底

`train.py` 原先对未知模式**静默掉进 `all` 分支**，会启动一个真实的端到端任务。
实测一次 `--help` 笔误白烧了 3.5 分钟算力，还抢占了一个正在跑的消融（污染其计时）。
现在有了显式校验 + `help` 模式；未知参数直接报错退出。

教训推广：**任何"默认落到最重的分支"的 CLI 都很危险**，宁可报错。


### ⚠️ `mode: async` 会被进度条刷成超时

持续刷新的进度条会让终端**永远不进入 idle**，所以用 async 启动时基本一定会等到超时
才返回（进程其实正常在跑）。这只是"等待首个空闲信号"的机制不适用，不是错误。

## ⚠️ `ssh host 'cd X && nohup … & sleep; tail log'` 的两个坑

想"一句 ssh 就把长任务甩到远端后台"，很容易写成：

```bash
ssh host 'cd $D && nohup bash _scale_run.sh > run.scale.log 2>&1 & sleep 8; tail -5 run.scale.log'
```

**① `&` 只作用到它左边的整条 `cd X && nohup …`。** 后面的 `sleep` / `tail`
在 **ssh 会话的原始 cwd**（`/root`）里跑 ⇒ `tail: cannot open 'run.scale.log'`，
看着像"日志没生成"，其实作业正在跑。要看日志就得再 `cd` 一次。

**② 这条通道不会关。** 后台进程继承了这个连接的 stdin，ssh 会**一直挂着**
（本地终端被占死，`mode: sync` 必然超时）。好处是作业不会被 SIGHUP 掉；
坏处是这个终端不能复用，得另开一个。
要干净收尾就用下面这套 —— 旧的 `setsid nohup …` 写法已删除：现在统一用
`./sync.sh start`（远端 `tmux new-session -d`，**即时有界返回**，不占这条连接），
掉线容错交给"远端 tmux + 本地每轮新连接的守望"。远端需装有 `tmux`（`apt-get install -y tmux`）。

## 常用远端命令速查

```bash
D=/root/autodl-tmp/sts_lightspeed
PY=/root/miniconda3/bin/python

# 采集（按怪组表逐项，并行；不给 --workers 就按 cgroup 配额）
$PY -u collect_encs.py 100 500 $D/data/encdata_n100_s500.npz --workers 32
# 训练（配方全是 flag；seeds 的并行训练需 `--mp-start fork`）
$PY -u train.py seeds $D/data/encdata_n100_s500.npz 3 --epochs 8 --threads 1 \
        --save '$D/data/encdata_n100_s500_s{seed}.pt' --tb 1 --tb-dir $D/data/runs
# 评估（漫游主评；种子必须 ≥ collect.EVAL_SEED0 = 1e8）
$PY -u eval_walk.py 200 "net:$D/data/encdata_n100_s500_s0.pt:100" 100000000
```

