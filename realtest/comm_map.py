"""真机（Slay the Spire + CommunicationMod）↔ 本仓引擎的**语义映射层**。

职责只有三件，全部是**纯函数**（不碰 IO / 网络 / torch）：

1. **ID 空间转换**：本仓各枚举 ↔ 游戏字符串 id。
   来源是这个仓库自己 —— `include/constants/`：
     · `SaveFileMappings.h` 里 `NLOHMANN_JSON_SERIALIZE_ENUM` 的 `{XxxId::NAME, "游戏字符串"}`
       ⇒ 卡 "Strike_R" / 遗物 "Burning Blood" / 药水 "Fire Potion" 与本仓库枚举**同源**；
     · `MonsterIds.h` 的枚举名按「去下划线小写」对齐游戏的怪物 id（"JAW_WORM" ↔ "JawWorm"）；
       15 组引擎名 ≠ 游戏类名（虱子/GREMLIN 群/强盗…）另有 `MONSTER_ALIASES` 显式对齐。
     · `PlayerStatusEffects.h` / `MonsterStatusEffects.h` 的**显示名表**对齐 power.ID（"Strength"…）；
     · `MonsterMoves.h` 的全局招式枚举按**怪物前缀**分组，组内顺序即游戏 `move_id`（1 基）。
   ⚠️ 招式的「1 基 + 组内同序」是从引擎代码（如 Book of Stabbing 的 `// 1 multi stab` 注释）与
      枚举分组推出的**约定**；对不上时会进 `notes`（缺口报告），不会静默。

2. **观测构造**：`game_state_to_snapshot(gs, map_ctx) -> (snap, notes)`
   产出**与 `sts.BattleContext.get_snapshot()` 同构**的 1076 维整型快照
   （偏移以 `azlayout` 为唯一来源；引擎模块不可用时退回内置布局并告警，守卫会在有引擎处断言两者相等）。
   `notes` 逐条记录「拿不到 / 只能近似」的字段 —— 这是本层的**契约**：宁缺毋滥、缺口可见。

3. **动作空间**：`legal_action_mask` / `idx_to_command`（96 维统一动作，语义权威在 C++ `actionToIdx`）
   与选牌屏幕（GRID / HAND_SELECT）的候选槽 ↔ `CHOOSE` 序号。

被 `comm_battle.py`（实时客户端）与 `_chk_comm.py`（离线守卫）共同使用。
"""

from __future__ import annotations

import argparse
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # realtest/ 上一级 = 仓库根
if ROOT not in sys.path:            # `azlayout`（及下游 `slaythespire`）在仓库根 ⇒ 保证可导入
    sys.path.insert(0, ROOT)
CONST = os.path.join(ROOT, "include", "constants")


# ================================================================ 头文件解析

def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def enum_names(header: str, enum: str) -> list[str]:
    """按**声明顺序**取 `enum class <enum> ... { ... }` 的成员名（下标 = 枚举值）。"""
    m = re.search(rf"enum\s+class\s+{enum}\s*(?::[^{{]*)?\{{(.*?)\}};", header, re.S)
    if not m:
        raise RuntimeError(f"{enum}: 找不到枚举定义")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    names = re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*(?:=[^,]*)?,?\s*$", body, re.M)
    return names


def string_table(header: str, table: str) -> list[str]:
    """取 `static constexpr const char* const <table>[] = {...}` 的字符串序列。

    ⚠️ `=` 可缺（`playerStatusStrings[] { ... }` 就是无等号的写法）。
    """
    m = re.search(rf"{table}\s*\[\s*\]\s*=?\s*\{{(.*?)\}};", header, re.S)
    if not m:
        raise RuntimeError(f"{table}: 找不到字符串表")
    return re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))


def _norm(s: str) -> str:
    """归一化：小写、只留字母数字（"JawWorm"/"JAW_WORM" ⇒ "jawworm"）。"""
    return re.sub(r"[^a-z0-9]", "", s.lower())


def _savefile_pairs(text: str, enum: str) -> dict[str, int]:
    """`NLOHMANN_JSON_SERIALIZE_ENUM(<enum>, {...})` 块 → {游戏字符串: 枚举名}。

    ⚠️ 结尾的 `;` **可以没有**（本仓的 CardId 块就写作 `})`）。
    """
    m = re.search(rf"NLOHMANN_JSON_SERIALIZE_ENUM\(\s*{enum}\s*,\s*\{{(.*?)\}}\s*\)\s*;?",
                  text, re.S)
    if not m:
        raise RuntimeError(f"SaveFileMappings: 找不到 {enum} 的映射块")
    out = {}
    for name, s in re.findall(rf"\{{\s*{enum}::(\w+)\s*,\s*\"([^\"]*)\"\s*\}}", m.group(1)):
        out[s] = name
    return out


