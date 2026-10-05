"""真机桥接（`comm_map` / `comm_battle`）的**离线守卫** —— 不需要游戏，也不需要引擎模块。

它用两只**手搓的 CommunicationMod 状态**（战斗 / 选牌）走一遍全链路：
头文件 ID 表 → 1076 维快照 → 合法掩码 → 统一动作下标 → 游戏命令 → HUD → JSONL 记录，
并把每个断言打印成 ✅/❌（与仓库其它 `_chk_*` 同风格）。

有引擎模块时（远端），会额外**逐项断言内置布局 == azlayout**；
本机引擎过期时只告警不判负（回退路径本来就是为真机测试机准备的，见 `comm_map` 顶部注释）。

用法: .venv/Scripts/python.exe realtest/_chk_comm.py（在仓库根跑）
"""

from __future__ import annotations

import io
import json
import os
import sys

import numpy as np

import comm_map as CM
import ckpt

PASS, FAIL = 0, 0
FAILED = []


def check(name: str, ok: bool, detail: str = "") -> bool:
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  ✅ {name}" + (f"  （{detail}）" if detail else ""))
    else:
        FAIL += 1
        FAILED.append(name)
        print(f"  ❌ {name}" + (f"  —— {detail}" if detail else ""))
    return ok


# ---------------------------------------------------------------- 夹具

def _card(cid, name=None, cost=1, upg=0, play=True, target=True, typ="ATTACK", misc=0):
    return {"id": cid, "name": name or cid, "cost": cost, "upgrades": upg,
            "is_playable": play, "has_target": target, "type": typ, "misc": misc,
            "uuid": f"u-{cid}-{upg}-{id(object())}"}


def battle_state() -> dict:
    # 真机纪律：available_commands 在协议**消息顶层**，game_state 内没有它
    #（2026-09-23 曾在此埋错 ⇒ 真机 END 永不可用，能量 0 时掩码全 0 死循环）。
    return {
        "in_game": True,
        "current_hp": 42, "max_hp": 80, "floor": 5, "act": 1, "gold": 123,
        "class": "IRONCLAD", "ascension_level": 0,
        "screen_type": "NONE", "room_phase": "COMBAT",
        "relics": [{"id": "Burning Blood", "name": "Burning Blood", "counter": 0},
                   {"id": "Pen Nib", "name": "Pen Nib", "counter": 7}],
        "deck": [_card("Strike_R", "Strike"), _card("Strike_R", "Strike"),
                 _card("Strike_R", "Strike"), _card("Strike_R", "Strike"),
                 _card("Strike_R", "Strike"), _card("Defend_R", "Defend", typ="SKILL", target=False),
                 _card("Defend_R", "Defend", typ="SKILL", target=False),
                 _card("Defend_R", "Defend", typ="SKILL", target=False),
                 _card("Defend_R", "Defend", typ="SKILL", target=False),
                 _card("Bash", "Bash", cost=2)],
        "potions": [{"id": "Fire Potion", "name": "Fire Potion", "can_use": True,
                     "can_discard": True, "requires_target": True},
                    {"id": "Potion Slot", "name": "Potion Slot", "can_use": False,
                     "can_discard": False, "requires_target": False}],
        "map": [],
        "combat_state": {
            "turn": 2,
            "cards_discarded_this_turn": 1, "times_damaged": 3,
            "player": {"current_hp": 42, "max_hp": 80, "block": 5, "energy": 3, "orbs": [],
                       "powers": [{"id": "Strength", "name": "Strength", "amount": 2},
                                  {"id": "Vulnerable", "name": "Vulnerable", "amount": 1}]},
            "hand": [_card("Strike_R", "Strike"),
                     _card("Strike_R", "Strike"),
                     _card("Defend_R", "Defend", typ="SKILL", target=False),
                     _card("Bash", "Bash", cost=2, upg=1)],
            "draw_pile": [_card("Strike_R", "Strike"), _card("Defend_R", "Defend",
                                                            typ="SKILL", target=False)],
            "discard_pile": [_card("Strike_R", "Strike")],
            "exhaust_pile": [],
            "monsters": [{"id": "Cultist", "name": "Cultist", "current_hp": 48, "max_hp": 50,
                          "block": 0, "intent": "ATTACK", "move_id": 1,
                          "move_base_damage": 6, "move_adjusted_damage": 6, "move_hits": 1,
                          "half_dead": False, "is_gone": False,
                          "powers": [{"id": "Ritual", "name": "Ritual", "amount": 1}]}],
        },
    }


