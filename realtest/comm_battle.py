r"""在**真实游戏**里跑「仅战斗」的可视化测试 —— CommunicationMod 客户端。

参照：
  · ForgottenArbiter/CommunicationMod —— stdin/stdout JSON 协议（协议本体，见 README）；
  · JustinoChan/AscensionAI —— 工程经验（先 `ready` 再重导入、stdout 只走协议、
    屏幕钻取的兜底与错误恢复）。

它会做的事（两种模式）：
  1. **默认 = 观察模式**：非战斗屏幕**完全不操作** —— 地图/奖励/商店由人工驾驶
     （CommunicationMod 的语义：不回命令 = 游戏保持现状、人类照常操作），
     进入战斗（含战斗内选牌）才自动接管；
  2. **`--glue` = 无人值守胶水模式**：自动 `START`（主菜单 ⇒ `<职业> <飞升> [种子]`）、
     走第一条地图路线、跳卡牌奖励…；
  3. **战斗**：真机状态 → `comm_map.game_state_to_snapshot`（1076 维，与训练同构）→
     策略（`--ckpt` 的 AZNet 贪心 / 内置启发式 / 随机）→ `PLAY/POTION/END`；
     战斗内的选牌屏幕（GRID/HAND_SELECT）走 `select_head`；
  4. 每一步战斗都写：终端 HUD（stderr + 日志文件）+ `--record` 的 JSONL（含快照与决策）。

用法（由 CommunicationMod 按 `config.properties` 的 `command=` 拉起，**不要手动跑**）：

    %LOCALAPPDATA%\ModTheSpire\CommunicationMod\config.properties:
        command=D\:/tmp/sts_lightspeed/.venv/Scripts/python.exe D\:/tmp/sts_lightspeed/realtest/comm_battle.py --ckpt D\:/tmp/sts_lightspeed/data/n20k_obs.pt --games 1 --fights 3 --record D\:/tmp/sts_lightspeed/data/comm_run.jsonl --hud

离线自检（**不需要游戏**，在仓库根跑）：`python realtest/_chk_comm.py`；回放已录制的决策：

    python realtest/comm_battle.py --replay data/comm_<ts>.jsonl --ckpt data/n20k_obs.pt

⚠️ 本文件的纪律：`sys.stdout` 是协议通道（只许写命令 + `ready`），所有人类可读输出
   走 stderr / 日志文件。安装与缺口解读见 `realtest/README.md`。
"""

from __future__ import annotations

import sys

# ── stdout 纪律：必须在任何重导入之前建立。CommunicationMod 以 10s 为限等 "ready"。
_PROTO = sys.stdout
_PROTO.write("ready\n")
_PROTO.flush()
sys.stdout = sys.stderr                      # 之后 print() 全部落到 stderr（游戏日志可见）

import argparse                              # noqa: E402
import json                                  # noqa: E402
import os                                    # noqa: E402
import random                                # noqa: E402
import time                                  # noqa: E402

import numpy as np                           # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                 # realtest/ 上一级 = 仓库根（`aznet`/`azlayout` 在那里）
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import ckpt                                  # noqa: E402
import comm_map as CM                        # noqa: E402

_START_TS = time.strftime("%Y%m%d_%H%M%S")
_LOG_F = None


def log(msg: str) -> None:
    line = f"[comm {time.strftime('%H:%M:%S')}] {msg}"
    print(line, file=sys.stderr, flush=True)
    if _LOG_F:
        _LOG_F.write(line + "\n")
        _LOG_F.flush()


def send(cmd: str) -> None:
    _PROTO.write(cmd + "\n")
    _PROTO.flush()
    log(f"→ {cmd}")


# ================================================================ 策略

class RandomPolicy:
    name = "random"

    def act(self, gs, snap, mask):
        idx = int(np.random.choice(np.flatnonzero(mask)))
        return idx, [(idx, 1.0)]