def _build_ids():
    cards_h = _read(os.path.join(CONST, "Cards.h"))
    save_h = _read(os.path.join(CONST, "SaveFileMappings.h"))

    card_enum = enum_names(cards_h, "CardId")
    card_int = {n: i for i, n in enumerate(card_enum)}
    card_by_str = {s: card_int[n] for s, n in _savefile_pairs(save_h, "CardId").items()
                   if n in card_int}

    relic_h = _read(os.path.join(CONST, "Relics.h"))
    relic_int = {n: i for i, n in enumerate(enum_names(relic_h, "RelicId"))}
    relic_by_str = {s: relic_int[n] for s, n in _savefile_pairs(save_h, "RelicId").items()
                    if n in relic_int}
    if len(relic_int) == 0:
        raise RuntimeError("Relics.h 枚举解析为空")

    potion_h = _read(os.path.join(CONST, "Potions.h"))
    potion_int = {n: i for i, n in enumerate(enum_names(potion_h, "Potion"))}
    potion_by_str = {s: potion_int[n] for s, n in _savefile_pairs(save_h, "Potion").items()
                     if n in potion_int}

    mon_h = _read(os.path.join(CONST, "MonsterIds.h"))
    mon_enum = enum_names(mon_h, "MonsterId")
    monster_by_str = {_norm(n): i for i, n in enumerate(mon_enum)}          # "JawWorm"→JAW_WORM

    ps_h = _read(os.path.join(CONST, "PlayerStatusEffects.h"))
    ps_disp = string_table(ps_h, "playerStatusStrings")
    ps_enum = string_table(ps_h, "playerStatusEnumStrings")
    player_status_by_str = {}
    for tbl in (ps_disp, ps_enum):
        for i, s in enumerate(tbl):
            if s and s != "INVALID":
                player_status_by_str.setdefault(_norm(s), i)

    ms_h = _read(os.path.join(CONST, "MonsterStatusEffects.h"))
    ms_disp = string_table(ms_h, "enemyStatusStrings")          # 显示名（"Strength"...）
    ms_enum = string_table(ms_h, "monsterStatusEnumStrings")
    monster_status_by_str = {}
    for tbl in (ms_disp, ms_enum):
        for i, s in enumerate(tbl):
            if s and s != "INVALID":
                monster_status_by_str.setdefault(_norm(s), i)

    mm_h = _read(os.path.join(CONST, "MonsterMoves.h"))
    move_enum = enum_names(mm_h, "MonsterMoveId")
    # 招式名 = "<怪物枚举名>_<招式>"（如 CULTIST_DARK_STRIKE）⇒ 按**最长前缀**归到怪物名下，
    # 组内保持枚举顺序 = 游戏 `move_id`（1 基）顺序。
    moves_by_monster: dict[int, list[int]] = {}
    mon_names = {i: n for i, n in enumerate(mon_enum)}
    for i, name in enumerate(move_enum):
        best = None
        for mid, mname in mon_names.items():
            if mid == 0 or not name.startswith(mname + "_"):
                continue
            if best is None or len(mname) > len(mon_names[best]):
                best = mid
        if best is not None:
            moves_by_monster.setdefault(best, []).append(i)
    return dict(card_by_str=card_by_str, relic_by_str=relic_by_str,
                potion_by_str=potion_by_str, monster_by_str=monster_by_str,
                player_status_by_str=player_status_by_str,
                monster_status_by_str=monster_status_by_str,
                move_enum=move_enum, moves_by_monster=moves_by_monster)


_IDS = _build_ids()
CARD_BY_STR = _IDS["card_by_str"]
RELIC_BY_STR = _IDS["relic_by_str"]
POTION_BY_STR = _IDS["potion_by_str"]
MONSTER_BY_STR = _IDS["monster_by_str"]

# 引擎枚举名 ≠ 游戏类名（规范化后仍对不上）的显式对齐表（2026-09-23 用游戏 jar 的怪物类全量核对）：
# 键 = 游戏类名（小写去下划线），值 = 引擎枚举名（同规范）。
MONSTER_ALIASES = {
    # 虱子（双重证据：精灵图 LouseNormal→louseRed / LouseDefensive→louseGreen；
    #        HP 区间 10-15/11-16 ↔ RED_LOUSE、11-17/12-18 ↔ GREEN_LOUSE 逐值吻合）
    "lousenormal": "redlouse", "lousedefensive": "greenlouse",
    "fuzzylousenormal": "redlouse", "fuzzylousedefensive": "greenlouse",
    "gremlinfat": "fatgremlin", "gremlinwarrior": "madgremlin",
    "gremlinthief": "sneakygremlin", "gremlintsundere": "shieldgremlin",
    "banditbear": "bear", "banditleader": "romeo", "banditpointy": "pointy",
    "champ": "thechamp", "maw": "themaw", "healer": "mystic",
    "slaverblue": "blueslaver", "slaverred": "redslaver", "snakedagger": "dagger",
    "slaverboss": "taskmaster",                        # Taskmaster 的运行时 id
    "banditchild": "pointy",                          # 三强盗的 Pointy（运行时 id=BanditChild）
}


def monster_idx(monster_id: str) -> int:
    """游戏怪物 id（类名）→ 引擎 `MonsterId` 下标；未知 ⇒ 0。"""
    k = _norm(monster_id or "").replace(" ", "")   # 真机 id 偶带空格（"Shelled Parasite"/"Apology Slime"）
    v = MONSTER_BY_STR.get(k)
    if v is None:
        v = MONSTER_BY_STR.get(MONSTER_ALIASES.get(k, ""), 0)
    return v
PLAYER_STATUS_BY_STR = _IDS["player_status_by_str"]
MONSTER_STATUS_BY_STR = _IDS["monster_status_by_str"]
MOVE_ENUM = _IDS["move_enum"]

# ⚠️ 游戏内部 power id ≠ 引擎显示名（2026-09-23 真机 SlimeBoss 战抓到）：
#   · "Weakened" —— 游戏 `WeakPower` 的**运行时 id**（显示名才是 "Weak"）⇒ 引擎两侧都叫
#     "Weak"，必须走别名；不打这个补丁，玩家施加的虚弱在快照里整块丢失。
#   · "Split" —— 史莱姆 Boss 的预告 power（amount=-1）；**引擎没有这个名字**（分割在引擎里
#     是 move，不是 power）⇒ 保持显式缺口（与训练一致，不硬映射）。
_STATUS_ALIASES = {"weakened": "weak"}
for _tab in (PLAYER_STATUS_BY_STR, MONSTER_STATUS_BY_STR):
    for _a, _c in _STATUS_ALIASES.items():
        if _c in _tab and _a not in _tab:
            _tab[_a] = _tab[_c]

