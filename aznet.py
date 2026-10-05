"""网络 + 全量重导出 —— RL 侧的**唯一入口**。

    from aznet import AZNet, OBS_DIM, teacher_action, net_action
    net = AZNet(**CONFIG).eval()          # 评估前必须 eval（dropout）
    a = net_action(net, b)

实现按职责拆到三个模块，这里只做重导出（`from X import *` 同时满足导入方的扁平用法）：

    azlayout.py  观测布局 → 下标 / 归一化 / 名义类别        纯数据，无模型无搜索
    azaction.py  动作空间 + 编码约定 + 策略函数
    azsearch.py  PUCT 搜索（MCTSNode / mcts_search / mcts_action）
    aznet.py     **本文件**：FactorPolicyHead / AZNet + 上面的重导出

⚠️ 下划线开头的名字 `import *` **不带**，必须单独列出。
⚠️ 导入方一律继续 `from aznet import ...`，**表面与拆分前一致**（连 `aznet.np` / `aznet.sts` 都在）。
"""
import inspect
import os

import torch
import torch.nn as nn
import torch.nn.functional as Fn

from azaction import *                      # noqa: F401,F403
from azlayout import *                      # noqa: F401,F403
from azlayout import (INPUT_STATE_CARD_SELECT, P_INPUT_STATE,  # noqa: F401
                      SELECT_REGION_IDX)
# （下划线名 `import *` 不带，必须显式）
from azsearch import *                      # noqa: F401,F403


# ---------------------------------------------------------------- 网络
# 战斗网的**建网超参**：五个嵌入宽度 + 初始化尺度。**本网自己一套** —— 整局网那份（`multi_hot`
# 的 `RELIC_RANK`/`POTION_RANK`）已于 2026-10-06 随整局线退役，与本网无关。
# ⚠️ 这五个宽度**直接决定 `in_dim`**（换值 = 作废旧权重）；`hidden` / `dropout` / `depth` 只改容量。
# 这里是**唯一来源**：`AZNet` 签名默认值与 `net_cfg()` 都引用它们，别在别处再写这些数字。
CARD_RANK, MONSTER_RANK, INTENT_MOVE_RANK = 8, 8, 8
RELIC_RANK, POTION_RANK, INIT_STD = 8, 8, 0.02

# 容量 / 架构：同样只在这里定义（不是运行参数：无 CLI flag，也不进权重名；换值 = 换网）。
HIDDEN, DEPTH, DROPOUT = 256, 3, 0.2
ARCH, SEL_ARCH = "factor", "factor"


def net_cfg() -> dict:
    """建网配置 = `AZNet` 形参的默认值（数值只来自上面的常量）。写进存档 `config`，供
    `train.check_init()` 比对；键名手写 ⇒ 与 `AZNet.__init__` 的一致性由 `_chk_pool.py` 兜。"""
    return dict(hidden=HIDDEN, depth=DEPTH, arch=ARCH, sel_arch=SEL_ARCH, dropout=DROPOUT,
                card_rank=CARD_RANK, monster_rank=MONSTER_RANK,
                intent_move_rank=INTENT_MOVE_RANK, relic_rank=RELIC_RANK,
                potion_rank=POTION_RANK, init_std=INIT_STD)


def net_kwargs(cfg) -> dict:
    """从配置 dict 里挑出 `AZNet.__init__` 认得的键，并**打印**被丢掉的。
    """
    ok = set(inspect.signature(AZNet.__init__).parameters) - {"self"}
    extra = sorted(set(cfg) - ok)
    if extra:
        print(f"⚠️ 配置里有 {extra} 不是 `AZNet.__init__` 的参数（存档里多出来的键？）—— 已忽略",
              flush=True)
    return {k: v for k, v in cfg.items() if k in ok}


