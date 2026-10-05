"""整局漫游 harness（**库**，无 CLI）：从涅奥开始随机沿地图往下走，
遇到**战斗房间**（MONSTER / ELITE / BOSS）就地打完，直到分出胜负（通关或战败）。
跑满 Act 1..3（每幕 16 个节点：地图第 0..14 行 + 第 15 行固定 Boss）。

消费者：
  · `eval_walk.py` —— 同分布胜率主评（自带漫游循环，只借用这里的策略装配）
  · `_chk_shape.py` —— 守卫，patch `play_battle` 后用 `run_one` 取真实 `(floor, act)` 观测点
CLI 已删：整局「能不能跑到终局 / 某策略能走多远」都看 `eval_walk.py`（同分布漫游、配对 seed）；

策略 spec（`parse_spec`，与 `train` / `dump_battle` 的走法语法同源）：
    random              随机
    teacher:<sims>      C++ UCT 教师
    net:<path>[:<sims>] 训练好的 AZNet；**省略 sims 或 sims=0 = 裸网络**，>0 = MCTS(sims)

并行：`_init_worker` 在**每个进程内**建一次策略（策略含闭包、spawn 下不可 pickle ⇒
只传路径，worker 自己 `torch.load`）；调用方用 `initargs=((spec, batch, threads),)` 装参。
实测**串行与并行逐位一致**（同 seed 同终局/同战斗数）。

⚠️ 权重只在**远端**：本地 `data/` 是空的（`*.pt` 被 gitignore，缓存是远端产物）。
`net:` 模式要么把 `.pt` 拷回本地 `data/`，要么在远端跑（`./sync.sh run "…"`）。

本文件是**漫游 harness + 策略装配**：不含评测口径（口径与阅读纪律见 `eval_walk.py`），
也不含环境层（整局线的 `runenv.Episode` / `RunEnv` 已于 2026-10-06 随整局线退役）。

⚠️ 随机源：每局一个 `random.Random(seed)` 实例（地图与奖励共用）。
`aznet.random_action` 用的是全局 `random` 且**不播种** ⇒ 那个基线每次都不一样、不能配对比较；
这里每局独立播种 ⇒ **同 seed 逐位可复现**，可以拿来做回归。
（网络/教师策略本身是确定性的，所以整局仍随 seed 复现。）

⚠️ 房间类型以**移动后**的 `gc.cur_room` 为准（`gc.map.get_room_type(x, y+1)` 只是移动前的预测），
且事件房会被引擎**改判**成 MONSTER / SHOP / TREASURE，只有移动后才知道真实类型。

⚠️⚠️ **网络是在 `boss` 场景（Act 1 Boss + `_boss.build_deck` 的 ~20 张卡组）上训练的**，
而整局漫游里卡组从初始 10 张慢慢长起来 —— 这是**分布外**。
所以别看绝对数字，要拿 `teacher:<sims>` 在同一批 seed 上跑一遍当参照
（阅读纪律见 `eval_walk.py` 开头的口径声明）。

事件房：按 `gc.valid_event_options` 掩码取**最低合法位**（掩码 0 才离开）。2026-09-17
「事件房会硬崩」的结论**前提是当时只能盲选选项**；`valid_event_options` 绑上后，
`_chk_run.py` 实测 2000 局 / 48 种事件 / 0 崩溃（该守卫 2026-09-30 退役，读数在 `.agent/notes.md`）
⇒ 事件房照进（不再丢事件收益）。
"""
import os
import random

import slaythespire as sts

import ckpt

CHARACTER, ASCENSION = sts.CharacterClass.IRONCLAD, 0

MAX_LOOP = 30000            # 整局最多处理多少个「屏幕」，兜底防死循环
MAX_BATTLE_STEPS = 20000    # 单场战斗最多多少个动作

# 会进战斗的房间（BOSS 是每幕第 15 层，引擎在 transition_to_map_node 里强制指定）
BATTLE_ROOMS = (sts.Room.MONSTER, sts.Room.ELITE, sts.Room.BOSS)

# PUCT 探索常数：与 `dump_battle.py` / `train.report` 保持一致（1.5）
C_PUCT = 1.5


# ---------------- 地图 ----------------

def map_choices(gc):
    """当前节点可达的下一层 x 列表（开局 cur_map_node 是 (-1,-1)，同一套写法适用）。"""
    return [x for x in range(7) if gc.map.has_edge(gc.cur_map_node_x, gc.cur_map_node_y, x)]


def step_map(gc, rng):
    """在可达节点里**均匀随机**走一步，返回移动后**实际**的房间类型 gc.cur_room。

    没有可达节点时返回 None。走法是**真均匀**（整局线那套 `runenv.random_map_policy` 带房间偏好，
    已于 2026-10-06 退役）——本脚本要的就是「随便乱走」这条路径本身能不能走通。
    """
    xs = map_choices(gc)
    if not xs:
        return None
    gc.transition_to_map_node(rng.choice(xs))
    return gc.cur_room