# 权威招式编号表（2026-09-23）：{引擎 MonsterId 下标: {游戏 move_id: 引擎招式枚举名}}。
# 游戏 move_id = CommunicationMod 的 `moveInfo.nextMove` 字节，编号**不保证 1 基连续**
# （虱子 {3,4}、邪教徒 {1,3}…）。本表由游戏 jar 反编译源码（_decomp/）逐类核对生成。
MOVE_ID_TABLE = {
    1: {1: 'ACID_SLIME_L_CORROSIVE_SPIT', 2: 'ACID_SLIME_L_TACKLE', 3: 'ACID_SLIME_L_SPLIT', 4: 'ACID_SLIME_L_LICK'},
    2: {1: 'ACID_SLIME_M_CORROSIVE_SPIT', 2: 'ACID_SLIME_M_TACKLE', 4: 'ACID_SLIME_M_LICK'},
    3: {1: 'ACID_SLIME_S_TACKLE', 2: 'ACID_SLIME_S_LICK'},
    4: {1: 'AWAKENED_ONE_SLASH', 2: 'AWAKENED_ONE_SOUL_STRIKE', 3: 'AWAKENED_ONE_REBIRTH', 5: 'AWAKENED_ONE_DARK_ECHO', 6: 'AWAKENED_ONE_SLUDGE', 8: 'AWAKENED_ONE_TACKLE'},
    5: {2: 'BEAR_BEAR_HUG'},
    6: {1: 'BLUE_SLAVER_STAB', 4: 'BLUE_SLAVER_RAKE'},
    7: {1: 'BOOK_OF_STABBING_MULTI_STAB', 2: 'BOOK_OF_STABBING_SINGLE_STAB'},
    8: {1: 'BRONZE_AUTOMATON_FLAIL', 2: 'BRONZE_AUTOMATON_HYPER_BEAM', 3: 'BRONZE_AUTOMATON_STUNNED', 4: 'BRONZE_AUTOMATON_SPAWN_ORBS', 5: 'BRONZE_AUTOMATON_BOOST'},
    9: {2: 'BRONZE_ORB_SUPPORT_BEAM', 3: 'BRONZE_ORB_STASIS'},
    10: {1: 'BYRD_PECK', 3: 'BYRD_SWOOP', 4: 'BYRD_STUNNED', 5: 'BYRD_HEADBUTT', 6: 'BYRD_CAW'},
    11: {1: 'CENTURION_SLASH', 3: 'CENTURION_FURY'},
    12: {1: 'CHOSEN_ZAP', 2: 'CHOSEN_DRAIN', 3: 'CHOSEN_DEBILITATE', 4: 'CHOSEN_HEX', 5: 'CHOSEN_POKE'},
    13: {1: 'CORRUPT_HEART_BLOOD_SHOTS', 3: 'CORRUPT_HEART_DEBILITATE'},
    14: {1: 'CULTIST_DARK_STRIKE', 3: 'CULTIST_INCANTATION'},
    15: {1: 'DAGGER_STAB', 2: 'DAGGER_EXPLODE'},
    16: {1: 'DARKLING_CHOMP', 2: 'DARKLING_HARDEN', 3: 'DARKLING_NIP', 5: 'DARKLING_REINCARNATE'},
    17: {0: 'DECA_BEAM', 2: 'DECA_SQUARE_OF_PROTECTION'},
    18: {0: 'DONU_BEAM'},
    19: {1: 'EXPLODER_SLAM', 2: 'EXPLODER_EXPLODE'},
    20: {2: 'FAT_GREMLIN_SMASH'},
    21: {1: 'FUNGI_BEAST_BITE', 2: 'FUNGI_BEAST_GROW'},
    22: {1: 'GIANT_HEAD_GLARE', 2: 'GIANT_HEAD_IT_IS_TIME', 3: 'GIANT_HEAD_COUNT'},
    23: {3: 'GREEN_LOUSE_BITE', 4: 'GREEN_LOUSE_SPIT_WEB'},
    24: {2: 'GREMLIN_LEADER_RALLY', 3: 'GREMLIN_LEADER_ENCOURAGE', 4: 'GREMLIN_LEADER_STAB'},
    25: {2: 'GREMLIN_NOB_SKULL_BASH', 3: 'GREMLIN_NOB_BELLOW'},
    26: {1: 'GREMLIN_WIZARD_ULTIMATE_BLAST', 2: 'GREMLIN_WIZARD_CHARGING'},
    27: {1: 'HEXAGHOST_DIVIDER', 2: 'HEXAGHOST_TACKLE', 3: 'HEXAGHOST_INFLAME', 4: 'HEXAGHOST_SEAR', 5: 'HEXAGHOST_ACTIVATE', 6: 'HEXAGHOST_INFERNO'},
    28: {1: 'JAW_WORM_CHOMP', 2: 'JAW_WORM_BELLOW', 3: 'JAW_WORM_THRASH'},
    29: {1: 'LAGAVULIN_SIPHON_SOUL', 3: 'LAGAVULIN_ATTACK', 5: 'LAGAVULIN_SLEEP'},  # 4/6 = STUN/OPEN_NATURAL，引擎无对应招式名
    30: {1: 'LOOTER_MUG', 2: 'LOOTER_SMOKE_BOMB'},
    31: {1: 'MAD_GREMLIN_SCRATCH'},
    32: {1: 'MUGGER_MUG', 2: 'MUGGER_SMOKE_BOMB'},
    33: {1: 'MYSTIC_ATTACK_DEBUFF', 2: 'MYSTIC_HEAL', 3: 'MYSTIC_BUFF'},
    34: {3: 'NEMESIS_SCYTHE'},
    35: {1: 'ORB_WALKER_LASER', 2: 'ORB_WALKER_CLAW'},
    36: {1: 'POINTY_ATTACK'},
    37: {3: 'RED_LOUSE_BITE', 4: 'RED_LOUSE_GROW'},
    38: {1: 'RED_SLAVER_STAB', 2: 'RED_SLAVER_ENTANGLE', 3: 'RED_SLAVER_SCRAPE'},
    39: {1: 'REPTOMANCER_SNAKE_STRIKE', 3: 'REPTOMANCER_BIG_BITE'},
    40: {1: 'REPULSOR_REPULSE', 2: 'REPULSOR_BASH'},
    41: {2: 'ROMEO_MOCK'},
    42: {3: 'SENTRY_BOLT', 4: 'SENTRY_BEAM'},
    43: {1: 'SHELLED_PARASITE_FELL', 2: 'SHELLED_PARASITE_DOUBLE_STRIKE', 4: 'SHELLED_PARASITE_STUNNED'},
    44: {1: 'SHIELD_GREMLIN_PROTECT', 2: 'SHIELD_GREMLIN_SHIELD_BASH'},
    45: {1: 'SLIME_BOSS_SLAM', 2: 'SLIME_BOSS_PREPARING', 3: 'SLIME_BOSS_SPLIT', 4: 'SLIME_BOSS_GOOP_SPRAY'},  # 2/4 于 2026-09-23 真机战后补齐（_decomp/src/SlimeBoss.java）
    46: {2: 'SNAKE_PLANT_ENFEEBLING_SPORES'},
    47: {1: 'SNEAKY_GREMLIN_PUNCTURE'},
    48: {1: 'SNECKO_PERPLEXING_GLARE', 2: 'SNECKO_BITE'},
    49: {1: 'SPHERIC_GUARDIAN_SLAM', 2: 'SPHERIC_GUARDIAN_ACTIVATE', 3: 'SPHERIC_GUARDIAN_HARDEN', 4: 'SPHERIC_GUARDIAN_ATTACK_DEBUFF'},
    50: {1: 'SPIKER_CUT', 2: 'SPIKER_SPIKE'},
    51: {1: 'SPIKE_SLIME_L_FLAME_TACKLE', 3: 'SPIKE_SLIME_L_SPLIT', 4: 'SPIKE_SLIME_L_LICK'},
    52: {1: 'SPIKE_SLIME_M_FLAME_TACKLE', 4: 'SPIKE_SLIME_M_LICK'},
    53: {1: 'SPIKE_SLIME_S_TACKLE'},
    54: {1: 'SPIRE_GROWTH_QUICK_TACKLE', 2: 'SPIRE_GROWTH_CONSTRICT', 3: 'SPIRE_GROWTH_SMASH'},
    55: {1: 'SPIRE_SHIELD_BASH', 2: 'SPIRE_SHIELD_FORTIFY', 3: 'SPIRE_SHIELD_SMASH'},
    56: {1: 'SPIRE_SPEAR_BURN_STRIKE', 2: 'SPIRE_SPEAR_PIERCER', 3: 'SPIRE_SPEAR_SKEWER'},
    57: {2: 'TASKMASTER_SCOURING_WHIP'},
    58: {1: 'THE_CHAMP_HEAVY_SLASH', 2: 'THE_CHAMP_DEFENSIVE_STANCE', 3: 'THE_CHAMP_EXECUTE', 4: 'THE_CHAMP_FACE_SLAP', 5: 'THE_CHAMP_GLOAT', 6: 'THE_CHAMP_TAUNT', 7: 'THE_CHAMP_ANGER'},
    59: {1: 'THE_COLLECTOR_SPAWN', 2: 'THE_COLLECTOR_FIREBALL', 3: 'THE_COLLECTOR_BUFF', 4: 'THE_COLLECTOR_MEGA_DEBUFF'},
    60: {2: 'THE_GUARDIAN_FIERCE_BASH', 3: 'THE_GUARDIAN_ROLL_ATTACK', 4: 'THE_GUARDIAN_TWIN_SLAM', 5: 'THE_GUARDIAN_WHIRLWIND', 7: 'THE_GUARDIAN_VENT_STEAM'},
    61: {2: 'THE_MAW_ROAR', 3: 'THE_MAW_SLAM', 4: 'THE_MAW_DROOL', 5: 'THE_MAW_NOM'},
    62: {2: 'TIME_EATER_REVERBERATE', 3: 'TIME_EATER_RIPPLE', 4: 'TIME_EATER_HEAD_SLAM', 5: 'TIME_EATER_HASTE'},
    63: {1: 'TORCH_HEAD_TACKLE'},
    64: {1: 'TRANSIENT_ATTACK'},
    65: {0: 'WRITHING_MASS_STRONG_STRIKE', 1: 'WRITHING_MASS_MULTI_STRIKE', 2: 'WRITHING_MASS_FLAIL', 3: 'WRITHING_MASS_WITHER', 4: 'WRITHING_MASS_IMPLANT'},
}
MOVES_BY_MONSTER = _IDS["moves_by_monster"]

