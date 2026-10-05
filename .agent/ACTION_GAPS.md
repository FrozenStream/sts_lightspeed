# 动作空间缺口 —— 以及因此被「无视」的游戏内容

> 2026-09-21 立。起因：给场景加**药水**之后 `teacher_action` 抛
> `ActionEncodingError`（`dim=60`），追下去发现这不只是「少绑一个函数」，
> 而是**有一类游戏动作本项目根本表达不出来**，因而连带一批游戏内容被静默无视。
>
> ⚠️ 在这之前它一直藏着，因为**旧场景里药水恒为 0**。教训：
> **「未绑定 / 未使用」的代码路径 = 未验证的路径**，一启用就会暴露。
>
> 本文件只写**核实过**的东西；每条都附现场复核命令。

---

## 1. 战斗动作只有 5 类，其中第 4 类没有下标

`include/sim/search/Action.h`：

```cpp
enum class ActionType { CARD=0, POTION, SINGLE_CARD_SELECT, MULTI_CARD_SELECT, END_TURN };
```

编码进策略头的部分（`POLICY_DIM = 96`）：

| 下标 | 含义 |
|---|---|
| `0..59` | `CARD`：手牌位 `h = idx/6`（0..9）× 目标 `t = idx%6`（0=无目标, 1..5=怪物 0..4）|
| `60..94` | `POTION`：槽位 `s = (idx-60)/7`（0..4）× `pt`（0..5 目标，`pt=6` = **丢弃**）|
| `95` | `END_TURN` |
| — | `SINGLE_CARD_SELECT` 走**单独的** `select_head`（`SELECT_DIM = 60`）|
| **无** | **`MULTI_CARD_SELECT`** |

`MULTI_CARD_SELECT` 的 `bits >> 29 == 3`，整个引擎**只会**产生一种多选动作：

```cpp
// src/sim/search/Action.cpp:541   getCardSelectActions 的 EXHAUST_MANY / GAMBLE 分支
//   "just dont deal with this right now"
actions.push_back({search::Action(search::ActionType::MULTI_CARD_SELECT, 0)});   // bits = 0x60000000 = 1610612736
```

即**「一张都不选」**。而 `sts::py::actionToIdx` 对它返回 **`-1`**。

⇒ 后果是**硬失败**，不是静默降级：`teacher_action` / `legal_actions_and_idx` 抛
`ActionEncodingError`，`mcts_search` 也无法展开这些节点。
_（守卫在这里是对的 —— 它把「悄悄丢动作」变成了响亮失败。）_

---

## 2. 核心缺口：选牌屏的 20 个任务，快照只铺 13 个

`include/combat/CardSelectInfo.h` 的 `CardSelectTask` 共 **21** 个成员（含 `INVALID`），
非空任务 **20** 个。

`bindings/slaythespire.cpp` 的 `appendCardSelectCandidates()` 用 `switch` 铺候选槽，
**显式支持 13 个**：

```
ARMAMENTS, DUAL_WIELD, EXHAUST_ONE, FORETHOUGHT, WARCRY,
CODEX, DISCOVERY, EXHUME, HEADBUTT, HOLOGRAM,
LIQUID_MEMORIES_POTION, SECRET_TECHNIQUE, SECRET_WEAPON
```

剩下 **7 个落到 `default:`** ⇒ `candidateCount = 0`、**一个候选槽都不铺**：

| 任务 | 谁会触发 | 铁甲可达 | 引擎能执行吗 |
|---|---|---|---|
| `EXHAUST_MANY` | 药水 `ELIXIR_POTION`（烧 10 张）<br>卡 `PURITY` | ✅ 药水侧**可达**<br>❌ 卡侧（不在红卡池）| ✅ `chooseExhaustCards` 已实现 |
| `GAMBLE` | 药水 `GAMBLERS_BREW`<br>遗物 `GAMBLING_CHIP`<br>卡 `CALCULATED_GAMBLE` | ✅ 药水侧**可达**<br>⚠️ 遗物侧理论可达（见 §4）<br>❌ 卡侧 | ✅ `chooseGambleCards` 已实现 |
| `MEDITATE` | 观者卡 | ❌ | ❌ 未实现 |
| `NIGHTMARE` | 静默卡 | ❌ | ❌ 未实现 |
| `RECYCLE` | 机器人卡（`chooseRecycleCard` 写了，但**任务从不被赋值**）| ❌ | ❌ |
| `SEEK` | 机器人卡 | ❌ | ❌ |
| `SETUP` | 静默卡 | ❌ | ❌ |

**这两组的性质完全不同**，别混为一谈：