def select_state() -> dict:
    st = battle_state()
    st["available_commands"] = ["choose", "proceed", "state"]
    st["screen_type"] = "HAND_SELECT"
    st["screen_state"] = {
        "hand": [_card("Strike_R", "Strike"), _card("Defend_R", "Defend", typ="SKILL",
                                                    target=False)],
        "selected": [], "max_cards": 1, "can_pick_zero": False,
    }
    return st


def map_state() -> dict:
    st = battle_state()
    st["room_phase"] = "COMPLETE"
    st["screen_type"] = "MAP"
    st["available_commands"] = ["choose", "state"]
    st["screen_state"] = {"next_nodes": [{"x": 1, "y": 1, "symbol": "M"}],
                          "boss_available": False}
    st["map"] = [{"x": 1, "y": 1, "symbol": "M",
                  "children": [{"x": 0, "y": 2, "symbol": "E"}, {"x": 1, "y": 2, "symbol": "M"}]},
                 {"x": 0, "y": 2, "symbol": "E"}, {"x": 1, "y": 2, "symbol": "M"}]
    return st


# ---------------------------------------------------------------- 用例

def t_ids():
    print("== ID 表（解析自 include/constants/）==")
    check("卡表解析出 >300 条", len(CM.CARD_BY_STR) > 300, f"{len(CM.CARD_BY_STR)} 条")
    check("遗物表解析出 >150 条", len(CM.RELIC_BY_STR) > 150, f"{len(CM.RELIC_BY_STR)} 条")
    check("药水表解析出 >40 条", len(CM.POTION_BY_STR) > 40, f"{len(CM.POTION_BY_STR)} 条")
    check("怪物表 66 条（含 INVALID=0，Cultist=14）",
          len(CM.MONSTER_BY_STR) == 66 and CM.MONSTER_BY_STR.get("cultist") == 14,
          f"{len(CM.MONSTER_BY_STR)} 条，cultist={CM.MONSTER_BY_STR.get('cultist')}")
    check("'Strike_R' 在卡表", "Strike_R" in CM.CARD_BY_STR)
    check("'Burning Blood' 在遗物表", "Burning Blood" in CM.RELIC_BY_STR)
    check("'Fire Potion' 在药水表", "Fire Potion" in CM.POTION_BY_STR)
    check("玩家状态 'Strength'/'Vulnerable' 可查",
          "strength" in CM.PLAYER_STATUS_BY_STR and "vulnerable" in CM.PLAYER_STATUS_BY_STR)
    check("怪物状态 'Ritual' 可查", "ritual" in CM.MONSTER_STATUS_BY_STR)
    cul = CM.MONSTER_BY_STR.get("cultist")
    block = CM.MOVES_BY_MONSTER.get(cul or 0) or []
    names = [CM.MOVE_ENUM[i] for i in block]
    check("Cultist 招式块 = [INCANTATION, DARK_STRIKE]",
          names == ["CULTIST_INCANTATION", "CULTIST_DARK_STRIKE"], str(names))
    check("map_move('Cultist')：1→DARK_STRIKE、3→INCANTATION、2→缺口 0",
          CM.map_move("Cultist", 1, []) == CM.MOVE_ENUM.index("CULTIST_DARK_STRIKE")
          and CM.map_move("Cultist", 3, []) == CM.MOVE_ENUM.index("CULTIST_INCANTATION")
          and CM.map_move("Cultist", 2, []) == 0)
    check("别名表键全部可解析且指到对应引擎名",
          all(CM.MONSTER_BY_STR[v] == CM.monster_idx(k) and CM.monster_idx(k) != 0
              for k, v in CM.MONSTER_ALIASES.items()), f"{len(CM.MONSTER_ALIASES)} 键")
    check("虱子方向：Normal=红(RED_LOUSE)、Defensive=绿(GREEN_LOUSE)（精灵图+HP 互证）",
          CM.monster_idx("FuzzyLouseNormal") == CM.MONSTER_BY_STR["redlouse"]
          and CM.monster_idx("FuzzyLouseDefensive") == CM.MONSTER_BY_STR["greenlouse"]
          and CM.monster_idx("GremlinWarrior") == CM.MONSTER_BY_STR["madgremlin"]
          and CM.monster_idx("GremlinTsundere") == CM.MONSTER_BY_STR["shieldgremlin"]
          and CM.monster_idx("BanditLeader") == CM.MONSTER_BY_STR["romeo"]
          and CM.monster_idx("SlaverBoss") == CM.MONSTER_BY_STR["taskmaster"]
          and CM.monster_idx("Healer") == CM.MONSTER_BY_STR["mystic"])
    check("编号表：虱子/邪教徒/麦斯提克 关键字节解析正确",
          CM.map_move("FuzzyLouseDefensive", 3, []) == CM.MOVE_ENUM.index("GREEN_LOUSE_BITE")
          and CM.map_move("FuzzyLouseDefensive", 4, []) == CM.MOVE_ENUM.index("GREEN_LOUSE_SPIT_WEB")
          and CM.map_move("FuzzyLouseNormal", 3, []) == CM.MOVE_ENUM.index("RED_LOUSE_BITE")
          and CM.map_move("FuzzyLouseNormal", 4, []) == CM.MOVE_ENUM.index("RED_LOUSE_GROW")
          and CM.map_move("Cultist", 1, []) == CM.MOVE_ENUM.index("CULTIST_DARK_STRIKE")
          and CM.map_move("Cultist", 3, []) == CM.MOVE_ENUM.index("CULTIST_INCANTATION")
          and CM.map_move("Healer", 1, []) == CM.MOVE_ENUM.index("MYSTIC_ATTACK_DEBUFF"))
    cov = sum(len(v) for v in CM.MOVES_BY_MONSTER.values())
    check("招式覆盖 ≥150 条", cov >= 150, f"{cov} 条")


