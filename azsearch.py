"""PUCT 搜索（网络引导）—— 不做随机 roll-out。

节点的落子/传回全靠 C++ 端的 `BattleContext.clone()` + `execute()`（确定性模拟），
所以这里只负责树与批量前向的调度。

⚠️ **展开在前向之后**（2026-10-02）：先按当前树降 k 条路径、集中一次前向，拿到真先验才建孩子。
   以前是先给**均匀占位先验**建好孩子再前向 ⇒ 同批的后续路径会拿占位先验做选择依据。
   `batch_size=1` 两种写法逐位相同（占位先验从不参与选择）；`>1` 时剩下与 AZ 的差别只有
   「同批互相看不到回传值」—— 叶子并行的固有近似，靠 `virtual_loss` 压低重复选中。
   （AGZ 原文同构：叶进队列、按 minibatch 8 评估、评估期间锁住搜索线程、用 virtual loss 让各
   线程评估不同节点 ⇒ 我们的 `batch_size` 就是那个 minibatch。）
⚠️ **树可跨步复用**（2026-10-03）：`SearchTree` 走完一步就把那个孩子变成新根，**子树连统计一起
   保留**（AGZ 原文的 tree reuse）；`mcts_search` 仍是一次性搜索（每次从零建树）⇒ 单点决策与
   守卫继续用它。复用要求**状态链连续**：`run(b, ...)` 会校验 `b` 与树根逐位相同，对不上直接报错
   —— 不静默重建（重建会让「同一个 SearchTree」静默变成两个不同的搜索）。
⚠️ 本模块被 `aznet` 全量重导出。
"""
import math

import numpy as np
import slaythespire as sts
import torch

from zspec import DEFAULT_SPEC, c_puct_for, q_init_for
from azaction import is_card_select_state, terminal_value

# 根噪声的 Dirichlet **总浓度**（KataGo 口径：围棋 0.03×361 = 10.83）。
# 每动作 α = 总浓度 / 合法动作数 —— 直接拿围棋的每动作 α=0.03 过来，在只有几个动作的战斗里
# 等于“随机挑一个动作给满权重”（总浓度 0.03~0.3），扰动比围棋大一个数量级。
DIRICHLET_TOTAL = 10.83


def _same_state(x, y) -> bool:
    """两个 BattleContext 是否逐位相同。

    快照是引擎给出的**唯一**权威状态口径（`_chk_actions` 的等价性判据用的也是它）。
    """
    return np.array_equal(x.get_snapshot_np(), y.get_snapshot_np())


class MCTSNode:
    """PUCT 节点。每个节点持有一个**克隆**的 BattleContext（确定性模拟）。

    **不回指父节点**：搜索只需向下走（回传靠 `path` 列表），树是单向的。留 `parent` 只会让
    「被 `advance` 丢掉的子树」因 parent↔children 循环引用而无法靠**引用计数**回收（要等分代
    GC，实测垃圾节点是活节点的 20 倍，每个还持着一个上下文）。
    """

    __slots__ = ("b", "prior", "action", "children", "N_VISIT", "WORTH",
                 "nv", "terminal", "tv")

    # `__slots__` 不会给类型检查器任何信息，必须显式声明，否则 `W += tv` 会因为
    # `tv: float | None` 而报「float 与 None 不支持 +」。
    N_VISIT: float
    WORTH: float
    nv: float
    tv: float | None                  # 只有 terminal 节点有值；None = 非终局

    def __init__(self, b, prior=0.0, action=None, zspec=DEFAULT_SPEC):
        self.b = b
        self.prior = prior
        self.action = action
        self.children = {}                 # idx -> MCTSNode
        self.N_VISIT = 0.0
        self.WORTH = 0.0
        self.nv = 0.0                      # 虚拟访问数（同批次内避免重复选中）
        self.terminal = b.is_battle_over()
        # 终局叶的价值**在这里**按训练口径给定；非终局叶稍后由网络头给值 ⇒ 同一尺度。
        self.tv = terminal_value(b, zspec) if self.terminal else None

    def Q(self) -> float:
        return self.WORTH / self.N_VISIT if self.N_VISIT > 0 else 0.0


