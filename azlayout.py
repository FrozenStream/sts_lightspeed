"""观测布局 → 下标 / 名义类别（**归一化不在这里**：除数写在 C++ 快照的赋值处）"""

import slaythespire as sts

# —————— 快照布局：C++ 键的逐条具名 ——————
LAYOUT = sts.snapshot_layout()

# ── 整体 ──
OBS_DIM = LAYOUT["len"]
ENCODED_CARD_ID_TYPES = LAYOUT["encoded_card_id_types"]


# ── id 空间的取值域（= 嵌入表行数 / one-hot 宽度 / 多热列数）──
MONSTER_ID_TYPES = LAYOUT["monster_id_types"]                 # 基数 = `MonsterId` 成员数
MONSTER_MOVE_ID_TYPES = LAYOUT["monster_move_id_types"]       # 基数 = `MonsterMoveId` 成员数
INPUT_STATE_TYPES = LAYOUT["input_state_types"]               # 基数 = `InputState` 成员数
CARD_SELECT_TASK_TYPES = LAYOUT["card_select_task_types"]     # 基数 = `CardSelectTask` 成员数
STANCE_TYPES = LAYOUT["stance_types"]                         # [NEUTRAL, CALM, WRATH, DIVINITY]
ORANGE_PELLETS_TYPES = LAYOUT["orange_pellets_types"]         # 橙丸位域（三牌型各是否已用）→ 8 种组合


# ── 逐槽块：容量 = *_SLOTS，每条目宽度 = *_FIELDS ──
HAND_OFF = LAYOUT["hand_off"]
HAND_SLOTS = LAYOUT["hand_slots"]                   # 槽数（= STS 手牌上限）
HAND_FIELDS = LAYOUT["hand_fields"]                 # [cardId, costForTurn, specialData, playable]

DRAW_OFF = LAYOUT["draw_off"]
DISCARD_OFF = LAYOUT["discard_off"]
EXHAUST_OFF = LAYOUT["exhaust_off"]
PILE_SLOTS = LAYOUT["pile_slots"]                   # 槽数/牌堆（容量，不是截断线；槽序无语义）
PILE_FIELDS = LAYOUT["pile_fields"]                 # [cardId]

DECK_OFF = LAYOUT["deck_off"]
DECK_SLOTS = LAYOUT["deck_slots"]                   # 槽数（= 引擎卡组上限）
DECK_FIELDS = LAYOUT["deck_fields"]                 # [cardId]

MONSTER_OFF = LAYOUT["monster_off"]
MONSTER_SLOTS = LAYOUT["monster_slots"]             # 怪槽数（= 动作目标字段能表示的怪数）
MONSTER_FIELDS = LAYOUT["monster_fields"]           # [id, moveId, curHp, maxHp, block, targetable, intentDmg, intentHits, miscInfo]

CARD_SELECT_OFF = LAYOUT["card_select_off"]         # 候选区：只在需要选牌时被赋值，其余帧整块置 0
CARD_SELECT_SLOTS = LAYOUT["card_select_slots"]     # 槽数（= `PILE_SLOTS`）
CARD_SELECT_FIELDS = LAYOUT["card_select_fields"]   # [cardId, cost, costForTurn, specialData]

POTION_SLOT_OFF = LAYOUT["potion_slot_off"]
POTION_SLOTS = LAYOUT["potion_slots"]            # 槽数（= POTION_BELT 上限），每槽 1 项 = 药水 id
POTION_TYPES = LAYOUT["potion_types"]               # 药水 id 基数（= 枚举成员数）；0 = 空槽


# ── 扁平块（无槽结构）：宽度 = `*_LEN` ──
COUNTS_OFF = LAYOUT["counts_off"]
COUNTS_LEN = LAYOUT["counts_len"]                   # 内容见下方「真正占用的长度」

PLAYER_LEN = LAYOUT["player_len"]                   # [curHp, maxHp, block, energy, turn, inputState, cardSelectTask]
PLAYER_OFF = LAYOUT["player_off"]
PLAYER_ATTR_LEN = LAYOUT["player_attr_len"]         # [stance, orbSlots, energyPerTurn]
PLAYER_ATTR_OFF = LAYOUT["player_attr_off"]
PLAYER_COUNTER_OFF = LAYOUT["player_counter_off"]
PLAYER_COUNTER_LEN = LAYOUT["player_counter_len"]   # 本轮出牌计数 + 遗物 counter（逐列见 C++ `Snap`）
PLAYER_STATUS_OFF = LAYOUT["player_status_off"]
PLAYER_STATUS_LEN = LAYOUT["player_status_len"]     # 下标 = PlayerStatus 枚举值