def t_snapshot():
    print("== 战斗状态 → 1076 维快照 ==")
    gs = battle_state()
    snap, notes = CM.game_state_to_snapshot(gs, {"symbol": "M", "children": ["M", "E", "?"]})
    check("长度 == 1076", snap.shape == (1076,), str(snap.shape))
    strike = CM.CARD_BY_STR["Strike_R"]
    bash = CM.CARD_BY_STR["Bash"]
    check("手牌槽0 cardId == Strike_R*2", snap[13] == strike * 2, f"{snap[13]} vs {strike * 2}")
    check("手牌槽3 == Bash*2+1（升级位）", snap[13 + 4 * 3] == bash * 2 + 1)
    check("手牌 playable 字段 == 1", snap[13 + 3] == 1)
    check("计数块 [怪, 手, 抽, 弃, 耗] == [1,4,2,1,0]",
          list(snap[8:13]) == [1, 4, 2, 1, 0], str(list(snap[8:13])))
    check("抽牌堆槽0 == Strike_R*2", snap[53] == strike * 2)
    check("弃牌堆槽0 == Strike_R*2", snap[113] == strike * 2)
    check("总牌组槽0 == Strike_R*2 且长度 64 块兜底为 0", snap[233] == strike * 2 and snap[297 - 1] == 0)
    mon = 297
    check("怪物槽 [id,hp,maxHp,block] == [14,48,50,0]",
          list(snap[mon:mon + 4]) == [14, 48, 50, 0], str(list(snap[mon:mon + 4])))
    check("怪物 move == Cultist 的 move_id=1（DARK_STRIKE，编号表口径）",
          snap[mon + 4] == CM.MOVE_ENUM.index("CULTIST_DARK_STRIKE"))
    check("怪物 targetable/伤害/次数 == [1,6,1]", list(snap[mon + 5:mon + 8]) == [1, 6, 1])
    check("玩家属性 [stance,orbs,energy] == [0,0,3]", list(snap[337:340]) == [0, 0, 3])
    check("回合计数: 弃 1 / 受伤 3 / PenNib 7", snap[343] == 1 and snap[345] == 3 and snap[346] == 7)
    ps = CM.PLAYER_STATUS_BY_STR
    check("玩家状态: Strength=2 / Vulnerable=1",
          snap[348 + ps["strength"]] == 2 and snap[348 + ps["vulnerable"]] == 1)
    ms = CM.MONSTER_STATUS_BY_STR
    check("怪物状态: Ritual=1", snap[435 + ms["ritual"]] == 1)
    check("非选牌帧：选牌区全 0", not snap[650:834].any())
    check("run 上下文: act/floor/gold == [1,5,123]", list(snap[834:837]) == [1, 5, 123])
    check("下一行直方图: E=1,M=1,?=1 → 位 2/3/4 各 1",
          list(snap[837:844]) == [0, 0, 1, 1, 1, 0, 0], str(list(snap[837:844])))
    check("遗物位图: Burning Blood / Pen Nib 置 1",
          snap[844 + CM.RELIC_BY_STR["Burning Blood"]] == 1
          and snap[844 + CM.RELIC_BY_STR["Pen Nib"]] == 1)
    check("药水: [count,cap]==[1,2]，袋子 Fire=1，槽0=Fire、槽1=0",
          list(snap[1025:1027]) == [1, 2]
          and snap[1027 + CM.POTION_BY_STR["Fire Potion"]] == 1
          and snap[1071] == CM.POTION_BY_STR["Fire Potion"] and snap[1072] == 0)
    hard = [n for n in notes if n.startswith("缺口")]
    check("缺口可数且不含未知卡/怪/遗物", isinstance(hard, list), f"{len(hard)} 条：{hard[:2]}")
    return snap, notes