- **`EXHAUST_MANY` / `GAMBLE` = 我们这边的问题。** 引擎执行路径是好的，
  是**观测 + 动作编码**给不出表达。而且连上游教师自己都放弃了
  （`SimpleAgent.cpp:506` 的注释 `// just select none`）——
  我们连这个退化的「不选」都编不进去。
- **另外 5 个 = 引擎自己就没实现。** `bindings/bindings-util.cpp` 的 `selectCard()` 里写得很直白：

  ```cpp
  case CardSelectTask::MEDITATE:
  case CardSelectTask::NIGHTMARE:
  case CardSelectTask::RECYCLE:
  case CardSelectTask::SEEK:
  case CardSelectTask::SETUP:
      // not implemented in the engine yet
      break;
  ```

  且只有非铁甲角色能触发 ⇒ 对当前项目**不可达**，暂时不用管。

**复现**（原始报错现场）：Asc 20、`seed 16`、step 3 —— 教师喝掉 `Elixir Potion`，
`input_state=2`、`task=EXHAUST_MANY`、`n_legal=1`，而那个唯一合法动作
`action_to_idx` 返回 `-1`。

---

## 3. 因此被无视的游戏内容（具体到条目）

### 3.1 药水：铁甲池 33 瓶，被排除 2 瓶

`_boss.UNSUPPORTED_POTIONS = (ELIXIR_POTION, GAMBLERS_BREW)`

| 药水 | 真实效果 | 状态 |
|---|---|---|
| **Elixir Potion** | 消耗手牌中最多 10 张 | 🚫 **已剔除**（触发 `EXHAUST_MANY`）|
| **Gamblers Brew** | 手牌任意张洗回牌堆，再抽等量 | 🚫 **已剔除**（触发 `GAMBLE`）|
| Liquid Memories | 弃牌堆 1 张回手（`LIQUID_MEMORIES_POTION`）| ✅ 支持 |
| Fairy Potion | 致死时自动回复 | ✅ 不可主动喝（`isValidPotionAction` 拒绝）+ 自动复活已实现（`Player.cpp:334`）|
| Smoke Bomb | 脱离战斗 | ⚠️ 战斗内是 `// todo` 空操作，但仍**是合法动作** ⇒ 网络可以把一瓶药水浪费在无效果上 |

⇒ 铁甲少了**两张实用药水**。这是**已知且明确的能力损失**，不是"顺手过滤一下"。
场景侧带断言防止排除列表静默过期（`_boss.grant_run_resources`）。

### 3.2 遗物：会开选牌屏的只有 3 件

| 遗物 | 任务 | 状态 |
|---|---|---|
| `Toolbox` | `DISCOVERY` | ✅ 支持（且档位是 SHOP，抽不到）|
| `Nilry's Codex` | `CODEX` | ✅ 支持（SPECIAL，抽不到）|
| **`Gambling Chip`** | **`GAMBLE`** | ❌ **不支持** |

⚠️ **`Gambling Chip` 是颗定时炸弹**：档位表（`Relics.h` 的 `relicTiers[]`）把它标成
`RARE`（真实游戏是 Uncommon），而当前抽取只有 `COMMON 2~3 + UNCOMMON 0~1`
⇒ **摸不到**，实测 500 seed 出现 **0** 次。
**一旦把抽取扩到 RARE，训练立刻会被它打挂。**

### 3.3 卡牌侧：够不到

红卡池没有触发这两个任务的卡（`PURITY`、`CALCULATED_GAMBLE` 都不在池里），
其余 5 个任务只有非铁甲角色能触发 ⇒ **卡牌侧目前无缺口**。
但这是在「牌堆只从红池抽」这个前提下成立的；**别把这个前提当成永久的**。

### 3.4 实测复核（500 seed，Asc 20）

```
唯一遗物 64 种、唯一药水 32 种（31 真 + 空槽）
GAMBLING_CHIP 0    TOOLBOX 0    NILRYS_CODEX 0
ELIXIR_POTION 0    GAMBLERS_BREW 0    LIQUID_MEMORIES 23 ✅
```

⇒ 当前的**场景抽取 + 排除列表**与「不可表达的动作」这一集合**恰好不冲突**。
（这个结论有前提，见 §4。）

---

## 4. 相邻缺口：不是「动作」问题，但同样让游戏内容恒为 0

### 4.1 遗物实现覆盖（181 件）

| | 数量 |
|---|---|
| 战斗侧（`src/combat`、`include/combat`）有实现 | **120** |
| 只有战斗外实现（`src/game`）| **51** |
| 两处都没有 | **10**（含 `TOY_ORNITHOPTER`：真实效果「每次用药回 5 血」）|

