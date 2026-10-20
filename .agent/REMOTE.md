# 远端环境（seetacloud）


⚠️ **换/新实例要走的五步**（少一步就 import 不到扩展）：
1. `tar` 推源码 + `json/` + `pybind11/`（这两份不在 `sync.sh` 的同步清单里）；
2. 搭 **ONNX Runtime**：`./_ort.sh wheel`（或 `tgz`）⇒ `ORT_HOME=/root/autodl-tmp/ort/onnxruntime`；
3. 配 **必须显式给 Python**：这台机上 `python3` 不在 PATH，`find_package(Python)` 会失败 ⇒
   `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DORT_HOME=/root/autodl-tmp/ort/onnxruntime -DPython_EXECUTABLE=/root/miniconda3/bin/python`
   （128 核宿主上 `-j64` 编完只要 **1 min** 量级）；
   ⚠️ 加上一个 Release 构建给门控/生产口径：同一条命令换 `-B build-onnx -DCMAKE_BUILD_TYPE=Release`。
4. 写 `.pth`：`echo <repo>/build > /root/miniconda3/lib/python3.12/site-packages/sts_lightspeed_build.pth`；
5. 传 checkpoint（`data/*.pt` 不在仓库里，要从现役那台 `cat` 过来）。

🚨 **GPU 红线（实测踩过）**：**别开 N 个进程、每个各自带一个 CUDA 上下文**。
32 进程 × 各自 CUDA 上下文把那台容器**直接弄断**（SSH `Connection refused` ~10 分钟）。要在 GPU 上跑，
就**一个进程独占卡**（也被实测支持：每次 CUDA 调用有 ~1.45 ms 固定延迟，多进程只是把延迟串起来）。
同理：**先量 N=1 再外推**，别一上来就按核数/显存上限铺开（K=8 棵树 × 32 进程也压死过一台）。

## 本地侧：`sync.sh` **必须用 Git Bash 跑**（2026-10-10 踩过）

⚠️ 症状：终端里 `ssh -p <port> root@…` 通，但 `bash sync.sh push` 报
`Permission denied (publickey,password)` ／ `Host key verification failed`。
**远端没事，是"本地"换了**：

| 你敲的命令 | 实际执行的 ssh | 运行在 | `$HOME` | 用的私钥 |
|---|---|---|---|---|
| 终端里 `ssh` | `C:\Windows\System32\OpenSSH\ssh.exe` | Windows | `C:\Users\<你>` | 同一份 ✓ |
| `bash sync.sh`（默认 `bash`） | `/usr/bin/ssh` | **WSL 虚拟机** | `/home/<你>`（WSL 自己的） | WSL 里那份（往往没有）❌ |
| `"C:\Program Files\Git\bin\bash.exe" sync.sh` | `/usr/bin/ssh`（MSYS） | Windows | `C:\Users\<你>` ✓ | 同一份 ✓ |

`AppData\Local\Microsoft\WindowsApps\bash.exe` 是 **WSL 入口**，不是"Windows 上的 bash" ——
所以 `bash xxx.sh` 与"在终端输入"**不是同一个程序、也不是同一个 `$HOME`**。
⇒ 跑本仓任何 `.sh` 一律用 Git Bash：

    "C:\Program Files\Git\bin\bash.exe" sync.sh push
    "C:\Program Files\Git\bin\bash.exe" sync.sh          # 比较两端 md5

`sync.sh` 现在**显式给私钥**（`-i "${STS_SSH_KEY:-$HOME/.ssh/id_rsa}"`，并对 `/c/Users/<user>/…`
兜底）⇒ 不依赖 `$HOME`，换机器只改 `STS_SSH_KEY`。⚠️ 别再把密钥拷进 WSL 的 `/home/*/.ssh/`
去"修好它"——那是第二份密钥，将来只会分叉出"终端能连、脚本连不上"。

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
$PY -m pybind11_stubgen slaythespire._slaythespire -o build   # 构建后刷桩（远端没 venv ⇒ 手动）
$PY _chk_all.py                                        # 守卫总检（engine 档）
```

⚠️ **构建配置 = Debug（2026-09-30 定案，别改回 Release）**：`include/sts_common.h` **无条件**
`#define sts_asserts`，但那些 `#ifdef` 块体里写的是 **C 的 `assert`** ⇒ Release（`-DNDEBUG`）下
全仓自检**静默失效**。更危险的是两处「断 assert = UB」——
`enumerateActionsForNode` 的 `default:` 与 `playoutRandom` 的 `edges.empty()` 分支，后面都紧跟
`uniform_int_distribution(0, size-1)`。改 Debug 后全绿，且代价为零（见下条）。

产物：`build/slaythespire/_slaythespire.cpython-312-x86_64-linux-gnu.so`（LTO 开启）。
⚠️ 2026-10-11 **包化**之后扩展是**包里的** `_slaythespire`，`import slaythespire` 拿到的是包 ——
单配置生成器直接把包落在 `build/slaythespire/`，所以上面那个指向 `build/` 的 `.pth` **一个字都不用改**
（CMake 里按 `CMAKE_CONFIGURATION_TYPES` 分了两种情况）。刷桩也相应的从 `slaythespire` 变成
`slaythespire._slaythespire`。


⚠️ **本机（Windows）配置要用 `-T ClangCL`**（2026-10-15 踩过）：本仓源码里全是中文注释，而
`cl.exe` 默认按**系统代码页**（这台机 = GBK）解释源文件 ⇒ 某些多字节序列会把后面的字符/换行吃掉，
报出一堆看不懂的语法错（实测 `collect/snapshot.h` 报 30+ 条 `push` 相关的 C3867/C2059）。
`clang-cl` 按 UTF-8 读 ⇒ 没事。完整命令：