class FactorPolicyHead(nn.Module):
    def __init__(self, hidden, card_rank, monster_rank, intent_move_rank, potion_rank):
        super().__init__()
        card_dim = card_rank + HAND_EXTRA_N
        # 目标行 = 两个 id 嵌入 + **除名义列外**的数值列（与手牌 key 同形：嵌入在前、标量在后）
        tar_dim = monster_rank + intent_move_rank + MONSTER_EXTRA_N
        # ── key: cat(emb, scalars) → linear ──
        self.k_hand = nn.Linear(card_dim, hidden)
        self.k_potion = nn.Linear(potion_rank, hidden)
        self.discard_target = nn.Parameter(torch.zeros(tar_dim))        # 药水目标 6「丢弃」（虚拟行）
        self.none_target = nn.Parameter(torch.zeros(tar_dim))           # 目标 0「无目标」（虚拟行）
        # ── score：(q + key + target) → linear ──
        self.q = nn.Linear(hidden, hidden)                              # 状态查询
        self.k_target = nn.Linear(tar_dim, hidden)
        self.out = nn.Linear(hidden, 1)                                 # 槽 × 目标 → logit
        self.end_head = nn.Linear(hidden, 1)                            # END_TURN

        # 交叉矩阵（`n_tar`）：score 里加一项 `key ⊙ target`（逐元素积 = 双线性项）。
        # **没有开关** —— 2026-10-02 起常开（原来 `pair="none"` 时它白占参数、梯度恒 None）。
        self.n_tar = nn.Linear(tar_dim, hidden)
        for m in (self.k_hand, self.k_potion, self.q, self.k_target, self.n_tar):
            nn.init.normal_(m.weight, std=1.0 / hidden ** 0.5)
            nn.init.zeros_(m.bias)

    def forward(self, state, hand_emb, hand_extra_scalars, mon_id_emb, mon_move_emb,
                mon_extra_scalars, pot_emb):
        #   state               [B, hidden]                             trunk 输出
        #   hand_emb            [B, HAND_SLOTS, card_rank]              手牌槽卡牌嵌入
        #   hand_extra_scalars  [B, HAND_SLOTS, HAND_EXTRA_N]           手牌额外标量列
        #   mon_id_emb          [B, MONSTER_SLOTS, monster_rank]        怪 id 嵌入
        #   mon_move_emb        [B, MONSTER_SLOTS, intent_move_rank]    怪招式 id 嵌入
        #   mon_extra_scalars   [B, MONSTER_SLOTS, MONSTER_EXTRA_N]     怪额外标量列（不含两个 id 列）
        #   pot_emb             [B, POTION_SLOTS, potion_rank]          药水槽嵌入
        query = self.q(state).unsqueeze(1).unsqueeze(2)                                             # [B, 1, 1, hidden]
        mon = torch.cat([mon_id_emb, mon_move_emb, mon_extra_scalars], dim=-1)                      # [B, MONSTER_SLOTS, tar_dim]

        key_hand = self.k_hand(torch.cat([hand_emb, hand_extra_scalars], dim=-1))                   # [B, HAND_SLOTS, hidden]
        hand_tgt = torch.cat([self.none_target.expand_as(mon[:, :1]), mon], dim=1)                  # add None to targets head
        # 加性主效应 + **交叉矩阵**（key ⊙ target 逐元素积）
        hand_score = (query + key_hand.unsqueeze(2) + self.k_target(hand_tgt).unsqueeze(1)
                      + key_hand.unsqueeze(2) * self.n_tar(hand_tgt).unsqueeze(1))                  # [B, HAND_SLOTS, MONSTER_SLOTS + 1, hidden]

        key_potion = self.k_potion(pot_emb)                                                         # [B, POTION_SLOTS, hidden]
        potion_tgt = torch.cat([hand_tgt, self.discard_target.expand_as(hand_tgt[:, :1])], dim=1)   # add Discard to targets end
        potion_score = (query + key_potion.unsqueeze(2) + self.k_target(potion_tgt).unsqueeze(1)
                        + key_potion.unsqueeze(2) * self.n_tar(potion_tgt).unsqueeze(1))            # [B, POTION_SLOTS, MONSTER_SLOTS + 2, hidden]

        # hand + potion + END_TURN
        parts = [self.out(torch.relu(hand_score)).flatten(1), self.out(torch.relu(potion_score)).flatten(1), self.end_head(state)]
        return torch.cat(parts, dim=-1)