POTION_SLOT_ID = "Potion Slot"          # 空药水槽的 id（Spire 原版行为）


# ================================================================ 布局（与 C++ Snap 同步）

# ⚠️ 唯一来源是 C++（`bindings/slaythespire.cpp` 的 `Snap`）。这里的内置副本只是
#    **离线回退**：本机 `slaythespire` 模块过期/缺失时（真机测试机常见），
#    `azlayout` 根本 import 不了。有引擎时 `cross_check_layout()` 会逐项断言相等。
_FALLBACK = {
    "len": 1076,
    "player_off": 0,
    "counts_off": 8,
    "hand_off": 13, "hand_slots": 10, "hand_fields": 4,
    "draw_off": 53, "discard_off": 113, "exhaust_off": 173, "pile_slots": 60, "pile_fields": 1,
    "deck_off": 233, "deck_slots": 64, "deck_fields": 1,
    "monster_off": 297, "monster_slots": 5, "monster_fields": 8,
    "player_attr_off": 337,
    "player_counter_off": 340,
    "player_status_off": 348, "player_status_len": 87,
    "monster_status_off": 435, "monster_status_len": 43,
    "card_select_off": 650, "card_select_slots": 60, "card_select_fields": 3,
    "select_meta_off": 830, "select_meta_len": 4,
    "run_ctx_off": 834, "run_ctx_len": 10,
    "relic_off": 844, "relic_len": 181,
    "potion_meta_off": 1025, "potion_bag_off": 1027, "potion_types": 44,
    "potion_slot_off": 1071, "potion_slot_len": 5,
}