class SearchTree:
    """一棵可增量搜索、可跨步复用的 PUCT 树。

    `run(b, n)` 跑 `n` 次**新增**模拟（不是「把总数补到 n」）；`advance(idx)` 把根推进到 `idx`
    那条边的孩子（= 上一步真实走的那一步）。两者交替即 AGZ 的 tree reuse：

        tree = SearchTree(b, net, batch_size=8)
        while not b.is_battle_over():
            counts, actions = tree.run(b, 500)
            idx = 选一个（π 采样 / argmax / ε 探索）
            actions[idx].execute(b)
            tree.advance(idx)

    复用**省的是重复劳动**：`advance` 之后新根已经带着上一步探索出的子树与访问统计，新增的模拟
    是在它上面继续加深，而不是从零重建。代价是树在一局内累积 ⇒ 内存随局长度增长。
    """

    def __init__(self, b, net, c_puct=None, batch_size=1, virtual_loss=1.0,
                 zspec=DEFAULT_SPEC, dirichlet_total=DIRICHLET_TOTAL, dirichlet_eps=0.0, rng=None):
        self.net = net
        # `c_puct` 必须与 Q 的尺度匹配（Q 的尺度由值口径决定）：默认按 `zspec` 派生，
        # 显式传值仅供扫描用。
        self.c_puct = c_puct_for(zspec) if c_puct is None else c_puct
        # 未访问节点的 Q 初值 = 该值口径的区间中点（不是 0：`z` 不跨零时 0 等于“最差”）。
        self.q_init = q_init_for(zspec)
        self.batch_size = batch_size
        self.virtual_loss = virtual_loss
        self.zspec = zspec
        # 根节点先验噪声（AGZ：`P(s,a) = (1-ε)p_a + εη_a`，`η ~ Dir(α)`）。
        # `dirichlet_total` 是**总浓度**（不是每动作 α）：每动作 α = 总浓度 / 合法动作数。
        # `eps == 0` 时完全关闭，且不消耗 `rng`；`rng` 由调用方给，保证同 seed 逐位可复现。
        self.dirichlet_total = dirichlet_total
        self.dirichlet_eps = dirichlet_eps
        self.rng = rng
        self.root = MCTSNode(b.clone(), zspec=zspec)
        self._moved = False               # 刚 advance 过 ⇒ 下次 run 要校验状态对齐

    # ---------------------------------------------------------------- 复用
    def advance(self, idx):
        """走 `idx` 这条边：它的孩子成为新根，其余孩子与旧根一起丢弃。

        这里**不校验**状态 —— 真实状态要到下一次 `run(b, ...)` 才拿得到（那时 `b` 已经走完这步）。
        """
        child = self.root.children.get(idx)
        if child is None:
            raise KeyError(f"树里没有 idx={idx} 这条边（advance 前必须先 run 过）")
        self.root = child
        self._moved = True

    def _align(self, b):
        """校验 `advance` 的落实：真实状态必须与树里的新根**逐位相同**。"""
        if not self._moved:
            return
        if not _same_state(self.root.b, b):
            raise RuntimeError(
                "SearchTree 的状态链断了：真实状态与 `advance` 后的树根不一致 —— 树只能服务一条"
                "连续的决策链（单点决策用 `mcts_search`；换了别的战斗要新建树）")
        self._moved = False

    # ---------------------------------------------------------------- 搜索
    def run(self, b, simulations):
        """在真实状态 `b` 上跑 `simulations` 次**新增**模拟，返回 `(idx -> N, idx -> Action)`。

        调用契约：拿到返回值后必须执行 `actions[idx]`（`idx` 由调用方选：π 采样 / argmax / ε），
        再 `advance(idx)` —— 否则下一次 `run` 会因为状态对不上而报错。
        """
        self._align(b)
        root = self.root
        done = 0
        while done < simulations:
            k = min(self.batch_size, simulations - done)
            leaves, paths = [], []

            # ---- 1) 降 k 条路径（**不展开**：真先验要等前向回来才有） ----
            for _ in range(k):
                node, path = root, [root]
                while not node.terminal and node.children:
                    best, best_u, sq = None, float("-inf"), math.sqrt(node.N_VISIT + node.nv)
                    for ch in node.children.values():
                        tot = ch.N_VISIT + ch.nv
                        q = ((ch.WORTH - ch.nv * self.virtual_loss) / tot
                             if tot > 0 else self.q_init)
                        u = q + self.c_puct * ch.prior * sq / (1.0 + tot)
                        if u > best_u:
                            best_u, best = u, ch
                    if best is None:
                        break
                    # 侵占（virtual loss）**沿路径**打，同 lc0 的 `n_in_flight_`：选择时它进
                    # `tot`、也压低 q ⇒ 同批后续路径在**每个分岔点**就避开，而不是挤到同一个叶上。
                    # 根不打（根上只会抬高 sqrt(N)，不改变分岔）。
                    best.nv += self.virtual_loss
                    node, path = best, path + [best]

                if node.terminal:
                    # `tv` 只在 terminal 节点上非 None；能走到这个分支就已经是 terminal 了。
                    # 断言既做类型收窄（否则 `W += None` 报错），也把不变量钉死。
                    tv = node.tv
                    assert tv is not None, "terminal 节点缺少终局价值"
                    for n in path[1:]:                 # 终局没有回传阶段，标记在这里还回去
                        n.nv -= self.virtual_loss
                    for n in path:
                        n.N_VISIT += 1.0
                        n.WORTH += tv
                    done += 1
                    continue

                leaves.append(node)
                paths.append(path)

            if not leaves:
                continue

            # ---- 2) 一次批量前向 ----
            # 快照由引擎一次填成 (k, snapshot_len) float32（省掉 k 次 int32 分配 + stack + cast）。
            obs = sts.batch_snapshot([nd.b for nd in leaves])
            with torch.no_grad():
                p_act, p_sel, val = self.net(obs)
            vals = val.reshape(-1).tolist()
            # 后处理（掩码 + softmax）走 numpy：torch 的逐项 `mask[i] = True` 与标量 softmax 在
            # b=1 下占整个搜索约 1/4，同活 numpy 便宜约 8 倍。
            p_act, p_sel = p_act.numpy(), p_sel.numpy()

            # ---- 3) 展开（此时才有真先验）+ 回传 ----
            for j, (node, path) in enumerate(zip(leaves, paths)):
                for n in path[1:]:                     # 撤销沿路径的侵占标记
                    n.nv -= self.virtual_loss
                # 同批两条路径可能落在同一个叶上 ⇒ 只展开一次。
                # 两边的先验来自同一个状态的前向、等价；重复展开反而会冲掉已建的孩子。
                if not node.children:
                    logits = (p_sel[j] if is_card_select_state(node.b) else p_act[j])
                    # 一次跨语言调用完成「枚举 + 逐个克隆并执行」（旧写法是每个动作三次往返）。
                    # 非终局必有合法动作（引擎保证枚举非空）⇒ 空表不可达，这里不给兜底。
                    cands, kids = node.b.expand_children()
                    idxs = [i for i, _a in cands]
                    # 只对候选位置归一化：非法位置的 exp 是 0、不进分母 ⇒ 与全量 softmax 等价。
                    sel = logits[idxs]
                    e = np.exp(sel - sel.max())
                    priors = (e / e.sum()).tolist()
                    # 根节点混入 Dirichlet 噪声（只在根，且只在此处新建孩子时施加一次）：
                    # 让每条边都有被访问的机会，同时搜索仍可凭 Q 把差动作压下去。
                    if (node is self.root and self.dirichlet_eps > 0
                            and self.rng is not None and len(priors) > 1):
                        eta = self.rng.dirichlet([self.dirichlet_total / len(priors)] * len(priors))
                        priors = [(1.0 - self.dirichlet_eps) * p + self.dirichlet_eps * float(et)
                                  for p, et in zip(priors, eta)]
                    for (i, a), child_b, p in zip(cands, kids, priors):
                        node.children[i] = MCTSNode(child_b, prior=p, action=a, zspec=self.zspec)
                for n in path:
                    n.N_VISIT += 1.0
                    n.WORTH += vals[j]
                done += 1

        counts = {i: ch.N_VISIT for i, ch in root.children.items()}
        actions = {i: ch.action for i, ch in root.children.items()}
        return counts, actions