MONSTER_STATUS_OFF = LAYOUT["monster_status_off"]
MONSTER_STATUS_LEN = LAYOUT["monster_status_len"]   # 下标 = MonsterStatus 枚举值（每只怪一组）

SELECT_META_OFF = LAYOUT["select_meta_off"]
SELECT_META_LEN = LAYOUT["select_meta_len"]         # 4 项 —— 内容见下方「真正占用的长度」

RUN_CTX_OFF = LAYOUT["run_ctx_off"]
RUN_CTX_LEN = LAYOUT["run_ctx_len"]                 # [floor, gold]

RELIC_OFF = LAYOUT["relic_off"]
RELIC_TYPES = LAYOUT["relic_len"]                     # 下标 = RelicId 枚举值（多热列数）

POTION_META_OFF = LAYOUT["potion_meta_off"]
POTION_META_LEN = LAYOUT["potion_meta_len"]         # [potionCount, potionCapacity]


# ── 派生的小组合 ──
PILE_OFFSETS = (DRAW_OFF, DISCARD_OFF, EXHAUST_OFF)

# —————— 单字段下标 ——————
# [curHp, maxHp, block, energy, turn, inputState, cardSelectTask]
P_INPUT_STATE = PLAYER_OFF + 5
P_SELECT_TASK = PLAYER_OFF + 6
# 橙丸位域：`playerCounter` 块的第 13 列（逐列清单见 C++ `Snap` 里那块注释）。
# 它是**位域不是计数** ⇒ 当名义类别（8 种组合），不跟着邻居除同一个除数。
P_ORANGE_PELLETS = PLAYER_COUNTER_OFF + 12

# —————— 卡牌嵌入：手牌槽 / 三个牌堆槽 / 总牌组槽 / 卡牌选择候选槽 ——————
# 升级前后 = 不同 id，直接嵌入（cardId = raw*2 + upgraded）。
CARD_ID_IDX = ([HAND_OFF + HAND_FIELDS * i for i in range(HAND_SLOTS)] +
               [offset + PILE_FIELDS * j for offset in PILE_OFFSETS for j in range(PILE_SLOTS)] +
               [DECK_OFF + DECK_FIELDS * j for j in range(DECK_SLOTS)] +
               [CARD_SELECT_OFF + CARD_SELECT_FIELDS * k for k in range(CARD_SELECT_SLOTS)])

# —————— 药水嵌入 ——————
POTION_SLOT_IDX = [POTION_SLOT_OFF + k for k in range(POTION_SLOTS)]

# —————— 遗物嵌入 ——————
ENTITY_IDX = list(range(RELIC_OFF, RELIC_OFF + RELIC_TYPES))

# —————— 不进网络的列 ——————
# 除 `CARD_ID_IDX` / `CAT_IDX` 之外：遗物（走池化）+ 药水逐槽 id（走查表）。
SKIP_IDX = set(ENTITY_IDX) | set(POTION_SLOT_IDX)

# —————— 真正占用的长度（每一块“用了几个槽”） ——————
# 逐槽块都是**定长**的（容量 = `*_SLOTS`），而“这一帧真正用了几个槽”写在别处：
#   hand / 三个牌堆 / 总牌组 / 怪物 → `counts` 块（字段顺序见 C++ `Snap::countsLen` 上方）
#   选牌候选 → `select_meta[0]`；药水 → `potion_meta[0]`。
# ⚠️ 这些列都是**已归一化的输入**（除数写在 C++ 的赋值处）⇒ 不是一个“读出来就能用的数”。
#    要真值走引擎访问器：`bc.monsters_alive` / `bc.master_deck_size` / `bc.monsters[i].cur_hp` …
COUNTS_MONSTER_COUNT_IDX = COUNTS_OFF + 0           # **含尸体**、只增不减 ⇒ 不是活怪数
COUNTS_MONSTER_ALIVE_IDX = COUNTS_OFF + 1           # curHp > 0，胜负判据
COUNTS_HAND_SIZE_IDX = COUNTS_OFF + 2
COUNTS_DRAW_SIZE_IDX = COUNTS_OFF + 3
COUNTS_DISCARD_SIZE_IDX = COUNTS_OFF + 4
COUNTS_EXHAUST_SIZE_IDX = COUNTS_OFF + 5
COUNTS_DECK_SIZE_IDX = COUNTS_OFF + 6
SELECT_CANDIDATE_COUNT_IDX = SELECT_META_OFF + 0    # 候选槽的真实条数
POTION_COUNT_IDX = POTION_META_OFF + 0              # 袋子里瓶数（÷`POTION_SLOTS`）