class FactorSelectHead(nn.Module):
    def __init__(self, hidden, card_rank):
        super().__init__()
        # ── key: cat(emb, scalars) → linear ──
        self.k_cand = nn.Linear(card_rank + SEL_EXTRA_N, hidden)        # (卡牌嵌入 ⊕ 额外标量列) → key
        # ── score：(q + key) → linear ──
        self.q = nn.Linear(hidden, hidden)                              # 状态查询
        self.out = nn.Linear(hidden, 1)                                 # 槽 → logit
        for m in (self.q, self.k_cand):
            nn.init.normal_(m.weight, std=1.0 / hidden ** 0.5)
            nn.init.zeros_(m.bias)

    def forward(self, state, cand_emb, cand_extra_scalars):
        #   state               [B, hidden]                             trunk 输出
        #   cand_emb            [B, SELECT_DIM, card_rank]              候选槽卡牌嵌入
        #   cand_extra_scalars  [B, SELECT_DIM, SEL_EXTRA_N]            候选槽额外标量列
        query = self.q(state).unsqueeze(1)                              # [B, 1, hidden]
        key_cand = self.k_cand(torch.cat([cand_emb, cand_extra_scalars], dim=-1))   # [B, SELECT_DIM, hidden]
        # 无目标维：合法掩码 = 候选 `cardId != 0`，由调用方负责
        return self.out(torch.relu(query + key_cand)).squeeze(-1)                   # [B, SELECT_DIM]