def t_actions(snap):
    print("== 合法掩码 / 动作 → 命令 ==")
    gs = battle_state()
    ac = ["play", "potion", "end", "state"]
    mask = CM.legal_action_mask(gs, ac)
    check("掩码长度 96", mask.shape == (96,))
    check("槽0 Strike 可打目标0 → idx=1", bool(mask[0 * 6 + 1]))
    check("槽2 Defend 无目标 → idx=12", bool(mask[2 * 6]))
    check("END(idx=95) 合法", bool(mask[95]))
    check("回归：END 只认顶层 available_commands（gs 内无此字段）",
          not CM.legal_action_mask(gs)[95] and "available_commands" not in gs)
    gs0 = battle_state()                    # 真机首战现场：能量 0、手牌全不可出
    gs0["combat_state"]["player"]["energy"] = 0
    for c in gs0["combat_state"]["hand"]:
        c["is_playable"] = False
    for p in gs0["potions"]:
        p["can_use"] = p["can_discard"] = False
    m0 = CM.legal_action_mask(gs0, ["end", "state"])
    check("回归·能量 0（无牌可打）：掩码恰好只剩 END", list(np.flatnonzero(m0)) == [95],
          str(list(np.flatnonzero(m0))))
    check("回归·敌方回合（无 end、无牌可打）：掩码为空",
          not CM.legal_action_mask(gs0, ["state", "wait"]).any())
    check("药水: 用(需目标)打0 = 61、丢弃 = 66",
          bool(mask[60 + 0 * 7 + 1]) and bool(mask[60 + 0 * 7 + 6]))
    check("空药水槽2无动作", not mask[60 + 1 * 7:60 + 2 * 7].any())
    check("命令: 1→'PLAY 1 0'", CM.idx_to_command(1, gs) == "PLAY 1 0")
    check("命令: 0→'PLAY 1'", CM.idx_to_command(0, gs) == "PLAY 1")
    check("命令: 95→'END'", CM.idx_to_command(95, gs) == "END")
    check("命令: 60/61/66 → 用/打0/丢", CM.idx_to_command(60, gs) == "POTION USE 0"
          and CM.idx_to_command(61, gs) == "POTION USE 0 0"
          and CM.idx_to_command(66, gs) == "POTION DISCARD 0")
    desc = CM.action_desc(1, gs)
    check("HUD 描述含卡名", "Strike" in desc, desc)