class HeuristicPolicy:
    """极简启发式（只做**管线冒烟**用途，不是棋力基线）。

    规则：有来袭伤害且手里有防御 ⇒ 打防御；否则打费用最低的攻击牌打最低血量目标；
    另外若「痛击」可打且场上没易伤，优先痛击。
    """
    name = "heuristic"

    def act(self, gs, snap, mask):
        combat = gs.get("combat_state") or {}
        hand = list(combat.get("hand") or [])
        monsters = list(combat.get("monsters") or [])
        player = combat.get("player") or {}
        incoming = sum(int(m.get("move_adjusted_damage") or 0) * int(m.get("move_hits") or 1)
                       for m in monsters if not m.get("is_gone"))
        block = int(player.get("block") or 0)
        targetable = [i for i, m in enumerate(monsters)
                      if not m.get("is_gone") and not m.get("half_dead")]
        low_hp = min(targetable, key=lambda i: int(monsters[i].get("current_hp") or 0)) \
            if targetable else 0

        def playable(k, t=None):
            i = k * 6 + (0 if t is None else 1 + t)
            return i if mask[i] else None

        vuln_up = any(CM._norm(p.get("id", "")) == "vulnerable"
                      for m in monsters for p in (m.get("powers") or []))
        if incoming > block:
            for k, c in enumerate(hand):
                if (c.get("name") or "").startswith("Defend"):
                    i = playable(k)
                    if i is not None:
                        return i, [(i, 1.0)]
        for k, c in enumerate(hand):
            if c.get("type") != "ATTACK" or not c.get("is_playable"):
                continue
            i = playable(k) if not c.get("has_target") else playable(k, low_hp)
            if i is not None:
                return i, [(i, 1.0)]
        if CM.END_TURN_IDX in np.flatnonzero(mask):
            return CM.END_TURN_IDX, [(CM.END_TURN_IDX, 1.0)]
        return int(np.flatnonzero(mask)[0]), [(int(np.flatnonzero(mask)[0]), 1.0)]


class NetPolicy:
    """加载 `data/*.pt`（AZNet）。**建网配置在同文件里**（单文件存档，`ckpt`）。"""

    def __init__(self, ckpt: str):
        import torch
        from aznet import AZNet, net_kwargs
        ck = ckpt.read(ckpt)
        if not ck["config"]:
            raise RuntimeError(f"{ckpt} 里没有建网配置（裸 state_dict？）—— 不知道架构，拒绝猜；"
                               "用 `train.save_ckpt` / `ckpt.migrate` 重存一份")
        self.space = int(ck.get("space") or 0)
        if self.space and self.space != CM.END_TURN_IDX + 1:
            raise RuntimeError(f"动作空间不符：存档 space={self.space}，本仓 = {CM.END_TURN_IDX + 1}")
        self.torch = torch
        self.net = AZNet(**net_kwargs(ck["config"]))
        self.net.load_state_dict(ck["weights"], strict=True)
        self.net.eval()
        self.name = os.path.basename(ckpt)
        self.metrics = {k: float(v) for k, v in (ck.get("metrics") or {}).items()}

    def act(self, gs, snap, mask):
        torch = self.torch
        select = (gs.get("screen_type") in ("GRID", "HAND_SELECT"))
        with torch.no_grad():
            p_act, p_sel, _ = self.net(torch.as_tensor(snap, dtype=torch.float32).unsqueeze(0))
        logits = (p_sel if select else p_act).squeeze(0)
        # ⚠️ 2026-09-23 真机回归修的 bug：`legal_action_mask` 对选牌屏也返回**96 维**统一
        #    掩码（只把前 SEL_SLOTS 槽置位），而选牌屏 logits 来自 `select_head`（60 维）
        #    ⇒ 直接 masked_fill 会 RuntimeError（a 96 vs b 60）。修法：掩码截到与 logits 同长。
        m = torch.as_tensor(mask[: logits.shape[0]], dtype=torch.bool)
        k = int(torch.argmax(logits.masked_fill(~m, -1e9)))
        probs = torch.softmax(logits.masked_fill(~m, -1e9), dim=0)
        top = sorted(((int(i), float(probs[i])) for i in torch.nonzero(m).flatten()),
                     key=lambda x: -x[1])[:5]
        return k, top