LAYOUT: dict[str, int] = dict(_FALLBACK)
LAYOUT_SOURCE = "fallback"
LAYOUT_WARN = ""
try:                                                                            # noqa: SIM105
    import azlayout as _al                                                     # noqa: N813
    LAYOUT = dict(_al.LAYOUT)
    LAYOUT_SOURCE = "azlayout"
except Exception as e:                                                          # noqa: BLE001
    LAYOUT_WARN = (f"⚠️ azlayout 不可用（{type(e).__name__}: {e}）—— 使用内置布局副本。"
                   f"要在本机做严格校验，请先重建 slaythespire 模块（见 realtest/README.md）。")

OBS_DIM = int(LAYOUT["len"])
HAND_OFF, HAND_SLOTS, HAND_FIELDS = LAYOUT["hand_off"], LAYOUT["hand_slots"], LAYOUT["hand_fields"]
PILE_OFFS = (LAYOUT["draw_off"], LAYOUT["discard_off"], LAYOUT["exhaust_off"])
DECK_OFF, DECK_SLOTS = LAYOUT["deck_off"], LAYOUT["deck_slots"]
MON_OFF, MON_SLOTS, MON_FIELDS = LAYOUT["monster_off"], LAYOUT["monster_slots"], LAYOUT["monster_fields"]
SEL_OFF, SEL_SLOTS, SEL_FIELDS = (LAYOUT["card_select_off"], LAYOUT["card_select_slots"],
                                  LAYOUT["card_select_fields"])
META_OFF = LAYOUT["select_meta_off"]
RUN_CTX_OFF = LAYOUT["run_ctx_off"]
RELIC_OFF, RELIC_LEN = LAYOUT["relic_off"], LAYOUT["relic_len"]
POTION_META_OFF = LAYOUT["potion_meta_off"]
POTION_BAG_OFF, POTION_TYPES = LAYOUT["potion_bag_off"], LAYOUT["potion_types"]
POTION_SLOT_OFF, POTION_SLOTS = LAYOUT["potion_slot_off"], LAYOUT["potion_slot_len"]

IN_NORMAL, IN_CARD_SELECT = 1, 2        # InputState::PLAYER_NORMAL / CARD_SELECT

# 本仓 Room 枚举前 7 类（`include/constants/Rooms.h`）：SHOP, REST, EVENT, ELITE, MONSTER, TREASURE, BOSS
ROOM_BY_SYMBOL = {"$": 0, "R": 1, "?": 2, "E": 3, "M": 4, "T": 5}
BOSS_ROOM = 6

# 每回合能量：基础 3 + 加能量的遗物 / 能力（近似；游戏 JSON 不直接给）
_ENERGY_RELICS = {"Coffee Dripper", "Cursed Key", "Ectoplasm", "Fusion Hammer",
                  "Philosopher's Stone", "Runic Dome", "Sozu", "Velvet Choker",
                  "Busted Crown", "Mark of Pain"}


def cross_check_layout() -> list[str]:
    """内置布局 ↔ `azlayout` 逐项断言；返回不一致列表（空 = 全等）。"""
    if LAYOUT_SOURCE != "azlayout":
        return ["azlayout 不可用，未做交叉校验"]
    bad = []
    for k, v in _FALLBACK.items():
        got = LAYOUT.get(k)
        if got is None:
            bad.append(f"{k}: azlayout 缺键（引擎过旧？）")
        elif int(got) != v:
            bad.append(f"{k}: 内置 {v} vs azlayout {got}")
    return bad


# ================================================================ 小工具

def _get(d, *keys, default=None):
    cur = d
    for k in keys:
        if not isinstance(cur, dict) or k not in cur:
            return default
        cur = cur[k]
    return cur


def _card_id(name: str, notes: list[str], where: str) -> int:
    v = CARD_BY_STR.get(name)
    if v is None:
        notes.append(f"缺口：未知卡牌 id {name!r}（={where}）")
        return 0
    return v


def map_move(monster_id: str, game_move_id, notes: list[str]) -> int:
    """游戏 (怪物 id, move_id) → 本仓全局 MonsterMoveId。

    `move_id` 是游戏 `setMove((byte)N, …)` 的原始字节（CommunicationMod 直传
    `moveInfo.nextMove`）——**不是**“第几个招式”，不保证 1 基连续。
    优先查 `MOVE_ID_TABLE`（反编译源码逐类核对得到的权威表）；
    未入表怪物才回退“前缀块顺序当 1 基”的旧假设，并标注。
    对不上（0/缺失/超界）⇒ 0 + 缺口记录。
    """
    if game_move_id is None:
        return 0
    mid = monster_idx(monster_id)
    if mid == 0:
        notes.append(f"缺口：未知怪物 id {monster_id!r}")
        return 0
    j = int(game_move_id)
    tbl = MOVE_ID_TABLE.get(mid)
    if tbl is not None:
        name = tbl.get(j)
        if name is None:
            notes.append(f"缺口：{monster_id} 的 move_id={j} 不在编号表 {sorted(tbl)} —— 记为 0")
            return 0
        return MOVE_ENUM.index(name)
    block = MOVES_BY_MONSTER.get(mid) or []
    if block and 1 <= j <= len(block):
        return block[j - 1]
    notes.append(f"缺口：move_id={j} 超出 {monster_id} 的招式块（共 {len(block)} 条）—— 记为 0")
    return 0


# ================================================================ 观测构造