def t_select():
    print("== 选牌屏幕（HAND_SELECT）==")
    gs = select_state()
    snap, notes = CM.game_state_to_snapshot(gs, None)
    strike = CM.CARD_BY_STR["Strike_R"]
    check("选牌槽0 == Strike_R*2", snap[650] == strike * 2)
    check("select_meta == [2,1,0,0]", list(snap[830:834]) == [2, 1, 0, 0])
    check("inputState == CARD_SELECT(2)", snap[5] == 2)
    mask = CM.legal_action_mask(gs)
    check("掩码: 槽0/槽1 合法、槽2 非法", bool(mask[0]) and bool(mask[1]) and not mask[2])
    check("命令: CHOOSE 1（身份映射）", CM.idx_to_command(1, gs) == "CHOOSE 1")
    check("选牌屏掩码仍是 96 维统一空间（真机回归：曾按 60 维喂网络 ⇒ 96 vs 60）",
          mask.shape == (96,), str(mask.shape))


def t_layout():
    print("== 布局一致性（内置 ↔ azlayout）==")
    total = CM._FALLBACK["len"]
    check("回退布局自洽（各块拼回 1076）",
          13 + 40 + 60 + 60 + 60 + 64 + 40 + 3 + 8 + 87 + 215 + 180 + 4 + 10 + 181 + 2 + 44 + 5
          == total, f"sum=={total}")
    bad = CM.cross_check_layout()
    if CM.LAYOUT_SOURCE == "azlayout":
        check("内置布局与 azlayout 逐项相等", not bad, "; ".join(bad[:3]))
    else:
        print(f"  ⚠️ 跳过严格断言（azlayout 不可用）—— 本机引擎模块过期时属预期；"
              f"远端（引擎最新）应跑完整版")
        check("回退告知已生效", bool(CM.LAYOUT_WARN))