# —————— 卡牌集合块（三个牌堆 + 总牌组） ——————
# 走“逐槽 id → 均值池化”的卡牌集合块：三个牌堆 + **总牌组**（最后一块）。
# `aznet.card_feats` 对这几块是**同一段代码**（同一套「逐槽 id → 均值池化」口径）。
PILE_CARD_IDX = ([[off + PILE_FIELDS * j for j in range(PILE_SLOTS)] for off in PILE_OFFSETS]
                 + [[DECK_OFF + DECK_FIELDS * j for j in range(DECK_SLOTS)]])
# 三个牌堆的**占用长度**，顺序与 `PILE_OFFSETS` 一致。
PILE_COUNT_IDX = [COUNTS_DRAW_SIZE_IDX, COUNTS_DISCARD_SIZE_IDX, COUNTS_EXHAUST_SIZE_IDX]

# —————— 卡牌的顺序 / 空槽口径 ——————
# 卡牌 id 的两种消费方式（下标都取自 CARD_ID_IDX）：
#   手牌 / 候选集 —— **逐槽位**嵌入（候选集槽号 == select_head 动作下标，位置有意义）
#   牌堆 / 总牌组 —— **只给集合**：逐槽 id → 均值池化，空槽排除在「和」与「分母」之外。
#
# ✅ 牌堆块的槽位给的是**容器序**（不排序），而网络对这几块**只做集合池化** ⇒ 槽序不可见、
#    也不构成预言机泄漏。代价（已明确接受）：`Havoc`/`Mayhem`/`Warcry`/`Headbutt` 这 4 张
#    铁甲牌失去「看堆顶」的能力 —— 真机本来就看不到堆顶，且是可枚举的已知小集合。
#    （实现只此一条路径：`aznet.card_feats`。）

HAND_CARD_IDX = [HAND_OFF + HAND_FIELDS * i for i in range(HAND_SLOTS)]
# 手牌槽的**额外标量列**（除 cardId 外：costForTurn, specialData, playable）—— 字段号从
# `HAND_FIELDS` 派生，别手写。
# 用途：两条路都要**比较手上哪张牌值得打**，光有卡牌嵌入不够，还得知道每张牌的费用/能否打。
# ⚠️ 消费者（`aznet.policy_keys` 的 reshape 末维、`k_hand` 输入宽度）一律
#    import `HAND_EXTRA_N`，别手写 3（就是 `potionTypes` 曾手写 43、枚举长员后静默丢列那类坑）。
HAND_EXTRA_OFF = tuple(range(1, HAND_FIELDS))
HAND_EXTRA_N = len(HAND_EXTRA_OFF)
HAND_EXTRA_IDX = [HAND_OFF + HAND_FIELDS * i + f for i in range(HAND_SLOTS) for f in HAND_EXTRA_OFF]

# —————— 选牌候选 ——————
SEL_CARD_IDX = [CARD_SELECT_OFF + CARD_SELECT_FIELDS * k
                for k in range(CARD_SELECT_SLOTS)]
# 候选槽的**额外标量列**（除 cardId 外：cost, costForTurn, specialData）—— 字段号同样从
# `CARD_SELECT_FIELDS` 派生，与手牌侧同形。
# ⚠️ 非法/空槽整槽填 `[0,0,0,0]`（`appendCardSelectCandidates`）⇒ **合法掩码就是 cardId != 0**。
#    槽 k == `select_card(k)`，即容器序（手牌 / 消耗堆 / 弃牌堆 / 抽牌堆 / 生成的 3 张）。
SEL_EXTRA_OFF = tuple(range(1, CARD_SELECT_FIELDS))
SEL_EXTRA_N = len(SEL_EXTRA_OFF)
SEL_EXTRA_IDX = [CARD_SELECT_OFF + CARD_SELECT_FIELDS * k + f
                 for k in range(CARD_SELECT_SLOTS) for f in SEL_EXTRA_OFF]