class AZNet(nn.Module):
    """卡牌嵌入 + 数值特征 -> trunk -> {policy_head, select_head, value}。

    形参见 `__init__` 开头的表；三个头的宽度一律引用 `POLICY_DIM` / `SELECT_DIM` / 1。

    卡牌 id 的两种消费方式（见 `card_feats`）：
      槽位（hand / select）—— 逐槽位嵌入（槽位号 == select_head 的动作下标）
      牌堆 + 总牌组       —— **只给集合**：不传堆序（给序 = 读预言机信息），
                             空槽排除在「和」与「分母」外后取均值；总牌组是 run 级不变量

    ⚠️ 嵌入表**一律**「行 0 = 空槽、可学」：`card_embed` / `emb_monster_id` / `potion_embed`
    都不许加 `padding_idx=0`（空槽嵌入恒 0 ⇒ 从学策略变成背训练集）。遗物**没有表**（181 列多热
    走不带 bias 的 `Linear`）；整局网用 `MultiHotEmbed`（行 0 = padding），**别把行约定搬错网**。
    """

    # buffer 必须类级声明：Pylance 看不到 `nn.Module.__getattr__` 的返回类型 ⇒ 用到的每个都要写。
    _idx_card_id: torch.Tensor
    _idx_scalar: torch.Tensor
    _idx_potion_slot: torch.Tensor
    _idx_monster_id: torch.Tensor
    _idx_monster_extra: torch.Tensor
    _idx_hand_extra: torch.Tensor
    _idx_sel_card: torch.Tensor
    _idx_sel_extra: torch.Tensor
    _sel_region_mask: torch.Tensor           # 选牌区掩码

    def __init__(self, hidden=HIDDEN, dropout=DROPOUT, depth=DEPTH,
                 arch=ARCH, sel_arch=SEL_ARCH,
                 card_rank=CARD_RANK, monster_rank=MONSTER_RANK,
                 intent_move_rank=INTENT_MOVE_RANK,
                 relic_rank=RELIC_RANK, potion_rank=POTION_RANK,
                 init_std=INIT_STD):
        super().__init__()
        # ── 形参 ──
        # 五个嵌入宽度（默认值 = 上方的 `*_RANK` 常量）：card_rank / monster_rank /
        #   intent_move_rank 是**逐槽 id** 的宽度，relic_rank / potion_rank 是多热表 / 槽表的秩
        #   （potion_rank 还决定头的 `k_potion` 宽度，intent_move_rank 也进目标行）。
        # 三个训练旋钮（都有 CLI flag，`train.CONFIG.update` 覆盖）：
        # arch     flat = Linear(hidden, POLICY_DIM)  | factor = FactorPolicyHead（手牌槽 × 目标，
        #          **含交叉矩阵**，常开）
        # sel_arch flat = Linear(hidden, SELECT_DIM)  | factor = FactorSelectHead（候选集共享打分）
        assert arch in ("flat", "factor"), arch
        assert sel_arch in ("flat", "factor"), sel_arch

        self.arch, self.depth = arch, depth
        self.sel_arch = sel_arch
        # ⚠️ 网络**不记**配置（2026-09-28 删掉了那条 `locals()` 拄法）：配置就是脚本里的
        # `train.CONFIG`（+ CLI 覆盖），存盘时由 `train.save_ckpt` 直接写它。
        # 别再引入「网络自己记一份」的机制 —— 两份会漂，而脚本里那份是人能直接读到的。

        self.card_embed = nn.Embedding(ENCODED_CARD_ID_TYPES, card_rank)
        nn.init.normal_(self.card_embed.weight, std=init_std)

        self.potion_embed = nn.Embedding(POTION_TYPES, potion_rank)
        nn.init.normal_(self.potion_embed.weight, std=init_std)

        self.relic_linear = nn.Linear(RELIC_TYPES, relic_rank, bias=False)
        nn.init.normal_(self.relic_linear.weight, std=init_std)

        card_dim = card_rank * (len(HAND_CARD_IDX) + len(SEL_CARD_IDX) + len(PILE_CARD_IDX))
        scalar_dim = len(SCALAR_IDX)
        # ── 名义类别段：显式声明（哪几列走嵌入 / 哪几列走 one-hot）──
        # `monster_id` / `intent_move_id` 逐槽嵌入，**0 = 空槽 ⇒ 掩掉**（与牌堆池化同口径）；
        # 其余四列 one-hot（0 是合法值，不掩）。与 `azlayout.CAT_SPEC` 的一致性由下面断言对账。
        self.emb_monster_id = nn.Embedding(MONSTER_ID_TYPES, monster_rank)
        self.emb_intent_move_id = nn.Embedding(MONSTER_MOVE_ID_TYPES, intent_move_rank)

        # 列 / 取值域 / 顺序 与 `CAT_SPEC` 逐项对账
        _net_cols = [MONSTER_ID_IDX, MONSTER_INTENT_MOVE_IDX,
                     [P_INPUT_STATE], [P_SELECT_TASK], [PLAYER_ATTR_OFF], [P_ORANGE_PELLETS]]
        _net_k = [self.emb_monster_id.num_embeddings, self.emb_intent_move_id.num_embeddings,
                  INPUT_STATE_TYPES, CARD_SELECT_TASK_TYPES, STANCE_TYPES, ORANGE_PELLETS_TYPES]
        assert [c for c in _net_cols] == [c for _n, c, _k in CAT_SPEC] and _net_k == [k for _n, _c, k in CAT_SPEC], "`aznet` 显式声明的名义类别（列 / 取值域 / 顺序）与 `azlayout.CAT_SPEC` 漂开了"

        cat_dim = (MONSTER_SLOTS * monster_rank
                   + MONSTER_SLOTS * intent_move_rank
                   + INPUT_STATE_TYPES
                   + CARD_SELECT_TASK_TYPES
                   + STANCE_TYPES
                   + ORANGE_PELLETS_TYPES)

        potion_dim = POTION_SLOTS * self.potion_embed.embedding_dim
        relic_dim = self.relic_linear.out_features
        in_dim = scalar_dim + card_dim + cat_dim + potion_dim + relic_dim

        layers: list[nn.Module] = []
        for i in range(depth):
            layers.append(nn.Linear(in_dim if i == 0 else hidden, hidden))
            layers.append(nn.ReLU())
            layers.append(nn.Dropout(dropout))
        self.trunk = nn.Sequential(*layers)
        if arch == "flat":
            self.policy_head = nn.Linear(hidden, POLICY_DIM)
        else:
            self.policy_head = FactorPolicyHead(hidden, card_rank, monster_rank,
                                                intent_move_rank, potion_rank)
        if sel_arch == "flat":
            self.select_head = nn.Linear(hidden, SELECT_DIM)
        else:
            self.select_head = FactorSelectHead(hidden, card_rank)
        self.value_head = nn.Linear(hidden, 1)
        # ── 布局缓存：全部源自 `azlayout`，名 = 常量名加 `_idx_` ──
        # 为什么是 buffer 而不是普通属性：① `.to(dev)` / `.half()` 会一起搬
        # （否则「obs 在卡上、下标在 CPU」）；② `index_select` **只收 Tensor** ⇒ 免掉每帧转换。
        # `persistent=False`：**派生量不是状态**（由 `azlayout` 唯一决定），进 ckpt 只会多一份会过期的抄本。
        # **没有 `scale`**：归一化已由 C++ 快照生产者做完（除数写在赋值处）⇒ obs 到手就是网络输入。
        #    布局探针因此不再是 buffer：改布局必然改 `N_SCALAR` ⇒ `trunk.0.weight` 形状对不上。
        self.register_buffer("_idx_card_id", torch.as_tensor(CARD_ID_IDX), persistent=False)
        self.register_buffer("_idx_scalar", torch.as_tensor(SCALAR_IDX), persistent=False)
        # 药水逐槽 id（`_inputs` 资源段 + 因子化头的 key 都要用）
        self.register_buffer("_idx_potion_slot", torch.as_tensor(POTION_SLOT_IDX), persistent=False)
        self.register_buffer("_idx_monster_id", torch.as_tensor(MONSTER_ID_IDX), persistent=False)
        self.register_buffer("_idx_monster_extra", torch.as_tensor(MONSTER_EXTRA_IDX), persistent=False)
        self.register_buffer("_idx_hand_extra", torch.as_tensor(HAND_EXTRA_IDX), persistent=False)
        self.register_buffer("_idx_sel_card", torch.as_tensor(SEL_CARD_IDX), persistent=False)
        self.register_buffer("_idx_sel_extra", torch.as_tensor(SEL_EXTRA_IDX), persistent=False)
        # 选牌区掩码：非 `CARD_SELECT` 帧整块置 0（见 `_gate_select_region`）
        m = torch.zeros(OBS_DIM, dtype=torch.bool)
        m[SELECT_REGION_IDX] = True
        self.register_buffer("_sel_region_mask", m, persistent=False)
        self._pile_lens = tuple(len(x) for x in PILE_CARD_IDX)
        # ── 自检 ──
        # 拼宽 == in_dim、两个头的**真实产出** == POLICY_DIM / SELECT_DIM（不看推导）。
        # 「Python 下标跟 C++ 布局漂开」默认是**静默**的，这里是唯一兜底。
        # ⚠️ 临时 eval：trunk 的 Dropout 会吃 RNG（构造期吃 RNG = 动了别处的初始化流）。
        was_training = self.training
        self.eval()
        with torch.no_grad():
            _p, _s, _v = self(torch.zeros(1, OBS_DIM, dtype=torch.float32))
        self.train(was_training)
        assert _p.shape[-1] == POLICY_DIM, f"策略头产出宽 {_p.shape[-1]} != POLICY_DIM {POLICY_DIM}"
        assert _s.shape[-1] == SELECT_DIM, f"选牌头产出宽 {_s.shape[-1]} != SELECT_DIM {SELECT_DIM}"
        with torch.no_grad():
            w = self._inputs(torch.zeros(1, OBS_DIM, dtype=torch.float32)).shape[-1]
        assert w == in_dim, f"输入拼接宽度 {w} != trunk 的 in_dim {in_dim}"

    def cat_feats(self, obs):
        """名义类别段：2 组嵌入（每槽 **0 = 空槽 ⇒ 掩掉**）+ 3 个 one-hot。

        列与顺序显式写死（不用表驱动）；与 `azlayout.CAT_SPEC` 的一致性由 `__init__` 断言对账。
        """
        mid = obs[:, MONSTER_ID_IDX].long()            # [B, 5] 怪物 id（0 = 空槽）
        mvd = obs[:, MONSTER_INTENT_MOVE_IDX].long()   # [B, 5] 招式 id（0 = 空槽）
        parts = [
            self.emb_monster_id(mid) * (mid > 0).unsqueeze(-1),
            self.emb_intent_move_id(mvd) * (mvd > 0).unsqueeze(-1),
            Fn.one_hot(obs[:, P_INPUT_STATE].long(), INPUT_STATE_TYPES).to(torch.float32),
            Fn.one_hot(obs[:, P_SELECT_TASK].long(), CARD_SELECT_TASK_TYPES).to(torch.float32),
            Fn.one_hot(obs[:, PLAYER_ATTR_OFF].long(), STANCE_TYPES).to(torch.float32),
            Fn.one_hot(obs[:, P_ORANGE_PELLETS].long(), ORANGE_PELLETS_TYPES).to(torch.float32),
        ]
        return torch.cat([p.flatten(1) for p in parts], dim=-1)

    def card_feats(self, obs):
        """obs: [B, OBS_DIM] -> [B, card_dim]"""
        # 一次 gather + 一次嵌入（分 5 次会各吃一遍 `nn.Module._call_impl` 的调度开销）。
        # 布局：手牌 | draw | discard | exhaust | 总牌组 | 候选 —— 与 `CARD_ID_IDX` 顺序一致。
        ids = torch.index_select(obs, 1, self._idx_card_id).long()     # [B, 314]（手牌 10 + 3 堆 × 60 + 牌组 64 + 候选 60）
        pe = self.card_embed(ids)                                   # [B, 314, E]
        base = len(HAND_CARD_IDX)
        parts = [pe[:, :base].flatten(1)]            # 手牌：逐槽 concat（槽位有语义）
        for n in self._pile_lens:
            # 每个牌堆**只给集合**：不给堆序（真机看不到序，给序 = 读预言机信息）；
            # 空槽排除在「和」与「分母」外后取均值，分母 = **实际可见张数**（截断时不系统性缩小）。
            blk = pe[:, base:base + n]                              # [B, pile_slots, E]
            mask = (ids[:, base:base + n] > 0).unsqueeze(-1)
            parts.append((blk * mask).sum(dim=1) / mask.sum(dim=1).clamp(min=1))
            base += n
        parts.append(pe[:, base:].flatten(1))                        # 候选
        return torch.cat(parts, dim=-1)

    def _gate_select_region(self, obs):
        """非 `CARD_SELECT` 帧把选牌区置 0：引擎的 `cardSelectInfo` 从不清零，残留值会变成
        「本局发生过哪种选牌」的历史指纹 —— 详见 `azlayout`。

        ⚠️ 非就地：`obs` 是调用方的张量。
        """
        is_sel = obs[:, P_INPUT_STATE] == INPUT_STATE_CARD_SELECT          # [B]
        m = self._sel_region_mask.unsqueeze(0) & ~is_sel.unsqueeze(1)        # [B, OBS_DIM]
        return obs.masked_fill(m, 0.0)

    def _inputs(self, obs):
        """obs -> trunk 的输入（卡牌嵌入段 + 名义类别段 + 标量段）。
        抽出来是为了给 `__init__` 做形状自检（不过 dropout ⇒ 不吃 RNG）。
        """
        obs = self._gate_select_region(obs)      # 无条件
        parts = [self.card_feats(obs), self.cat_feats(obs)]
        # 标量段**已经归一化**（除数在 C++ 赋值处）⇒ 这里只取列，不再除。
        parts.append(torch.index_select(obs, 1, self._idx_scalar).float())
        # ⚠️ 从**原始** `obs` 上取块，不做"替换式"池化：替换会改宽度 ⇒ 后面的预存下标全静默错位。
        # 药水逐槽 id → 裸表（行号 = id，0 = 空槽）。⚠️ `clamp(min=0)` 不能省：负 id（-1 哨兵）会抛错。
        pot_ids = torch.index_select(obs, 1, self._idx_potion_slot).long()
        pot = self.potion_embed(pot_ids.clamp(min=0)).flatten(1)
        parts.append(torch.cat([self.relic_linear(obs[:, RELIC_OFF:RELIC_OFF + RELIC_TYPES]),
                                pot], dim=-1))
        return torch.cat(parts, dim=-1)

    def forward(self, obs):
        # obs: [B, OBS_DIM]
        # 允许 numpy 进来：`SearchTree` 的 obs 由 `sts.batch_snapshot` 产出（numpy）——
        # 已经是 torch 张量时这一行零开销（`isinstance` 一次）。
        if not isinstance(obs, torch.Tensor):
            obs = torch.as_tensor(obs)
        h = self.trunk(self._inputs(obs))
        if self.arch == "factor":
            p = self.policy_head(h, *self.policy_keys(obs))
        else:
            p = self.policy_head(h)
        if self.sel_arch == "factor":
            s = self.select_head(h, *self.select_keys(obs))
        else:
            s = self.select_head(h)
        return p, s, self.value_head(h).squeeze(-1)

    def policy_keys(self, obs):
        """因子化策略头的输入：手牌槽 (卡牌嵌入, 额外标量列) + 目标 (两个 id 嵌入, 额外标量列) + 药水槽嵌入。

        额外标量列 = 块内**除名义列外**的列（手牌 = 费用/特殊位/是否可打；怪 = 10 项数值 —— 12 项
        减去 `monsterId` / `intentMoveId`，那两列各自以嵌入形式**排在目标向量开头**）。
        三张表都在**状态侧** ⇒ 头只做投影：「槽/目标里有它」与「用它」是同一件事、同一空间。
        ⚠️ 手牌槽与怪目标都**必须**带额外标量：只给 id，头就不知道「打不打得起」「该打谁」。
        """
        hand_ids = torch.index_select(obs, 1, self._idx_card_id).long()[:, :len(HAND_CARD_IDX)]
        hand_extra_scalars = torch.index_select(obs, 1, self._idx_hand_extra).float().reshape(-1, len(HAND_CARD_IDX), HAND_EXTRA_N)
        mon_ids = torch.index_select(obs, 1, self._idx_monster_id).long()           # [B, 5] 原始 id
        mon_move_ids = obs[:, MONSTER_INTENT_MOVE_IDX].long()                       # [B, 5] 招式 id
        mon_extra_scalars = torch.index_select(obs, 1, self._idx_monster_extra).float().reshape(-1, MONSTER_SLOTS, MONSTER_EXTRA_N)
        pot_ids = torch.index_select(obs, 1, self._idx_potion_slot).long()
        pot_emb = self.potion_embed(pot_ids.clamp(min=0))   # ⚠️ 负 id（-1 哨兵）会让 Embedding 抛错
        return (self.card_embed(hand_ids), hand_extra_scalars, self.emb_monster_id(mon_ids),
                self.emb_intent_move_id(mon_move_ids), mon_extra_scalars, pot_emb)

    def select_keys(self, obs):
        """因子化 select 头的输入：（候选槽卡牌嵌入, 候选槽额外标量列）。"""
        cand_ids = torch.index_select(obs, 1, self._idx_sel_card).long()
        cand_extra_scalars = torch.index_select(obs, 1, self._idx_sel_extra).float().reshape(-1, SELECT_DIM, SEL_EXTRA_N)
        return self.card_embed(cand_ids), cand_extra_scalars