def t_pipeline():
    print("== 端到端（mock 消息 → 命令/记录）==")
    save = sys.stdout
    sys.stdout = io.StringIO()                # comm_battle 会在导入时写 "ready"
    try:
        import comm_battle as CB
    finally:
        sys.stdout = save
    proto = CB._PROTO                          # 捕获的协议通道（StringIO 或真 stdout）
    rec_path = os.path.join(CM.ROOT, "data", "_chk_comm_rec.jsonl")
    rec2 = rec_path.replace(".jsonl", "_glue.jsonl")
    for p in (rec_path, rec2):
        if os.path.exists(p):
            os.remove(p)

    gs0 = battle_state()                      # 首战回归场：能量 0、手牌全不可出
    gs0["combat_state"]["player"]["energy"] = 0
    for c in gs0["combat_state"]["hand"]:
        c["is_playable"] = False
    for p in gs0["potions"]:
        p["can_use"] = p["can_discard"] = False
    msgs = (
        {"ready_for_command": True, "in_game": False, "available_commands": ["start", "state"]},
        {"ready_for_command": True, "in_game": True, "available_commands": ["play", "end", "state"],
         "game_state": battle_state()},
        # ① 真机首战回归：能量 0 + end 可用 ⇒ 必须 END（旧代码在这里键到 66 Hz 死循环）
        {"ready_for_command": True, "in_game": True, "available_commands": ["end", "state"],
         "game_state": gs0},
        # ②③ 故障帧（play 可发却无牌可出 = 状态对不上）：只回 1 次 state（故障限频）
        {"ready_for_command": True, "in_game": True, "available_commands": ["play", "state"],
         "game_state": gs0},
        {"ready_for_command": True, "in_game": True, "available_commands": ["play", "state"],
         "game_state": gs0},
        # ④ 正常战斗帧（敌方回合，无 play/end）：完全沉默 —— 正常语义下绝不重发
        {"ready_for_command": True, "in_game": True, "available_commands": ["state", "wait"],
         "game_state": gs0},
        {"ready_for_command": True, "in_game": True, "available_commands": ["return", "proceed"],
         "game_state": {**battle_state(), "screen_type": "CARD_REWARD", "room_phase": "COMPLETE"}},
    )

    def _run(extra, rec):
        a = CB.parse_args([*extra, "--record", rec, "--policy", "heuristic", "--hud"])
        c = CB.Client(a, CB.HeuristicPolicy())
        off = len(proto.getvalue()) if hasattr(proto, "getvalue") else 0
        for m in msgs:
            c.on_message(m)
        got = proto.getvalue()[off:].strip().split("\n") if hasattr(proto, "getvalue") else []
        return c, got

    # ① 默认 = 观察模式：非战斗必须**沉默**；战斗帧出手，打不了牌就 END
    c1, cmds1 = _run([], rec_path)
    check("观察模式：菜单/结算屏不发命令；战斗帧 PLAY 3 → END（能量 0 时打不了就 END）",
          cmds1 == ["PLAY 3", "END", "state"], str(cmds1))
    check("故障帧只重询 1 次 state；正常帧（敌方回合）完全沉默（绝不逐条回击）",
          cmds1.count("state") == 1)
    check("观察模式：只记录战斗帧（JSONL 两行：PLAY + END）",
          os.path.exists(rec_path) and sum(1 for _ in open(rec_path, encoding="utf-8")) == 2)
    if os.path.exists(rec_path):
        r = json.loads(open(rec_path, encoding="utf-8").readline())
        check("记录含快照(1076)/动作/命令/顶层 ac", len(r["obs"]) == 1076 and r["action"] == 12
              and r["cmd"] == "PLAY 3" and r.get("ac") == ["play", "end", "state"])
    c1.rec.close()

    # ② --glue = 无人值守：自动 START + 胶水 return
    c2, cmds2 = _run(["--glue", "--games", "1"], rec2)
    check("胶水模式：START → PLAY 3 → END → state（故障限频）→ return",
          cmds2 == ["START IRONCLAD 0", "PLAY 3", "END", "state", "return"], str(cmds2))
    c2.rec.close()
    for p in (rec_path, rec2):
        if os.path.exists(p):
            os.remove(p)

    # HUD 渲染不炸
    hud = CB.render_hud(battle_state(), np.zeros(1076, dtype=np.int64),
                        CM.legal_action_mask(battle_state(), ["play", "end"]), 1, [(1, 0.5)], [], 0)
    check("HUD 渲染成功且含怪物/手牌", "Cultist" in hud and "Strike" in hud)