def game_state_to_snapshot(gs: dict, map_ctx: dict | None = None) -> tuple[np.ndarray, list[str]]:
    """CommunicationMod 的 `game_state` → 1076 维引擎快照。

    `map_ctx`（可选）: {"symbol": 当前节点符号, "children": [子节点符号, ...]}
    —— 由客户端在选地图时记录；缺失时 run 上下文直方图写 0（进缺口）。
    """
    s = np.zeros(OBS_DIM, dtype=np.int64)
    notes: list[str] = []

    combat = gs.get("combat_state") or {}
    player = combat.get("player") or {}
    hand = list(combat.get("hand") or [])
    monsters = list(combat.get("monsters") or [])
    screen_type = gs.get("screen_type") or "NONE"
    in_select = screen_type in ("GRID", "HAND_SELECT")

    # ---- 玩家块 8 项 --------------------------------------------------------
    s[0] = int(gs.get("current_hp") or 0)
    s[1] = int(gs.get("max_hp") or 0)
    s[2] = int(player.get("block") or 0)
    s[3] = int(player.get("energy") or 0)
    s[4] = int(combat.get("turn") or 0)
    s[5] = IN_CARD_SELECT if in_select else IN_NORMAL
    s[6] = 0                                    # cardSelectTask：真机不给 ⇒ 见缺口
    s[7] = 0                                    # outcome：进真机时不带终局语义
    if in_select:
        notes.append("近似：真机选牌屏幕不给 cardSelectTask（引擎的 21 类任务枚举）—— 写 0")

    # ---- 计数块 -------------------------------------------------------------
    alive = [m for m in monsters if not m.get("is_gone")]
    s[8] = len(alive)
    s[9] = len(hand)
    for k, key in enumerate(("draw_pile", "discard_pile", "exhaust_pile")):
        s[9 + 1 + k] = len(combat.get(key) or [])

    # ---- 手牌 10×4 ----------------------------------------------------------
    if len(hand) > HAND_SLOTS:
        notes.append(f"缺口：手牌 {len(hand)} 张 > 快照槽位 {HAND_SLOTS} —— 多出的丢弃")
    for i, c in enumerate(hand[:HAND_SLOTS]):
        cid = _card_id(c.get("id", ""), notes, "hand")
        up = 1 if int(c.get("upgrades") or 0) > 0 else 0
        b = HAND_OFF + HAND_FIELDS * i
        s[b] = cid * 2 + up
        s[b + 1] = int(c.get("cost") or 0)
        s[b + 2] = int(c.get("misc") or 0)
        s[b + 3] = 1 if c.get("is_playable") else 0

    # ---- 三个牌堆（各 60 槽）-----------------------------------------------
    for off, key in zip(PILE_OFFS, ("draw_pile", "discard_pile", "exhaust_pile")):
        pile = list(combat.get(key) or [])
        if len(pile) > 60:
            notes.append(f"缺口：{key} {len(pile)} 张 > 60 槽（引擎用堆顶优先截断，这里同口径）")
        for j, c in enumerate(pile[:60]):
            cid = _card_id(c.get("id", ""), notes, key)
            s[off + j] = cid * 2 + (1 if int(c.get("upgrades") or 0) > 0 else 0)
    for key in ("draw_pile", "discard_pile", "exhaust_pile"):
        if len(combat.get(key) or []) > 60:
            notes.append(f"近似：{key} 超过 60 时真机给不出「堆顶 60 张」的原文顺序（本仓只给集合，不受影响）")

    # ---- 总牌组 64 槽（masterDeck，run 级不变量）----------------------------
    deck = list(gs.get("deck") or [])
    if len(deck) > DECK_SLOTS:
        notes.append(f"缺口：牌组 {len(deck)} 张 > {DECK_SLOTS} 槽（引擎上限同值，超出即异常）")
    for j, c in enumerate(deck[:DECK_SLOTS]):
        s[DECK_OFF + j] = _card_id(c.get("id", ""), notes, "deck") * 2 + \
                          (1 if int(c.get("upgrades") or 0) > 0 else 0)

    # ---- 怪物 5×8 ----------------------------------------------------------
    for i, m in enumerate(monsters[:MON_SLOTS]):
        b = MON_OFF + MON_FIELDS * i
        s[b] = monster_idx(m.get("id", ""))
        if s[b] == 0 and m.get("id"):
            notes.append(f"缺口：未知怪物 id {m.get('id')!r}")
        s[b + 1] = map_move(m.get("id", ""), m.get("move_id"), notes)
        s[b + 2] = int(m.get("current_hp") or 0)
        s[b + 3] = int(m.get("max_hp") or 0)
        s[b + 4] = int(m.get("block") or 0)
        mdead = bool(m.get("is_gone")) or bool(m.get("half_dead"))
        s[b + 5] = 0 if mdead else 1
        s[b + 6] = int(m.get("move_adjusted_damage") or 0)
        s[b + 7] = int(m.get("move_hits") or 0)
    if len(monsters) > MON_SLOTS:
        notes.append(f"缺口：怪物 {len(monsters)} 只 > {MON_SLOTS} 槽")
    if monsters and all("move_id" not in m for m in monsters):
        notes.append("缺口：没有 move_id（戴 Runic Dome / 无意图）—— 意图伤害/次数也为 0")

    # ---- 玩家属性 3 项 ------------------------------------------------------
    s[337] = 0                                  # stance：只对观者有意义（本测试打铁甲，写 0）
    s[338] = len(player.get("orbs") or [])      # orbSlots
    s[339] = 3 + (1 if any(r.get("id") in _ENERGY_RELICS for r in gs.get("relics") or []) else 0) \
             + (1 if any(_norm(p.get("id", "")) == "berserk" for p in player.get("powers") or []) else 0)

    # ---- 回合计数 8 项 ------------------------------------------------------
    rel = {r.get("id"): int(r.get("counter") or 0) for r in gs.get("relics") or []}
    s[340] = 0                                  # cardsPlayedThisTurn：JSON 不给
    s[341] = 0
    s[342] = 0
    s[343] = int(combat.get("cards_discarded_this_turn") or 0)
    s[344] = 5                                  # cardDrawPerTurn：基础 5（遗物加成不跟踪）
    s[345] = int(combat.get("times_damaged") or 0)
    s[346] = rel.get("Pen Nib", 0)
    s[347] = rel.get("Happy Flower", 0)
    if combat:
        notes.append("缺口：cardsPlayed/attacksPlayed/skillsPlayedThisTurn 真机不给（写 0）")

    # ---- 玩家状态 87 项（下标 = PlayerStatus 枚举）-------------------------
    for p in player.get("powers") or []:
        idx = PLAYER_STATUS_BY_STR.get(_norm(p.get("id", "")))
        if idx is None or idx >= LAYOUT["player_status_len"]:
            notes.append(f"缺口：未映射的玩家 power {p.get('id')!r}")
            continue
        s[LAYOUT["player_status_off"] + idx] = int(p.get("amount") or 0)

    # ---- 怪物状态 5×43 ------------------------------------------------------
    for i, m in enumerate(monsters[:MON_SLOTS]):
        base = LAYOUT["monster_status_off"] + LAYOUT["monster_status_len"] * i
        for p in m.get("powers") or []:
            idx = MONSTER_STATUS_BY_STR.get(_norm(p.get("id", "")))
            if idx is None or idx >= LAYOUT["monster_status_len"]:
                notes.append(f"缺口：未映射的怪物 power {p.get('id')!r}")
                continue
            s[base + idx] = int(p.get("amount") or 0)

    # ---- 选牌区（非选牌帧恒 0 —— 与引擎的置零纪律一致）----------------------
    if in_select:
        scr = gs.get("screen_state") or {}
        cand = list(scr.get("hand") or scr.get("cards") or []) if screen_type == "HAND_SELECT" \
            else list(scr.get("cards") or [])
        for k, c in enumerate(cand[:SEL_SLOTS]):
            b = SEL_OFF + SEL_FIELDS * k
            s[b] = _card_id(c.get("id", ""), notes, "select") * 2 + \
                   (1 if int(c.get("upgrades") or 0) > 0 else 0)
            s[b + 1] = int(c.get("cost") or 0)
            s[b + 2] = int(c.get("misc") or 0)
        s[META_OFF] = min(len(cand), SEL_SLOTS)
        if screen_type == "HAND_SELECT":
            s[META_OFF + 1] = int(scr.get("max_cards") or 0)
            s[META_OFF + 2] = 1 if scr.get("can_pick_zero") else 0
            s[META_OFF + 3] = 0
        else:
            s[META_OFF + 1] = int(scr.get("num_cards") or 0)
            s[META_OFF + 2] = 0
            s[META_OFF + 3] = 1 if scr.get("any_number") else 0

    # ---- run 上下文 10 项 ---------------------------------------------------
    s[RUN_CTX_OFF] = int(gs.get("act") or 0)
    s[RUN_CTX_OFF + 1] = int(gs.get("floor") or 0)
    s[RUN_CTX_OFF + 2] = int(gs.get("gold") or 0)
    if map_ctx and map_ctx.get("children") is not None:
        for sym in map_ctx["children"][:7]:
            r = ROOM_BY_SYMBOL.get(sym)
            if r is None:
                notes.append(f"缺口：地图节点符号 {sym!r} 未映射（跳过）")
                continue
            s[RUN_CTX_OFF + 3 + r] += 1
    else:
        notes.append("缺口：无地图上下文（未记录当前地图节点）⇒ 下一行房间直方图写 0")

    # ---- 遗物 181 位图 ------------------------------------------------------
    for r in gs.get("relics") or []:
        idx = RELIC_BY_STR.get(r.get("id", ""))
        if idx is None or idx >= RELIC_LEN:
            notes.append(f"缺口：未知遗物 id {r.get('id')!r}")
            continue
        s[RELIC_OFF + idx] = 1

    # ---- 药水：[count, capacity] + 袋子 44 + 逐槽 5 -------------------------
    potions = list(gs.get("potions") or [])
    real = [p for p in potions if p.get("id") != POTION_SLOT_ID]
    s[POTION_META_OFF] = len(real)
    s[POTION_META_OFF + 1] = len(potions)       # 容量 = 列表长度（空槽也在列表里）
    for p in real:
        idx = POTION_BY_STR.get(p.get("id", ""))
        if idx is None or idx >= POTION_TYPES:
            notes.append(f"缺口：未知药水 id {p.get('id')!r}")
            continue
        s[POTION_BAG_OFF + idx] = 1
    for k in range(min(POTION_SLOTS, len(potions))):
        pid = potions[k].get("id", "")
        if pid == POTION_SLOT_ID:
            continue
        idx = POTION_BY_STR.get(pid)
        if idx is None:
            notes.append(f"缺口：未知药水 id {pid!r}（槽 {k}）")
            continue
        s[POTION_SLOT_OFF + k] = idx
    if potions and "Potion Slot" not in [p.get("id") for p in potions]:
        notes.append("近似：药水列表里没有 'Potion Slot'（全满），容量按列表长度")
    return s, notes