def build_policy(args):
    want = args.policy
    if args.ckpt and want in ("auto", "net"):
        try:
            p = NetPolicy(args.ckpt)
            log(f"策略 = 网络 {p.name}（metrics={p.metrics or '—'}）")
            return p
        except Exception as e:                                              # noqa: BLE001
            log(f"⚠️ 网络加载失败（{type(e).__name__}: {e}）⇒ 退回启发式")
            return HeuristicPolicy()
    if want == "net":
        log("⚠️ --policy net 但没给 --ckpt ⇒ 退回启发式")
    return {"random": RandomPolicy, "heuristic": HeuristicPolicy}.get(want, HeuristicPolicy)()


# ================================================================ HUD

def render_hud(gs, snap, mask, idx, top, notes, fights_done) -> str:
    combat = gs.get("combat_state") or {}
    player = combat.get("player") or {}
    hand = list(combat.get("hand") or [])
    monsters = list(combat.get("monsters") or [])
    hp, mhp = gs.get("current_hp"), gs.get("max_hp")
    lines = [
        f"┌─ 第 {fights_done + 1} 场 · 回合 {combat.get('turn', '?')} · "
        f"{gs.get('screen_type')} ─ 英雄 {hp}/{mhp} ⚡{player.get('energy', '?')} "
        f"🛡{player.get('block', 0)} 💰{gs.get('gold', 0)} 血 {gs.get('floor', '?')}层",
    ]
    for i, m in enumerate(monsters):
        intent = m.get("intent", "?")
        dmg = m.get("move_adjusted_damage") or 0
        hits = m.get("move_hits") or 0
        flag = "☠" if m.get("is_gone") else ("½" if m.get("half_dead") else " ")
        lines.append(f"│ [{i}]{flag} {m.get('name', m.get('id')):<18} "
                     f"{m.get('current_hp')}/{m.get('max_hp')} 🛡{m.get('block', 0)} "
                     f"意图 {intent}{f' {dmg}×{hits}' if dmg else ''}")
    lines.append("│ 手牌 " + " ".join(
        f"{k + 1}:{c.get('name')}{'*' if int(c.get('upgrades') or 0) else ''}"
        f"({c.get('cost')})" for k, c in enumerate(hand)))
    lines.append("│ 决策 " + CM.action_desc(idx, gs) + " | top: " + " ".join(
        f"#{i}:{p:.2f}" for i, p in top))
    gaps = list(dict.fromkeys(notes))
    lines.append(f"└ 缺口 {len(gaps)} 条" + (f"（{gaps[0][:60]}…）" if gaps else "（无）"))
    return "\n".join(lines)


# ================================================================ 客户端

_FAULT_RESEND_S = 30.0        # 故障态 state 重询的最小间隔（秒）；正常流程不重发