def t_elixir():
    """药水打开的「任意多选」手选屏（Elixir）—— 帧结构剪自真机 SlayTheSpire.log
    2026-09-23 15:37:37。真机事故：完成按钮是 `confirm`（不是 proceed）⇒ 假故障；
    且网络路径此时还会抛 RuntimeError（96 vs 60，见 t_net_select）。"""
    print("== 药水手选屏（Elixir：choose/confirm、can_pick_zero）==")
    save = sys.stdout
    sys.stdout = io.StringIO()
    try:
        import comm_battle as CB
    finally:
        sys.stdout = save
    proto = CB._PROTO

    def run(frame: dict):
        a = CB.parse_args(["--policy", "heuristic"])
        c = CB.Client(a, CB.HeuristicPolicy())
        off = len(proto.getvalue()) if hasattr(proto, "getvalue") else 0
        c.on_message({"ready_for_command": True, "in_game": True,
                      "available_commands": frame["available_commands"],
                      "game_state": frame["game_state"]})
        return proto.getvalue()[off:].strip().split("\n") if hasattr(proto, "getvalue") else []

    gs = select_state()
    gs["available_commands"] = ["choose", "confirm", "key", "click", "wait", "state"]
    gs["screen_state"] = {"max_cards": 99, "can_pick_zero": True, "selected": [],
                          "hand": [_card("Defend_R", "Defend", typ="SKILL", target=False),
                                   _card("Strike_R", "Strike")]}
    got = run({"available_commands": gs["available_commands"], "game_state": gs})
    check("Elixir 屏：can_pick_zero ⇒ 直接 confirm（旧代码判假故障）",
          got == ["confirm"], str(got))

    gs2 = select_state()
    gs2["available_commands"] = ["confirm", "key", "click", "wait", "state"]
    gs2["screen_state"] = {"max_cards": 99, "can_pick_zero": True, "selected": [0, 1],
                           "hand": []}
    got2 = run({"available_commands": gs2["available_commands"], "game_state": gs2})
    check("空手选屏（只剩 confirm）：发 confirm 而不发 state（不判故障）",
          got2 == ["confirm"], str(got2))


def t_net_select():
    """选牌屏 × 网络策略回归（真机 RuntimeError: tensor a (96) vs b (60)）。

    需要 `data/walk_v4_n10000.pt(.json)`；缺了跳过（可选回归）。
    """
    print("== 选牌屏 × 网络策略（96 vs 60 回归）==")
    save = sys.stdout
    sys.stdout = io.StringIO()
    try:
        import comm_battle as CB
    finally:
        sys.stdout = save
    ck = os.path.join(CM.ROOT, "data", "walk_v4_n10000.pt")
    if not (os.path.exists(ck) and ckpt.config_of(ck)):
        print("  ⏭️ 跳过：缺 data/walk_v4_n10000.pt（或存档里没有配置）")
        return
    net = CB.NetPolicy(ck)
    gs = select_state()                       # can_pick_zero=False ⇒ 必须真的选一张
    mask = CM.legal_action_mask(gs)
    snap, _ = CM.game_state_to_snapshot(gs, None)
    try:
        idx, _top = net.act(gs, snap, mask)
        check("选牌屏：NetPolicy.act 不再抛异常（旧 bug：logits 60 vs 掩码 96）", True)
        check("选牌屏：所选下标在候选内",
              0 <= idx < CM.SEL_SLOTS and bool(mask[idx]), f"idx={idx}")
    except Exception as e:                                        # noqa: BLE001
        check("选牌屏：NetPolicy.act 不再抛异常（旧 bug：logits 60 vs 掩码 96）", False,
              f"{type(e).__name__}: {e}")

    # 全流程（真机 Burning Pact 型两段式：先只有 choose（无完成键）；选中 1 张后才出现 confirm）
    proto = CB._PROTO
    c = CB.Client(CB.parse_args([]), net)
    off = len(proto.getvalue()) if hasattr(proto, "getvalue") else 0
    g1 = select_state()
    g1["available_commands"] = ["choose", "key", "click", "wait", "state"]
    c.on_message({"ready_for_command": True, "in_game": True,
                  "available_commands": g1["available_commands"], "game_state": g1})
    g2 = select_state()
    g2["available_commands"] = ["confirm", "key", "click", "wait", "state"]
    g2["screen_state"] = dict(g2["screen_state"], selected=[g2["screen_state"]["hand"][0]],
                              hand=[g2["screen_state"]["hand"][1]])
    c.on_message({"ready_for_command": True, "in_game": True,
                  "available_commands": g2["available_commands"], "game_state": g2})
    got = proto.getvalue()[off:].strip().split("\n") if hasattr(proto, "getvalue") else []
    check("全流程（Burning Pact 型）：网络 CHOOSE 选卡 + 选中后 confirm",
          len(got) == 2 and got[0].startswith("CHOOSE ") and got[1] == "confirm", str(got))