# ================================================================ 动作空间

HAND_ACTION_STRIDE, POTION_ACTION_STRIDE = 6, 7
END_TURN_IDX = 95


def legal_action_mask(gs: dict, available_commands=None) -> np.ndarray:
    """当前状态的合法动作掩码（**恒为 96 维统一动作空间**；选牌屏幕也只是把合法位
    放进前 `SEL_SLOTS`(=60) 个候选槽 —— 消费端拿 `select_head` 输出（60 维）时必须
    把掩码截到 60 再掩；2026-09-23 真机在「96 vs 60」上修过一次）。

    `available_commands`：协议**消息顶层**的可用命令表 —— 真机里 END 的合法性只在
    这里（`game_state` 内部没有该字段；2026-09-23 真机首战踩坑：曾误从 gs 内取，
    能量 0 时掩码全 0 死循环）。不传时回退 gs 内查找，仅为兼容旧 mock。
    """
    mask = np.zeros(96, dtype=bool)
    screen_type = gs.get("screen_type") or "NONE"
    if screen_type in ("GRID", "HAND_SELECT"):
        scr = gs.get("screen_state") or {}
        cand = (scr.get("hand") or scr.get("cards") or []) if screen_type == "HAND_SELECT" \
            else (scr.get("cards") or [])
        for k in range(min(len(cand), SEL_SLOTS)):
            mask[k] = True
        return mask

    combat = gs.get("combat_state") or {}
    hand = list(combat.get("hand") or [])
    monsters = list(combat.get("monsters") or [])
    targetable = [i for i, m in enumerate(monsters)
                  if not m.get("is_gone") and not m.get("half_dead")]
    for k, c in enumerate(hand[:HAND_SLOTS]):
        if not c.get("is_playable"):
            continue
        if c.get("has_target"):
            for i in targetable[:5]:
                mask[k * HAND_ACTION_STRIDE + 1 + i] = True
        else:
            mask[k * HAND_ACTION_STRIDE] = True
    for k, p in enumerate((gs.get("potions") or [])[:POTION_SLOTS]):
        if p.get("id") == POTION_SLOT_ID:
            continue
        base = 60 + k * POTION_ACTION_STRIDE
        if p.get("can_use"):
            if p.get("requires_target"):
                for i in targetable[:5]:
                    mask[base + 1 + i] = True
            else:
                mask[base] = True
        if p.get("can_discard"):
            mask[base + 6] = True
    ac = available_commands if available_commands is not None else (gs.get("available_commands") or [])
    if "end" in ac:
        mask[END_TURN_IDX] = True
    return mask


