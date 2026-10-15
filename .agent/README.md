# `.agent/` —— 本项目笔记的**唯一权威副本**

> 知识只放这里。这些文件在工作区内、随 git 跟踪、随工作区目录迁移，
> 撤回会体现为 `git diff` / `git log` 里的显式变更。
>
> VS Code 的仓库记忆**不在工作区里**（`%APPDATA%\Code\User\workspaceStorage\<workspace-hash>\…`，
> 无可配置路径的选项，**换路径打开工作区就指向一个新的空目录**）
> ⇒ 那边**只留一个指针** `/memories/repo/README.md`，指向本文件。

## 读哪一份

| 文件 | 内容 | 什么时候读 |
|---|---|---|
| `notes.md` | **正文（唯一）**：在役结构、契约/不变量、旋钮与实测数字、评估口径、**被推翻的结论**、未解的坑、环境 | 动手改代码前，先读它 |
| `REMOTE.md` | 远端 seetacloud 环境与命令、换实例后的重来顺序 | 上远端前（⚠️ 里面部分后台是旧实例，与 `notes.md` §7 冲突时以 §7 和各 `_*.sh` 头部为准） |
| `MIGRATION.md` | 迁到新机器的「带走 / 不要带」清单 | 换机器时 |
| `CODE_STYLE.md` | 代码风格约定 | 写代码前 |

> **2026-10-14**：旧 `notes.md`（477 KB）+ `DIARY.md`（521 KB）已合并精简成现在这一份 `notes.md`，
> `DIARY.md` 删除。原文都在 git 历史里（两者最后一次改动都是 `9e8ddce3`）：
> `git show 9e8ddce3:.agent/DIARY.md`、`git show 9e8ddce3:.agent/notes.md`。
> 删掉的是流水账、已删代码的设计史、已修 bug 的排查过程。

## 跑守卫

守卫全在 C++（`apps/*-test.cpp`）；Python 侧的 `_chk_*.py` **已全部删除**。

```bash
cmake --build build --config Release -j 16          # 全量 7 目标
build/Release/engine-test.exe                       # 引擎/动作/观测布局
build/Release/scene-test.exe                        # 场景缩放、banned 列表
build/Release/selfplay-test.exe <net.ts>            # K=1 12 帧逐位对拍（可复现性）
build/Release/selfplay-test.exe <net.ts> --sweep 24 # 12 个遭遇（含 act4）扫一遍
```

⚠️ 跑 `*-test.exe` 前把 `.venv\Lib\site-packages\torch\lib` 与 python DLL 目录放进 `PATH`（否则 `0xC0000135`）。
⚠️ **守卫的价值 = 被运行的概率**：掩码口径错了 4 天没人发现，就是因为没有对应守卫。改动引擎/布局/动作空间后**务必跑一遍**。

## 代码注释约定

**工程化注释，不写历史**：只写契约 / 不变量 / 单位 / 用法；短句、命令式。
禁止口语与事故复盘（"实测…""踩过…""曾经…"、日期、"教训"）—— 那类内容进 `notes.md`，代码里最多留一行指针。
能自解释的代码不加注释。

## 复核清单

⚠️ 记忆是追加式文本，**感知不到用户撤回**。任何「状态快照」型断言（「X 已删除」「现在是 Y」）在据其
动手之前，先跑命令核实现场（`Test-Path` / `Select-String`）—— 清单正文在 `notes.md`，别在这里再抄一份
（重复正是本目录当初分叉的原因）。