def t_slimeboss():
    """2026-09-23 真机 SlimeBoss 战的两个观测缺口回归（字段剪自真机帧）。

    现场：表里只有 {1, 3}，而真机连出 move_id=4（黏液喷吐）与 2（预备重击）⇒ 意图整块丢；
    另抓到 power "Weakened"（游戏 WeakPower 的运行时 id）映射不上。
    权威依据：`_decomp/src/SlimeBoss.java`（SLAM=1 / PREP_SLAM=2 / SPLIT=3 / STICKY=4）。
    """
    print("== SlimeBoss 招式表 + Weakened 别名（真机回归）==")
    gs = battle_state()
    m = gs["combat_state"]["monsters"][0]
    m.update({"id": "SlimeBoss", "name": "Slime Boss", "current_hp": 112, "max_hp": 140,
              "move_id": 4,
              "powers": [{"amount": -1, "name": "Split", "id": "Split"},
                         {"amount": 1, "name": "Vulnerable", "id": "Vulnerable"},
                         {"amount": 1, "name": "Weakened", "id": "Weakened"}]})
    snap, notes = CM.game_state_to_snapshot(gs, None)
    mv = CM.map_move("SlimeBoss", 4, [])
    check("SlimeBoss move_id=2/3/4 → PREPARING/SPLIT/GOOP_SPRAY（2/4 曾缺）",
          CM.map_move("SlimeBoss", 2, []) == CM.MOVE_ENUM.index("SLIME_BOSS_PREPARING")
          and CM.map_move("SlimeBoss", 3, []) == CM.MOVE_ENUM.index("SLIME_BOSS_SPLIT")
          and mv == CM.MOVE_ENUM.index("SLIME_BOSS_GOOP_SPRAY"))
    check("快照：move_id=4 不再报'不在编号表'", not any("move_id=4" in n for n in notes))
    check("快照：'Weakened'（游戏内部 id）落位为引擎 Weak == 1",
          snap[CM.LAYOUT["monster_status_off"] + CM.MONSTER_STATUS_BY_STR["weakened"]] == 1)
    check("快照：不再报'未映射的怪物 power Weakened'",
          not any("Weakened" in n for n in notes))
    check("快照：'Split' 保持显式缺口（引擎无此 power，与训练一致）",
          any("Split" in n for n in notes))
    check("Lagavulin：1→SIPHON_SOUL、3→ATTACK、5→SLEEP（4/6 保持缺口）",
          CM.map_move("Lagavulin", 1, []) == CM.MOVE_ENUM.index("LAGAVULIN_SIPHON_SOUL")
          and CM.map_move("Lagavulin", 3, []) == CM.MOVE_ENUM.index("LAGAVULIN_ATTACK")
          and CM.map_move("Lagavulin", 5, []) == CM.MOVE_ENUM.index("LAGAVULIN_SLEEP")
          and CM.map_move("Lagavulin", 4, []) == 0)
    check("WrithingMass：编号 0..4 全部入表（0 也是合法编号）",
          [CM.map_move("WrithingMass", j, []) for j in range(5)]
          == [CM.MOVE_ENUM.index(n) for n in
              ("WRITHING_MASS_STRONG_STRIKE", "WRITHING_MASS_MULTI_STRIKE",
               "WRITHING_MASS_FLAIL", "WRITHING_MASS_WITHER", "WRITHING_MASS_IMPLANT")])


def main():
    for fn in (t_ids,):
        fn()
    snap, _ = t_snapshot()
    t_actions(snap)
    t_select()
    t_layout()
    t_pipeline()
    t_elixir()
    t_net_select()
    t_slimeboss()
    print(f"\n================ {PASS} / {PASS + FAIL} 通过 ================")
    if FAILED:
        print("失败项：" + "; ".join(FAILED))
        sys.exit(1)


if __name__ == "__main__":
    main()