class Client:
    def __init__(self, args, policy):
        self.args = args
        self.policy = policy
        self.games_left = args.games
        self.fights_done = 0
        self.in_combat = False
        self.errors = 0
        self.map_ctx = None                     # {"symbol":.., "children":[..]}（选完地图节点后记录）
        self._last_idle = None                  # 观察模式的日志去重
        self._fault_tag = None                  # 故障日志去重（正常流程不重发，见 fault/retry_state）
        self._fault_t = 0.0                     # 上次故障重询 state 的时刻（≤ 1 次/_FAULT_RESEND_S）
        self.map_nodes = {}                     # (x,y) -> node（来自 MAP 屏幕的 map 列表）
        self.last_combat_key = None
        self.rec = open(args.record, "a", encoding="utf-8") if args.record else None

    # ---- 输出 --------------------------------------------------------------
    def hud(self, gs, snap, mask, idx, top, notes) -> None:
        if self.args.hud:
            log(render_hud(gs, snap, mask, idx, top, notes, self.fights_done))
        if notes:
            for n in dict.fromkeys(notes):
                log("  " + n)

    def record(self, gs, snap, mask, idx, cmd, top, notes, ac) -> None:
        if not self.rec:
            return
        self.rec.write(json.dumps({
            "ts": time.time(), "gs": gs, "obs": [int(x) for x in snap],
            "legal": [int(i) for i in np.flatnonzero(mask)], "action": int(idx),
            "cmd": cmd, "ac": list(ac), "top": [[int(i), round(p, 4)] for i, p in top],
            "notes": list(dict.fromkeys(notes)),
        }, ensure_ascii=False) + "\n")
        self.rec.flush()

    # ---- 单步 --------------------------------------------------------------
    def on_message(self, msg: dict) -> None:
        if msg.get("error"):
            self.errors += 1
            log(f"⚠️ 游戏报错：{msg['error']}（第 {self.errors} 次）")
            if self.errors > 30:
                log("错误过多 ⇒ 退出")
                self.finish(1)
            self.retry_state()                  # 故障态重询（限频）；不做逐条回击
            return
        if not msg.get("ready_for_command"):
            return
        if not msg.get("in_game"):
            if self.args.glue and self.games_left > 0 and "start" in (msg.get("available_commands") or []):
                seed = f" {self.args.seed}" if self.args.seed else ""
                send(f"START {self.args.character} {self.args.ascension}{seed}")
            else:
                self.idle("局外 / 主菜单")
            return

        gs = msg["game_state"]
        ac = msg.get("available_commands") or []
        screen = gs.get("screen_type") or "NONE"
        combat = gs.get("room_phase") == "COMBAT"

        if self.in_combat and not combat:
            self.fights_done += 1
            log(f"🏁 第 {self.fights_done} 场战斗结束（hp {gs.get('current_hp')}）")
            if self.args.fights and self.fights_done >= self.args.fights:
                self.finish(0)
        self.in_combat = combat

        if screen == "GAME_OVER":
            if not self.args.glue:
                self.idle("游戏结束画面（手动重开）")
                return
            log(f"💀 游戏结束（第 {self.games_left} 局）")
            self.games_left -= 1
            if self.games_left <= 0:
                self.finish(0)
            send("proceed")
            return

        if combat and screen in ("GRID", "HAND_SELECT"):
            self.step_select(gs, ac)
        elif combat and ("play" in ac or "end" in ac):
            self.step_combat(gs, ac)
        elif combat:
            # 敌方回合 / 过渡：正常语义 = 完全沉默，等 mod 的下一条自动状态
            self.idle("战斗帧无 play/end（敌方回合 / 过渡）")
        elif self.args.glue:
            self.glue(gs, ac)
        else:
            self.idle(f"{screen} / room_phase={gs.get('room_phase')}")

    # ---- 战斗 --------------------------------------------------------------
    def step_combat(self, gs, ac) -> None:
        snap, notes = CM.game_state_to_snapshot(gs, self.map_ctx)
        mask = CM.legal_action_mask(gs, ac)
        if not mask.any():
            self.fault("combat", "输入相位却无合法动作（状态与动作空间对不上？）")
            return
        idx, top = self.policy.act(gs, snap, mask)
        if not mask[idx]:
            log(f"⚠️ 策略给了非法下标 {idx} ⇒ 改打第一个合法动作")
            idx = int(np.flatnonzero(mask)[0])
        cmd = CM.idx_to_command(idx, gs)
        self._fault_tag = None                  # 恢复正常决策 ⇒ 清故障状态
        self.hud(gs, snap, mask, idx, top, notes)
        self.record(gs, snap, mask, idx, cmd, top, notes, ac)
        send(cmd)

    def step_select(self, gs, ac) -> None:
        scr = gs.get("screen_state") or {}
        sel = list(scr.get("selected") or scr.get("selected_cards") or [])
        num = int(scr.get("max_cards") or scr.get("num_cards") or 1)
        # ⚠️ 完成按钮有两种：多数选牌屏是 `proceed`；**药水类「任意多选」屏（Elixir 等）是
        #    `confirm`**（2026-09-23 真机实测：不认它 ⇒ 假故障「选牌帧无候选且无 proceed」）。
        done = "proceed" if "proceed" in ac else ("confirm" if "confirm" in ac else None)
        if done and (len(sel) >= num or scr.get("can_pick_zero")):
            send(done)
            return
        snap, notes = CM.game_state_to_snapshot(gs, self.map_ctx)
        mask = CM.legal_action_mask(gs, ac)
        if not mask.any():
            if done:
                send(done)          # 空屏（如 0 张可选的收尾帧）：完成按钮就是唯一动作
            else:
                self.fault("select", "选牌帧无候选且无 proceed/confirm")
            return
        idx, top = self.policy.act(gs, snap, mask)
        if not mask[idx]:
            idx = int(np.flatnonzero(mask)[0])
        cmd = CM.idx_to_command(idx, gs)
        self._fault_tag = None
        self.hud(gs, snap, mask, idx, top, notes)
        self.record(gs, snap, mask, idx, cmd, top, notes, ac)
        send(cmd)

    # ---- 非战斗胶水（"仅战斗"测试：只求推到下一场战斗）----------------------
    def glue(self, gs, ac) -> None:
        screen = gs.get("screen_type") or "NONE"
        if screen == "MAP":
            self.remember_map(gs)
            if "choose" in ac:
                send("choose 0")                            # 第一条可达路线；choose 是大小写不敏感
            elif "proceed" in ac:
                send("proceed")
            else:
                self.fault("glue", "MAP 屏无 choose/proceed")
        elif screen == "CARD_REWARD":
            send("return" if "return" in ac else "proceed")  # 跳过卡（"仅战斗"测试）
        elif screen == "COMBAT_REWARD":
            send("proceed")
        elif screen in ("SHOP_ROOM", "SHOP_SCREEN", "CHEST"):
            send("proceed")
        elif screen == "REST":
            send("choose 0")
        elif screen == "BOSS_REWARD":
            send("choose 0")
        elif screen == "EVENT":
            send("choose 0" if "choose" in ac else "proceed")
        else:
            if "proceed" in ac:
                send("proceed")
            elif "choose" in ac and gs.get("choice_list"):
                send("choose 0")
            else:
                self.fault("glue", f"未识别屏幕 {screen}，commands={ac}")

    def idle(self, tag: str) -> None:
        """观察模式：非战斗**不发任何命令** —— 游戏保持现状，人类照常操作。

        CommunicationMod 的语义：进程不回命令不等于卡死；状态一旦变化（比如进战斗）
        会再发新状态 ⇒ 我们在那时接管。日志按 tag 去重，避免人类操作时刷屏。
        """
        if tag != self._last_idle:
            self._last_idle = tag
            log(f"🙋 观察模式：{tag} → 不操作（人工驾驶；进战斗自动接管）")

    def fault(self, tag: str, why: str) -> None:
        """真·故障（处理异常 / 输入相位却无合法动作 / 屏幕无法识别）。

        **正常帧绝不回发** —— 敌方回合、动画过渡都会自然带来下一条状态；
        state 重询只属于故障恢复，显式打 🚨 故障标并按 ≤ 1 次/_FAULT_RESEND_S 限频。
        """
        if self._fault_tag != tag:
            self._fault_tag = tag
            log(f"🚨 故障：{why} ⇒ 不操作（等状态更新；重询 ≤ 1 次/{_FAULT_RESEND_S:.0f}s）")
        self.retry_state()

    def retry_state(self) -> None:
        """故障态的 state 重询（≤ 1 次/_FAULT_RESEND_S）。**正常流程绝不调用。**"""
        now = time.time()
        if now - self._fault_t >= _FAULT_RESEND_S:
            self._fault_t = now
            send("state")

    def remember_map(self, gs) -> None:
        for n in gs.get("map") or []:
            self.map_nodes[(n.get("x"), n.get("y"))] = n
        # 当前节点：取"下一行"里最可能刚走过的那层——v1 简化：记录下一步子节点集合即可
        self._pending_next = [n for n in (gs.get("screen_state") or {}).get("next_nodes") or []]

    def note_chosen_map(self) -> None:
        """选完地图后（下一次可见地图时）无法直接知道选哪条 ⇒ v1 在 MAP 屏幕按下时记录。"""
        if not getattr(self, "_pending_next", None):
            return
        n0 = self._pending_next[0]
        node = self.map_nodes.get((n0.get("x"), n0.get("y")))
        if node:
            self.map_ctx = {"symbol": node.get("symbol"),
                            "children": [c.get("symbol") for c in node.get("children") or []]}

    # ---- 收尾 --------------------------------------------------------------
    def finish(self, code: int) -> None:
        log(f"== 结束：战斗 {self.fights_done} 场，残留错误 {self.errors} ==")
        if self.rec:
            self.rec.close()
        if _LOG_F:
            _LOG_F.flush()
        os._exit(code)                                     # 不触发 atexit；游戏侧自行处理进程退出


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description="CommunicationMod 客户端：真机仅战斗可视化测试")
    ap.add_argument("--ckpt", default=None, help="AZNet 存档（建网配置在同文件里）")
    ap.add_argument("--policy", default="auto", choices=["auto", "net", "heuristic", "random"])
    ap.add_argument("--character", default="IRONCLAD")
    ap.add_argument("--ascension", type=int, default=0)
    ap.add_argument("--seed", default=None, help="游戏种子（可选，按显示格式的字母数字串）")
    ap.add_argument("--glue", action="store_true",
                    help="无人值守胶水模式：自动开局/走地图/跳奖励/自动重开；"
                         "默认（不带）= 观察模式：非战斗不操作、只接管战斗")
    ap.add_argument("--games", type=int, default=1, help="连打 N 局（仅 --glue 模式有效）")
    ap.add_argument("--fights", type=int, default=0,
                    help="打满 N 场战斗后退出（0 = 不限；两种模式都生效）")
    ap.add_argument("--record", default=None, help="决策 JSONL 输出路径（相对路径按仓库根解读）")
    ap.add_argument("--replay", default=None, help="离线回放：只重渲染 HUD（不连游戏）")
    ap.add_argument("--hud", action="store_true", help="打印终端 HUD（stderr/日志）")
    return ap.parse_args(argv)