```powershell
cmake -S . -B build -T ClangCL -DCMAKE_BUILD_TYPE=Debug      # ONNX 库默认找 third_party/onnxruntime
cmake --build build --config Debug -j 16
```

⚠️ **本机（Windows / MSBuild）增量构建会谎报"已最新"（2026-10-11 踩过，代价一整天）**：`cmake --build`
报成功、`.pyd` 被重链、stubgen 也跑，但某个 `.cpp` **从未进过编译器**（当时是 `collect/scenebuild.cpp`
—— 于是所有"6 组 × 300 seed 对账"其实都是拿**同一个旧 `.pyd` 自己跟自己比**，A/B 永远全同）。
判据（MSBuild 亲口说的）：

```powershell
cmake --build build --config Debug --target slaythespire -- /v:diag |
  Select-String "scenebuild.cpp"     # → 将编译 …scenebuild.cpp，因为未能在跟踪日志中找到它。
```

解法：**删 `build/<目标>.dir`（含 `*.tlog`）再编** —— `--clean-first` 不够。
所以 `_chk_all.py` 的 exe 类条目（`exe:scene-test`）现在**先比 mtime**，源码比产物新就拒跑
（宁可红着说"产物旧了"，也不要绿色地自欺）。远端 ninja/gcc 侧还没遇到过，但本轮改动**尚未在远端编过**。

⚠️ **`Copy-Item` 会连 mtime 一起复制 ⇒ 恢复注入过的文件后构建会静默跳过（2026-10-11 又踩一次）**：
把改动过的 `.cpp` 备份到别处、注入 bug、验完再 `Copy-Item` 恢复 —— 恢复出来的文件 mtime 是**备份时那个旧值**
（比 `.obj` 还旧）⇒ MSBuild 判定"没变"、不重编，于是**恢复后跑出来的还是注入版的行为**（当场骗过我一轮：
"恢复后 golden 仍红"）。恢复后必须 `(Get-Item x.cpp).LastWriteTime = Get-Date`（或先删 `build/<目标>.dir`
再编）。**判据**：恢复后先 `Get-FileHash` 对回原值，再看产物 mtime 是否真的刷新。

⚠️ **远端：`_push.sh` / `scp` 会带 mtime ⇒ 换来"新旧源码混编"（2026-10-16 踩，症状是 import 就报
`undefined symbol`）**：本地的 `collect/teacher.cpp` mtime 是 01:31，远端 `.o` 是 03:05 ⇒ 推过去之后
make 认为"源码比产物旧"、**不重编**，于是远端同时存在「新 `selfplay.cpp`（已删 `snapshotObs` 包装）+
旧 `teacher.cpp.o`（还在调那个包装）」⇒ `.so` 里留一个未定义符号。⚠️ **共享库链接默认允许未定义符号**
⇒ `cmake --build` 一路 `EXIT=0`，直到 `import slaythespire` 才炸（而 `selfplay-test` 是 exe，链接时就报，
所以"守卫全过"也不能证明 `.so` 没问题）。
判据/解法：推完先看时间戳
（`ls -la collect/<file>.cpp build/CMakeFiles/<目标>.dir/collect/<file>.cpp.o`），
**推完一律 `find collect bindings include src apps -name '*.cpp' -o -name '*.h' | xargs touch` 再编**；
或者推完先 `python -c "import slaythespire"`（1 秒，比编 5 分钟便宜）。

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
ssh -p $PORT $HOST "cd $D && $PY _chk_all.py"              # 期望 13/13（数字随守卫增删变，别当断言）
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
⚠️ **跨 CPU 微架构也一样**（2026-10-10 实测，2026-10-11 据此删掉了那条断言）：当时 `_chk_selfplay`
里有「K=4 并肩 == 4×K=1 逐位相同」一条，它在本地 100% 通过（`max|Δπ_n| = 0`）、在远端
**THE_GUARDIAN seed=0 会翻**；后来本地也开始翻（同机上 12 次里 1 次，第 17 帧 `a_idx` 由 `0`
变 `1`、`max|Δn| = 1`、总访问数 139/139 不变）。
两侧都是纯 CPU（`device="cpu"`）、同一份网络存档（当时是 TorchScript，现为 `.onnx`）、同一份
`build_battle` —— 差别只在 **BLAS/OpenMP 的归约顺序**（批尺寸一变就变）。树叶深、平局多
（精英/Boss）的局面才翻。
⇒ **跨树合批不可能逐位等于逐棵跑**，这条断言已从守卫里删除（它挡的"多树串状态"改由 ② +
`_chk_threads` + C++ 侧的 `engine-test search` / `golden_selfplay` 承担 —— Python 侧的
`_chk_search.py` 与其参照物 `azsearch.SearchTree` 2026-10-11 已删）。教训：**不要把"只有同机才成立"的等价性写成断言** ——
它挡不住 bug，只训练人忽略红色。
⇒ 采集数据必须带上「哪台机采的」，与编译器同等对待。

### 仓库记忆的位置

Copilot 的仓库记忆存在**本机的 VS Code workspaceStorage** 里，不在仓库内：

```
%APPDATA%\Code\User\workspaceStorage\<workspace-hash>\GitHub.copilot-chat\
    memory-tool\memories\repo\*.md
```

`.agent/memory/*.md` 是**权威副本**，随 git 走 —— 不需要任何导出/导入步骤。
（记忆工具那边只剩一个指针，就在上面这个位置，不随仓库迁移。）