# `InputState::EXECUTING_ACTIONS = 0` / `PLAYER_NORMAL = 1` / `CARD_SELECT = 2`
# （枚举定义见 `include/combat/InputState.h`，后面还有 CHOOSE_STANCE 等选择态）。
INPUT_STATE_CARD_SELECT = int(sts.InputState.CARD_SELECT)
# 「选牌区」= 候选块（cardId + cost + costForTurn + specialData）+ `cardSelectTask`
#   + `select_meta` 4 项。
SELECT_REGION_IDX = (list(SEL_CARD_IDX) + list(SEL_EXTRA_IDX) + [P_SELECT_TASK]
                     + list(range(SELECT_META_OFF, SELECT_META_OFF + SELECT_META_LEN)))

# —————— 选牌区门控 ——————
# **非 CARD_SELECT 帧把整个选牌区置 0**（无条件）。
#
# 为什么需要：`BattleContext::cardSelectInfo` **从不清零** —— 全仓 13 处赋值、**0 处复位**
#   （`src/combat/Actions.cpp` 11 行 + `BattleContext.cpp` 2 行），而 `appendCardSelectCandidates()`
#   只按 `cardSelectTask` 分支、**不看 inputState** ⇒ 不门控就把那几维 one-hot 变成
#   「这局发生过哪种选牌」的伪特征。
#   （候选块本身**已经是 0**：`isValidSingleCardSelectAction` 开头就
#    `if (inputState != CARD_SELECT) return false`，每槽都被涂成「非法」；而 `candidateCount = n`
#    照样计数 ⇒ 所以它**有值但槽全 0**，且它就等于当前牌堆大小（`counts` 块里已有）⇒ 冗余。
#    `pickCount` 是残留常量。⇒ **真正有信息量的只有 `cardSelectTask`**；其余一并置 0
#    只是为了让不变量变成“非 select 帧的选牌区**恒全 0**”，好断言。）

# —————— 怪物 ——————
# `MONSTER_FIELDS`（12 项/只）的内容：
#   [0] monsterId  [1] intentMoveId  [2] curHp  [3] maxHp  [4] block  [5] targetable
#   [6] intentDamage  [7] intentHits  [8] halfDead  [9] escapeNext  [10] isEscaping
#   [11] miscInfo（每怪杂项，含义随 monsterId 变）
# ⚠️ **槽位从不压缩、死怪留着** ⇒ `MONSTER_SLOTS` 不是活怪数（活怪数见 `COUNTS_MONSTER_ALIVE_IDX`）。
# ⚠️ 生死/去留**只有 `targetable` 一列**（= `!isDeadOrEscaped()`）；「为什么不能打」在意图 id 里：
#    觉醒者一阶段 / 暗灵倒下当帧的意图就是复活招式（`REVIVE_MOVES`），逃跑的 Looter/Mugger
#    停在 `LOOTER_ESCAPE`/`MUGGER_ESCAPE`，永久尸体则停在它最后一次滚出的招式。
MON_TARGETABLE_IDX = [MONSTER_OFF + MONSTER_FIELDS * i + 5 for i in range(MONSTER_SLOTS)]

# 「待复活」的意图招式名（`MonsterMoveId`）—— 判断尸体 vs 待复活的**语义**依据。
REVIVE_MOVES = frozenset({"AWAKENED_ONE_REBIRTH", "DARKLING_REGROW", "DARKLING_REINCARNATE"})

# —————— 怪物 ID 嵌入 ——————
MONSTER_ID_IDX = [MONSTER_OFF + MONSTER_FIELDS * i + 0 for i in range(MONSTER_SLOTS)]
MONSTER_INTENT_MOVE_IDX = [MONSTER_OFF + MONSTER_FIELDS * i + 1 for i in range(MONSTER_SLOTS)]