def replay(path: str, policy) -> None:
    print(f"== 回放 {path} ==")
    n = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            gs = r["gs"]
            snap = np.asarray(r["obs"], dtype=np.int64)
            mask = CM.legal_action_mask(gs, r.get("ac"))
            top = [(i, p) for i, p in r.get("top") or []]
            print(render_hud(gs, snap, mask, r["action"], top, r.get("notes") or [], 0))
            if policy is not None:
                idx, top2 = policy.act(gs, snap, mask)
                tag = "同" if idx == r["action"] else "异"
                print(f"   ↳ 现策略：{CM.action_desc(idx, gs)}（{tag}；录得 {CM.action_desc(r['action'], gs)}）")
            n += 1
    print(f"== 共 {n} 条决策 ==")


def main() -> None:
    global _LOG_F
    args = parse_args()
    os.chdir(ROOT)                                   # 游戏拉起的进程 CWD 不可控 ⇒ 锚到仓库根
    os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
    _LOG_F = open(os.path.join(ROOT, "data", f"comm_{_START_TS}.log"), "a", encoding="utf-8")
    log(f"启动：policy={args.policy} ckpt={args.ckpt} char={args.character} asc={args.ascension} "
        f"games={args.games} fights={args.fights}")
    if CM.LAYOUT_WARN:
        log(CM.LAYOUT_WARN)
    if args.replay:
        pol = None
        if args.ckpt:
            try:
                pol = NetPolicy(args.ckpt)
            except Exception as e:                                          # noqa: BLE001
                log(f"⚠️ 回放时网络不可用：{type(e).__name__}: {e}")
        replay(args.replay, pol)
        return

    policy = build_policy(args)
    client = Client(args, policy)
    log("等 CommunicationMod 的状态…（进程由 mod 拉起；手动跑不会有输入）")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            log(f"⚠️ 非 JSON 行（忽略）：{line[:120]!r}")
            continue
        try:
            client.on_message(msg)
        except Exception as e:                                              # noqa: BLE001
            client.fault("except", f"处理异常：{type(e).__name__}: {e}")


if __name__ == "__main__":
    main()