**本场景实际抽到的 61 种里，23 种（38%）战斗侧无实现** —— 即状态空间里 **23 列恒 0**：

```
CERAMIC_FISH, DREAM_CATCHER, ETERNAL_FEATHER, FROZEN_EGG, JUZU_BRACELET,
MATRYOSHKA, MAW_BANK, MEAL_TICKET, MOLTEN_EGG, PEAR, POTION_BELT, QUESTION_CARD,
REGAL_PILLOW, SINGING_BOWL, SMILING_MASK, STRAWBERRY, THE_COURIER, TINY_CHEST,
TOXIC_EGG, TOY_ORNITHOPTER, WAR_PAINT, WHETSTONE, WHITE_BEAST_STATUE
```

原因有两类，**含义不同**：

1. **本质是战斗外效果**（休息点 / 宝箱 / 商店 / 卡牌奖励 / 上限血）。
   但注意 —— **在本项目里它们连那个触发机会都没有**：遗物是开战时直接塞进
   `bc` 的，`WHETSTONE` / `WAR_PAINT` / `STRAWBERRY` / `PEAR` / `FROZEN_EGG`
   这类「**获得时**生效」的效果从未运行过 ⇒ 对战斗**真的**没有影响，恒 0 是正确的。
2. **两处都没实现**（10 件）。抽到了，但什么都不做。

⚠️ 这不是 bug。但它会**误导直觉**：看到状态里有遗物位、却得不到收益，
很容易想去「只挑有用的遗物」—— 那正是 6 倍选择偏差的来源
（见 `memory/experiment-pitfalls.md`）。**恒 0 列是事实，去筛它才是错。**

### 4.2 其他已知的「字段存在但无意义」

此前记录（见 `memory/experiment-pitfalls.md` 的「快照里的残留字段」）：

- `outcome` 每帧都是 0
- 怪兽死亡后槽位**不压缩**，且死怪仍报 `intentDmg`
- `monsterCount` ≠ 存活怪数
- `cardSelectTask` 在**非选牌帧**仍留上一次的值 ⇒ 已用 `STS_GATE_SELECT` 门控

---

## 5. 要做「多选支持」的话，工作量在哪

好消息：**引擎执行侧已经好了** —— `chooseExhaustCards` / `chooseGambleCards` 都在，
`Action::execute` 对 `MULTI_CARD_SELECT` 的分发也是通的。缺的只有三件事：

1. **观测**：候选块要铺 multi-hot。`pickCount ≤ 10`（10 是 `ELIXIR_POTION` 的上限）
   ⇒ 每张候选 1 位、最多 10 位；再给 `pickCount` / `canPickZero` / `canPickAnyNumber`（`select_meta` 已有）
2. **动作**：要有一个**可编码**的下标表示「确认当前勾选」。
   ⚠️ 这会**扩容策略头** ⇒ **作废全部缓存与检查点**，必须与别的改动分开做、单独计账
3. **搜索**：`getCardSelectActions` 的 `MULTI_CARD_SELECT` 分支要真的展开组合
   （现在是 `just dont deal with this right now`）

**建议顺序**：先按 §3.1 的排除列表把训练跑起来（当前状态），
多选支持等有独立预算时再做 —— 它属于「结构性缺口」，不该混进一次数据/训练迭代里。

---

## 6. 现场复核命令

```bash
# 谁触发了这两个任务（唯一权威来源）
grep -rn "Actions::ExhaustMany\|Actions::GambleAction" src/combat/BattleContext.cpp

# 快照支持哪些选牌任务
sed -n '/appendCardSelectCandidates/,/^    }/p' bindings/slaythespire.cpp

# 引擎自己哪些任务没实现
grep -n -A6 "not implemented in the engine yet" bindings/bindings-util.cpp

# 实际抽到的资源里有没有「不可表达」的
python - <<'PY'
import slaythespire as sts, _boss, collections
rc, pc = collections.Counter(), collections.Counter()
for s in range(500):
    gc = sts.GameContext(_boss.CHARACTER, s, 20)
    _boss.build_deck(gc, s); _boss.grant_run_resources(gc, s)
    for r in gc.relics: rc[r.id] += 1
    for p in gc.potions: pc[p] += 1
for n in ("GAMBLING_CHIP","TOOLBOX","NILRYS_CODEX"):
    print(n, rc.get(getattr(sts.RelicId, n), 0))
for n in ("ELIXIR_POTION","GAMBLERS_BREW"):
    print(n, pc.get(getattr(sts.Potion, n), 0))
PY
```

⚠️ 改**抽取范围**（加 RARE/BOSS 档、加药水瓶数）之后，**必须重跑上面最后一段** ——
`Gambling Chip` 就在 RARE 档等着。