def idx_to_command(idx: int, gs: dict, select_choices: list[int] | None = None) -> str:
    """统一动作下标 → CommunicationMod 命令字符串。

    `select_choices`：选牌屏幕时，槽 k → `CHOOSE` 序号 的映射表（默认恒等）。
    """
    screen_type = gs.get("screen_type") or "NONE"
    if screen_type in ("GRID", "HAND_SELECT"):
        choice = select_choices[idx] if select_choices else idx
        return f"CHOOSE {choice}"
    if idx == END_TURN_IDX:
        return "END"
    if 60 <= idx < END_TURN_IDX:
        slot, pt = divmod(idx - 60, POTION_ACTION_STRIDE)
        if pt == 0:
            return f"POTION USE {slot}"
        if pt == 6:
            return f"POTION DISCARD {slot}"
        return f"POTION USE {slot} {pt - 1}"
    k, t = divmod(idx, HAND_ACTION_STRIDE)
    return f"PLAY {k + 1}" + (f" {t - 1}" if t >= 1 else "")


def action_desc(idx: int, gs: dict) -> str:
    """给 HUD 用的人话描述。"""
    screen_type = gs.get("screen_type") or "NONE"
    if screen_type in ("GRID", "HAND_SELECT"):
        scr = gs.get("screen_state") or {}
        cand = (scr.get("hand") or scr.get("cards") or []) if screen_type == "HAND_SELECT" \
            else (scr.get("cards") or [])
        if idx < len(cand):
            c = cand[idx]
            return f"选牌：{c.get('name', c.get('id'))}"
        return f"选牌槽 {idx}"
    if idx == END_TURN_IDX:
        return "结束回合"
    if 60 <= idx < END_TURN_IDX:
        slot, pt = divmod(idx - 60, POTION_ACTION_STRIDE)
        return f"药水槽 {slot} " + {0: "使用", 6: "丢弃"}.get(pt, f"打目标 {pt - 1}")
    k, t = divmod(idx, HAND_ACTION_STRIDE)
    hand = (gs.get("combat_state") or {}).get("hand") or []
    name = hand[k].get("name", "?") if k < len(hand) else "?"
    return f"打出「{name}」" + (f" → 目标 {t - 1}" if t >= 1 else "")


# ================================================================ CLI（自检/查表）

def _dump_tables():
    print(f"卡 {len(CARD_BY_STR)} 条 | 遗物 {len(RELIC_BY_STR)} | 药水 {len(POTION_BY_STR)} | "
          f"怪物 {len(MONSTER_BY_STR)} | 玩家状态 {len(PLAYER_STATUS_BY_STR)} | "
          f"怪物状态 {len(MONSTER_STATUS_BY_STR)} | 招式 {len(MOVE_ENUM)}")
    for name in ("Strike_R", "Defend_R", "Bash", "Headbutt"):
        print(f"  卡 {name!r} -> {CARD_BY_STR.get(name)}")
    for name in ("Burning Blood", "Pen Nib", "Happy Flower"):
        print(f"  遗物 {name!r} -> {RELIC_BY_STR.get(name)}")
    print(f"  药水 'Fire Potion' -> {POTION_BY_STR.get('Fire Potion')}")
    print(f"  怪物 'Cultist' -> {MONSTER_BY_STR.get('cultist')}")
    print(f"布局来源 = {LAYOUT_SOURCE}；交叉校验: {cross_check_layout() or '一致'}")


def _dump_moves():
    from_mon = {mid: [] for mid in MOVES_BY_MONSTER}
    names = {i: n for i, n in enumerate(MOVE_ENUM)}
    for mid, block in sorted(MOVES_BY_MONSTER.items()):
        for j, m in enumerate(block, 1):
            from_mon[mid].append(f"{j}:{names[m]}")
    for mid, items in sorted(from_mon.items()):
        print(f"[{mid}] " + " | ".join(items))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="真机映射层自检 / 查表")
    ap.add_argument("--tables", action="store_true", help="打印各 ID 空间规模与抽查")
    ap.add_argument("--moves", action="store_true", help="打印每只怪物的招式表（游戏 move_id 顺序）")
    args = ap.parse_args()
    if not (args.tables or args.moves):
        ap.print_help(sys.stderr)
        sys.exit(0)
    if args.tables:
        _dump_tables()
    if args.moves:
        _dump_moves()