# ---------------- 战斗策略 ----------------

def random_choose(b, rng):
    """随机策略：合法动作里均匀抽一个。

    ⚠️ **不能把 END_TURN 混进均匀抽样**：3 张手牌时它有 1/4 概率被抽中，
    于是每回合平均只打 1 张牌就挨一整轮打 —— 实测 **Act 1 第 1 场就死**（seed 1、2 都死在第 1 层）。
    这里「随机」指的是**随机选哪张牌**，回合只在没牌可打时才结束。
    """
    acts = b.enumerate_actions()
    if not acts:
        return None
    plays = [a for a in acts if a.action_type != sts.ActionType.END_TURN] or acts
    return rng.choice(plays)


# ---------------- 战斗 ----------------

def play_battle(gc, choose, rng):
    """用 `choose(b)` 打完一场战斗，返回 (是否胜利, 动作数)。

    ⚠️ `BattleContext.init` 每个对象**只能调一次**（第二次是 UB）⇒ 每场战斗新建一个。
    ⚠️ `choose` 只应从 `b.enumerate_actions()` 这类**引擎权威枚举**里挑
       （需要目标的牌自动带合法目标），自己按手牌下标拼会撞 C++ 的 `isValidAction` 断言。
    """
    b = sts.BattleContext()
    b.init(gc)
    steps = stall = 0
    for _ in range(MAX_BATTLE_STEPS):
        if b.is_battle_over():
            break
        a = choose(b, rng)
        if a is None:
            if b.input_state == sts.InputState.CARD_SELECT:
                b.select_card(0)     # 选牌态没有可枚举动作时，取第 0 张（end_turn 在这里非法）
            else:
                b.end_turn()
            steps += 1
            continue
        before = b.player_energy
        a.execute(b)
        steps += 1
        # 连续 5 步能量没变 = 卡住了（0 费动作反复打 / 什么都不发生）→ 强制结束回合
        stall = 0 if b.player_energy != before else stall + 1
        if stall >= 5 and b.input_state == sts.InputState.PLAYER_NORMAL:
            b.end_turn()
            stall = 0
    b.exit_battle(gc)             # 败则 gc.outcome = PLAYER_LOSS，胜则走 regainControl（奖励屏）
    return b.outcome == sts.Outcome.PLAYER_VICTORY, steps


# ---------------- 一局 ----------------

def run_one(seed, choose, verbose=False):
    """跑完整一局，返回统计字典（`outcome` 存**字符串**，方便跨进程回传）。"""
    rng = random.Random(seed)
    gc = sts.GameContext(CHARACTER, seed, ASCENSION)
    gc.choose_event_option(0)                    # 涅奥：4 个选项里取第 0 个

    rooms, battles, wins, actions, loops = {}, 0, 0, 0, 0

    while gc.outcome == sts.GameOutcome.UNDECIDED and loops < MAX_LOOP:
        loops += 1
        s = gc.screen_state

        if s == sts.ScreenState.MAP_SCREEN:
            room = step_map(gc, rng)
            if room is None:
                break
            rooms[room.name] = rooms.get(room.name, 0) + 1
            if room in BATTLE_ROOMS:             # ← 战斗房间：就地打完
                battles += 1
                won, n = play_battle(gc, choose, rng)
                wins += int(won)
                actions += n

        elif s == sts.ScreenState.REWARDS:
            while gc.card_reward_count > 0:
                cards = gc.get_card_reward()
                if not cards:
                    break
                gc.pick_reward_card(rng.choice(cards))
            gc.regain_control()

        elif s == sts.ScreenState.BOSS_RELIC_REWARDS:
            gc.choose_boss_relic(3)              # 3 = 跳过

        elif s == sts.ScreenState.EVENT_SCREEN:
            # 合法选项掩码（bit k == `choose_event_option(k)` 合法，自带屏态门控）。
            # 掩码 0 = 真没有可选项（如 LAB 直接进奖励屏）⇒ 只能离开，与 SHOP_ROOM 同样处理。
            # 涅奥不走这里 —— 它在建局后单独用 `choose_event_option(0)`。
            mask = gc.valid_event_options
            if mask == 0:
                gc.screen_state = sts.ScreenState.MAP_SCREEN
            else:
                gc.choose_event_option((mask & -mask).bit_length() - 1)

        elif s == sts.ScreenState.SHOP_ROOM:
            gc.screen_state = sts.ScreenState.MAP_SCREEN   # 商店没绑购买接口，直接离开

        elif s == sts.ScreenState.REST_ROOM:
            gc.choose_campfire_option(0)         # 0 = 休息（回 30% 血）

        elif s == sts.ScreenState.TREASURE_ROOM:
            gc.choose_treasure_room_option(False)  # 不开箱

        elif s == sts.ScreenState.CARD_SELECT:
            gc.choose_select_card_screen_option(0)

        elif s == sts.ScreenState.BATTLE:
            # 正常路径在 MAP_SCREEN 分支里就按 cur_room 打完了；走到这里说明
            # 出现了「screen 是 BATTLE 而 cur_room 不在 BATTLE_ROOMS」的意外 —— 照打，别丢局。
            battles += 1
            won, n = play_battle(gc, choose, rng)
            wins += int(won)
            actions += n

        else:
            raise RuntimeError(f"seed {seed}: 未处理的屏幕 {s!s}")

    stats = {"seed": seed, "outcome": gc.outcome.name, "act": gc.act, "floor": gc.floor_num,
             "cur_hp": gc.cur_hp, "max_hp": gc.max_hp, "battles": battles, "wins": wins,
             "actions": actions, "screens": loops, "rooms": rooms,
             "deck": len(gc.deck), "gold": gc.gold}
    if verbose:
        print(f"[{seed}] {gc.outcome.name:14s} Act{gc.act} 层{gc.floor_num:2d}  "
              f"HP={gc.cur_hp:3d}/{gc.max_hp}  战斗={battles}(胜{wins})  动作={actions}  "
              f"牌组={len(gc.deck)}  房间={rooms}", flush=True)
    return stats


