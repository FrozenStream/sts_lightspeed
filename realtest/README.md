# 真机 · 仅战斗可视化测试（CommunicationMod 桥接）

**⚠️ 本工具只在「本机」——也就是**装了游戏的那台机器**——上跑。**整个 `realtest/` 目录
（程序 + 本文）不在 `_sync.sh` 受管集里**（与 `_old/` 同理），远端（无头训练机）不需要也不该有。
真要临时推某个文件做严格校验（如「布局 == azlayout」）用显式清单，用完即删。

**定位**：训练在快引擎（本仓 C++），**测试在真机**（游戏本体）。桥接方式与工程经验参照
[JustinoChan/AscensionAI](https://github.com/JustinoChan/AscensionAI)（说明文件怎么被 mod 拉起、
stdout 纪律、屏幕兜底）与 [ForgottenArbiter/CommunicationMod](https://github.com/ForgottenArbiter/CommunicationMod)
（协议本体）。"仅战斗"的含义：**非战斗屏幕只有最简胶水**（跳过卡牌奖励、走第一条地图路线、
事件选 0），目的只是把游戏推到下一场战斗。

## 0. 组成

| 文件 | 作用 |
|---|---|
| `comm_map.py` | 纯映射层：`include/constants/` 字符串表 ↔ 各 ID 空间；真机 `game_state` → **1076 维快照**（与训练同构）；动作下标 ↔ 游戏命令。 |
| `comm_battle.py` | CommunicationMod 客户端：`ready` → 收状态 → 决策 → `PLAY/POTION/END/CHOOSE`；终端 HUD、`--record`/`--replay`。 |
| `_chk_comm.py` | **离线守卫**（不需要游戏/引擎）：**69** 条断言，覆盖 ID 表、快照逐块、掩码、命令、选牌（含药水类 `confirm` 屏与 Burning Pact 型两段式）、**选牌屏 × 网络策略**、端到端协议序列。 |

## 1. 前置（Windows）

- Slay the Spire（Steam）+ [ModTheSpire](https://github.com/kiooeht/ModTheSpire) + [BaseMod](https://github.com/daviscook477/BaseMod) + [CommunicationMod](https://github.com/ForgottenArbiter/CommunicationMod)（可选 [SuperFastMode](https://github.com/Skrelpoid/SuperFastMode) 提速）。
- **Java 8**（ModTheSpire/BaseMod 需要）。
- 本仓 `.venv`（`comm_battle.py` 只用 numpy；用 `--ckpt` 时还需要 torch 与最新引擎模块，见 §4）。
- 游戏设置里开 **Fast Mode**。

## 2. 安装与配置

1. 把 `CommunicationMod.jar`（放 BaseMod 的 mods 目录即可）与 ModTheSpire 装好；
2. 先启一次游戏（ModTheSpire 勾选 BaseMod + CommunicationMod），它会生成配置文件：
   `%LOCALAPPDATA%\ModTheSpire\CommunicationMod\config.properties`
3. 写入（**冒号/反斜杠要转义**，见 CommMod README）：

```properties
command=D\:/tmp/sts_lightspeed/.venv/Scripts/python.exe D\:/tmp/sts_lightspeed/realtest/comm_battle.py --ckpt D\:/tmp/sts_lightspeed/data/n20k_obs.pt --fights 0 --record D\:/tmp/sts_lightspeed/data/comm_run.jsonl --hud
```

4. 重新启动游戏 —— CommunicationMod 会**在启动时拉起**上面的命令（`ready` 在 10 秒内必须送达，
   否则进程被杀；本客户端在第一行代码就发 `ready`）。

调试提示：mod 把 stdout **只**当协议通道；本客户端的所有人类输出都走 **stderr + `data/comm_*.log`**。

## 3. 运行/观感（两种模式）

- **默认 = 观察模式**：非战斗屏幕**完全不操作** —— 地图/奖励/商店由你手动玩；游戏一进战斗
  （含战斗内选牌）就自动接管，战斗结束又交还控制权。CommunicationMod 的语义是
  “不回命令 = 游戏保持现状、人类照常操作”，所以这不需要任何额外机制。
- **`--glue` = 无人值守胶水模式**：自动 `START`、走第一条地图路线、跳卡牌奖励…，
  配合 `--games/--fights` 做整局连打（口径见 §6）。
- 战斗：每回合在日志窗口打印 HUD（例）：

```
┌─ 第 1 场 · 回合 2 · NONE ─ 英雄 42/80 ⚡3 🛡5 💰123 血 5层
│ [0]  Cultist            48/50 🛡0 意图 ATTACK 6×1
│ 手牌 1:Strike(1) 2:Strike(1) 3:Defend(1) 4:Bash*(2)
│ 决策 打出「Defend」 | top: #12:1.00
└ 缺口 2 条（…）
```

- `--fights N` 打满 N 场自动退出（游戏留在原地）；`--games N` 连打 N 局（游戏结束 → 回主菜单 → 再 START）。
- 回放（**不需要游戏**）：`python comm_battle.py --replay data/comm_run.jsonl [--ckpt ...]`
  —— 重渲染 HUD；带 ckpt 时对每条记录打印「现策略选了什么 vs 当时选了什么」。

## 4. 引擎模块与布局校验

- `comm_map` 的偏移唯一来源是 `azlayout`（⇐ `sts.snapshot_layout()`）。
- 真机测试机上若 `build/.../slaythespire.*.pyd` 是**旧快照**的：`azlayout` import 失败 ⇒
  `comm_map` **回退内置布局**并告警（可运行，但"布局 == azlayout"的严格校验被跳过）。
  要跑完整校验：在该机重建引擎后重跑 `_chk_comm.py`（远端 16 核增量秒级；本地见 README/CMake）。
- ⚠️ 布局一变（1080+），**训练权重与缓存一起作废**；`--ckpt` 加载会因 `space`/形状不符直接报错。

## 5. 缺口（gaps）怎么读

HUD 末尾与记录里的 `缺口：…` / `近似：…` 是本层**契约**：拿不到的字段宁可写 0 也不猜。
常见条目：

| 条目 | 原因 / 影响 |
|---|---|
| `cardsPlayed/attacksPlayed/skillsPlayedThisTurn` 写 0 | 真机 JSON 不给这三项（引擎里是内部计数） |
| `cardSelectTask` 写 0 | 真机选牌屏幕不给"任务类型"（21 类枚举）；选牌**候选集**是对的 |
| `无地图上下文 ⇒ 直方图 0` | 还没走过地图节点；选过 MAP 后自动带上（仅下一行） |
| `move_id 超出招式块` | 招式编号约定对不上（1 基 + 组内同序）；影响"意图招式嵌入"一项 |
| `没有 move_id（戴 Runic Dome）` | 游戏主动隐藏意图 ⇒ 意图伤害/次数也为 0 |
| `未知卡牌/遗物/药水/怪物/power id` | 名字不在本仓字符串表里（新版本游戏/模组）—— 该条目会点名 |

## 6. 已知限制（v1）

- 只保证**铁甲**语义（stance=0、球位=0；每回合能量按遗物/狂暴近似）。
- 多选屏幕（选 2 张丢弃等）走"逐张 CHOOSE → proceed"；`unselect` 不支持（CommMod 限制）。
- `--glue` 模式：地图只走第一条路线、事件只选 0、卡牌奖励直接跳、**未点取的战斗奖励会丢** —— 想改就动 `comm_battle.Client.glue`。
- 观察模式没有上述限制（非战斗由人开）。
- 道具/商店不买；Boss 遗物选 0。
- **药水类「任意多选」屏（Elixir 等）**：完成按钮是 `confirm`（不是 `proceed`）；当前策略 = **0 张直接确认**（合法、保守、不消牌）。2026-09-23 修过两处（`confirm` 不识别 ⇒ 假故障；选牌屏喂网络的掩码未截断 ⇒ 96 vs 60 异常），真机帧已固化进 `_chk_comm` 回归。
- 训练数据是**漫游分布**（随机资源、~13.8 场/局），真机是真实成长曲线 —— 前期多为 act1 小怪（网络见过），act2/3 上层是分布外，这本身就是测试要看的。

## 7. 故障排查

| 现象 | 处置 |
|---|---|
| 启动后游戏卡 10 秒、进程被杀 | `ready` 没送达/command 写错（转义！）——看 `communication_mod_errors.log` |
| 游戏日志里刷 `error` | 本客户端记 `⚠️ 游戏报错：…`，并做**故障限频**重询（≤ 1 次/30s）；累计 30 次自动退出 |
| 一直不动 | 看 `data/comm_*.log`：`🚨 故障：…` 会点名原因（未识别屏幕/异常），把它修进 `glue`/解析 |
| 战斗里狂刷 `state`、按不出牌 | 2026-09-23 已修：END 合法性只认消息**顶层** `available_commands`；正常帧零重发（重发仅故障、≤ 1 次/30s）。旧日志见 `…\SlayTheSpire\communication_mod_errors.log` |
| `PLAY` 被拒绝 | 掩码与游戏合法集不一致（差在卡牌目标/费用）；错误会回落，且该帧已记录进 `--record` 便于复盘 |