def mcts_search(b, net, simulations, c_puct=None, batch_size=1, virtual_loss=1.0,
                zspec=DEFAULT_SPEC):
    """一次性搜索：从零建树、跑 `simulations` 次模拟、把树丢掉。

    `batch_size` 是**语义旋钮**（同批 k 条路径互相看不到彼此的回传值，靠**沿路径**的 virtual loss
    在每个分岔点避开彼此）；换批大小前必须重测下游胜率。`batch_size=1` 是逐位可复现的基准
    （标记加了立刻减回，下一次选择看到的还是 0）。
    要跨决策步复用树就用 `SearchTree`（自对弈那条路走它）。
    """
    return SearchTree(b, net, c_puct, batch_size, virtual_loss, zspec).run(b, simulations)


def mcts_action(net, b, simulations, c_puct=None, batch_size=1, zspec=DEFAULT_SPEC):
    """搜索后取访问数最大的动作（不下采样 —— 传统 UCT 的教训：π 太平坦）。"""
    counts, actions = mcts_search(b, net, simulations, c_puct, batch_size, zspec=zspec)
    # `key=counts.get` 运行期能用（键都来自 counts 本身），但类型上 `dict.get` 带
    # (key, default) 重载，会让 `max` 找不到匹配的重载。下标取值更直白。
    return actions[max(counts, key=lambda i: counts[i])]