# ---------------- 策略装配（父进程解析 spec，worker 里重建） ----------------
#
# 为什么走 `_MP` 全局 + initializer：策略函数含 `net` / 闭包，**spawn 模式下不可 pickle**，
# 所以只传「可 picklable 的 spec」（网络是**路径**，每个进程自己 `torch.load`），
# 由 initializer 在**每个进程内**重建策略（网络以**路径**传入，worker 自己 `torch.load`
# —— 权重 ~660 KB，重复加载比跨进程搬运更省心）。
#
# ⚠️ 父进程串行模式也走 `_init_worker`，所以「串行 vs 并行」用的是**同一份装配代码**，
#    不会出现「并行跑的是另一套策略」。

_MP = {}


def parse_spec(spec):
    """`random` | `teacher:<sims>` | `net:<path>[:<sims>]` → 可 picklable 的元组。

    ⚠️ 多余/未知的写法**直接报错**（`ValueError` / `FileNotFoundError`），不静默兜底。
    """
    p = spec.split(":")
    if p[0] == "random" and len(p) == 1:
        return ("random",)
    if p[0] == "teacher" and len(p) == 2:
        return ("teacher", int(p[1]))
    if p[0] == "net" and len(p) in (2, 3):
        sims = int(p[2]) if len(p) == 3 else 0
        if not os.path.exists(p[1]):
            raise FileNotFoundError(f"找不到权重 {p[1]}")
        return ("net", p[1], sims)
    raise ValueError(f"无法解析策略 {spec!r}；可选：random | teacher:<sims> | net:<path>[:<sims>]")


def _init_worker(cfg):
    """每个进程（含父进程串行模式）调一次：按 `cfg = (spec, batch, threads)` 填好 `_MP`。

    **建网配置从权重存档里读**（`ckpt.config_of`），不从外部旋钮猜 —— 权重能 load 但结构与
    旋钮不符时会得到静默错误的结果（同 `collect_selfplay._init_worker`）。
    `batch` = MCTS 叶批量（**不能硬编码 1**：`_gen_eval.csw` 踩过，batch 本身值 ~5pp；
    该脚本 2026-09-30 退役）；
    `threads` = 本进程 torch 线程数（与采集同源，否则浮点归约顺序变了不可比）。
    """
    spec, batch, threads = cfg
    _MP.clear()
    _MP.update(spec=spec, batch=batch)
    if spec[0] == "teacher":
        from aznet import teacher_action
        sims = spec[1]

        def choose(b, _rng):
            got = teacher_action(b, sims, _rng)  # → (π, mask, is_select, Action, root_win_rate)
            return None if got is None else got[3]
        _MP["choose"] = choose

    elif spec[0] == "net":
        import torch
        from aznet import AZNet, inference_net, mcts_action, net_action, net_kwargs

        torch.set_num_threads(threads)
        net_cfg = ckpt.config_of(spec[1])
        if not net_cfg:
            raise RuntimeError(
                f"{spec[1]} 存档里没有建网配置（旧的两文件格式或裸权重）；"
                f"用 `python ckpt.py {spec[1]}` 查看，必要时 `ckpt.migrate`")
        net = AZNet(**net_kwargs(net_cfg))
        net.load_state_dict(ckpt.weights_of(spec[1]))
        net.eval()
        net = inference_net(net, intra_threads=threads)      # 推理前向；线程数落到 torch
        sims = spec[2]
        choose = ((lambda b, _rng: net_action(net, b)) if sims == 0
                  else (lambda b, _rng: mcts_action(net, b, sims, C_PUCT, batch)))
        _MP["choose"] = choose
        _MP["net"] = net

    else:                                        # random
        _MP["choose"] = random_choose


def make_choose():
    return _MP["choose"]


def _job(seed):
    return run_one(seed, _MP["choose"])