# —————— 名义类别 ——————
# 布局侧声明：**哪几列是名义类别、各能取几种值** —— `(名字, 下标列表, 取值域)`。
# ⚠️ 这里**不说消费方式**：哪几列走嵌入 / 哪几列走 one-hot、0 要不要当空槽掩掉，
#    都由 `aznet` **显式声明**（`__init__` 逐个建模块、`cat_feats` 逐项算）。两边的一致性
#    由 `aznet.__init__` 末尾那条断言按（列 / 取值域 / 顺序）逐项对账 —— 漂开即报错。
# 这张表自己的用途：算 `CAT_IDX` —— 这些列**不进标量段**，由 `aznet.cat_feats` 按类别消费。
# 归一化**不在这里**：除数写在 C++ 的赋值处（`SnapWriter::push_index` 根本不给除数参数，
# 而 `push<D>` 无默认值）⇒ Python 侧**不再持有任何除数表**，也没有"按块对齐"这一步。
CAT_SPEC = [
    ("monster_id", MONSTER_ID_IDX, MONSTER_ID_TYPES),
    ("intent_move_id", MONSTER_INTENT_MOVE_IDX, MONSTER_MOVE_ID_TYPES),
    ("input_state", [P_INPUT_STATE], INPUT_STATE_TYPES),
    ("card_select_task", [P_SELECT_TASK], CARD_SELECT_TASK_TYPES),
    ("stance", [PLAYER_ATTR_OFF], STANCE_TYPES),
    (
        "orange_pellets",
        [P_ORANGE_PELLETS],
        ORANGE_PELLETS_TYPES,
    ),
]
CAT_IDX = [i for _n, idxs, _k in CAT_SPEC for i in idxs]
# `CAT_IDX` / `SKIP_IDX` / `SEL_EXTRA_IDX` 一律**不进**标量段：
#   `CAT_IDX` 由 `aznet` 的类别段显式消费；`SKIP_IDX` 走查表（遗物池化 / 药水逐槽 id）。
# ⚠️ `SEL_EXTRA_IDX`（候选槽的 cost / costForTurn / specialData，384 列）**只由 `select_keys`
#    逐槽消费** —— 那里才需要"槽 → 属性"的对应关系。整块拼进 trunk 等于给输入投影多 384 列
#    **几乎恒 0** 的维度：非 `CARD_SELECT` 帧整槽填 0（`isValidSingleCardSelectAction` 先看
#    `inputState`），实测元素非零 0.08%、恒定 298/384。
SCALAR_IDX = [i for i in range(OBS_DIM)
              if i not in (set(CARD_ID_IDX) | set(CAT_IDX) | SKIP_IDX | set(SEL_EXTRA_IDX))]

# —————— 怪物块的「额外标量列」（与 `HAND_EXTRA_*` / `SEL_EXTRA_*` 同形）——————
# 规则：块内**除名义列外**的列 —— **由 `CAT_IDX` 推出**（偏移 = `槽 × MONSTER_FIELDS + 字段号`
# ⇒ 取模即字段号），这里不重复声明「哪几列是名义列」。
# 两个 id 列都在 `CAT_SPEC` 里 ⇒ 一列也不当标量喂（那正是 `CAT_IDX` 的设计：
# **取值大小没有意义**）；它们各自以嵌入形式进策略头的目标向量（`aznet.policy_keys`，两个嵌入
# **排在标量之前**）。
_MON_NOMINAL_FIELDS = {(i - MONSTER_OFF) % MONSTER_FIELDS for i in CAT_IDX
                       if MONSTER_OFF <= i < MONSTER_OFF + MONSTER_SLOTS * MONSTER_FIELDS}
MONSTER_EXTRA_OFF = tuple(f for f in range(MONSTER_FIELDS) if f not in _MON_NOMINAL_FIELDS)
MONSTER_EXTRA_N = len(MONSTER_EXTRA_OFF)            # 10
MONSTER_EXTRA_IDX = [MONSTER_OFF + MONSTER_FIELDS * i + f
                     for i in range(MONSTER_SLOTS) for f in MONSTER_EXTRA_OFF]

# —————— 双方状态块 ——————
PLAYER_STATUS_IDX = list(range(PLAYER_STATUS_OFF, PLAYER_STATUS_OFF + PLAYER_STATUS_LEN))
MONSTER_STATUS_IDX = list(range(MONSTER_STATUS_OFF,
                                MONSTER_STATUS_OFF + MONSTER_SLOTS * MONSTER_STATUS_LEN))