# ---------------------------------------------------------------- 推理前向
def inference_net(net, intra_threads=1):
    """推理用的前向 —— **就是 `net` 本身**，只把并行度落到 torch 线程数。

    ⚠️ 这里曾经把权重导成 ONNX 交给 ORT 跑（前向快：b=1 约 2×、b=8 约 1.34×）。2026-10-03 换回
    torch：**探索阶段以语义简明优先** —— 少一套系统就少三类静默错：
      ① 权重被烤进图里 ⇒ 每次换权重都得重新导出，且导出内容同时取决于权重**和** `aznet` 的代码；
      ② 导出会把当时的 `train()` / `eval()` 模式**烘进图**（在 train 模式导出 ⇒ 推理静默带 dropout）；
      ③ ORT 与 torch 的 kernel 与图融合不同（实测 ~3e-08 漂移）⇒ 两边采的数据**不可比**。

    代价（实测，本地与远端比例一致）：整次搜索 b=1 **2×**、b=8 **1.34×**、b=32 1.08×。

    ⚠️ `intra_threads` **必须**落到 `torch.set_num_threads`：torch 默认吃满全核，而自对弈是 32 个
    worker 并行 ⇒ 不设就是 32 倍超订（`rollout_threads` 这个配方参数就是它）。
    """
    torch.set_num_threads(intra_threads)
    return net
