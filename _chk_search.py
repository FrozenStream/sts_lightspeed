"""`SearchTree` 的守卫：跨步复用必须**逐位可靠**。

复用把「上一步的那个孩子」直接当新根继续用 ⇒ 四件事必须成立，否则搜索会在无声中跑偏：

  ① **状态对齐** —— `advance` 之后树根的状态必须与真实状态**逐位相同**（不然统计挂在别的局面上，
     而 π 看起来完全正常）；
  ② **nv 不泄漏** —— `run` 结束全树 `nv == 0`（否则跨步累积，把 PUCT 的 q 项永久压低）；
  ③ **首步等价** —— 从零建树时 `SearchTree.run` 与 `mcts_search` 给出**同一份** counts；
  ④ **丢掉的子树立刻回收** —— 树里不能有循环引用（`MCTSNode` 曾回指父节点）：`advance` 每步丢掉
     上千个节点，靠分代 GC 回收会让驻留内存涨到活节点的 20 倍。

另测两条**必须报错**的路径：`advance` 一个树里没有的 idx、以及状态链断裂（走了别的动作）——
这两处静默兜底 = 静默换搜索树。

⚠️ 检查**各自成函数**：`gc.get_objects()` 统计的是「进程里所有 `MCTSNode`」，若同一个函数里还留着
别的树（比如越界/错位测试用的那两棵），它们会被算进「没回收」⇒ 假阳性。
"""
import gc
import sys

import numpy as np
import torch
import slaythespire as sts

from aznet import AZNet, inference_net
from azsearch import MCTSNode, SearchTree, mcts_search
from collect import make_stage_battle

SIMS, STEPS = 120, 5
CASES = (("CULTIST", 4), ("MAW", 45), ("THREE_SENTRIES", 12))


def _reachable(root) -> set[int]:
    seen, stack = {id(root)}, [root]
    while stack:
        nd = stack.pop()
        for ch in nd.children.values():
            if id(ch) not in seen:
                seen.add(id(ch))
                stack.append(ch)
    return seen


def _nv_bad(root) -> list[float]:
    bad, stack = [], [root]
    while stack:
        nd = stack.pop()
        if nd.nv != 0.0:
            bad.append(nd.nv)
        stack.extend(nd.children.values())
    return bad


def _reuse_chain(name: str, floor: int, ort) -> list[str]:
    """① ② ③ ④：一条连续的决策链走 `STEPS` 步。"""
    bad: list[str] = []
    enc = getattr(sts.MonsterEncounter, name)
    _, b, _ = make_stage_battle(0, enc, floor)
    tree = SearchTree(b, ort)
    for step in range(STEPS):
        if b.is_battle_over():
            break
        counts, actions = tree.run(b, SIMS)
        if step == 0:
            fresh, actions_f = mcts_search(b, ort, SIMS)
            if counts != fresh or set(actions) != set(actions_f):
                bad.append(f"{name} 首步与从零建树不一致")
        idx = max(counts, key=lambda i: counts[i])
        tree.advance(idx)
        actions[idx].execute(b)
        # ① advance 之后树根必须**就是**真实走完这步的状态
        if not np.array_equal(tree.root.b.get_snapshot_np(), b.get_snapshot_np()):
            bad.append(f"{name} 步{step}：advance 后树根与真实状态不一致")
        # ② nv 必须清零
        leak = _nv_bad(tree.root)
        if leak:
            bad.append(f"{name} 步{step}：nv 泄漏 {leak[:3]}")
    # ④ 丢掉的子树必须**已经被引用计数回收**（不等分代 GC）
    live = _reachable(tree.root)
    stray = [o for o in gc.get_objects() if isinstance(o, MCTSNode) and id(o) not in live]
    if stray:
        bad.append(f"{name}：{len(stray)} 个已丢弃的 MCTSNode 未被引用计数回收")
    return bad


def _must_raise(name: str, floor: int, ort) -> list[str]:
    """越界 idx 与状态链断裂都必须报错（不静默重建）。"""
    bad: list[str] = []
    enc = getattr(sts.MonsterEncounter, name)

    _, b2, _ = make_stage_battle(0, enc, floor)
    t2 = SearchTree(b2, ort)
    t2.run(b2, 4)
    try:
        t2.advance(10 ** 6)
        bad.append(f"{name}：越界 idx 没有报错")
    except KeyError:
        pass

    _, b3, _ = make_stage_battle(0, enc, floor)
    t3 = SearchTree(b3, ort)
    c3, a3 = t3.run(b3, SIMS)
    i_same = max(c3, key=lambda i: c3[i])
    i_other = next(i for i in c3 if i != i_same)
    t3.advance(i_same)
    a3[i_other].execute(b3)
    try:
        t3.run(b3, SIMS)
        bad.append(f"{name}：状态链断裂没有报错")
    except RuntimeError:
        pass
    return bad


def _torch_path(name: str, floor: int) -> list[str]:
    """裸 `AZNet`（torch 路径）也必须能跑 MCTS。

    `SearchTree.run` 的 obs 是 `sts.batch_snapshot` 产出的 **numpy**；只验 ORT 会漏掉「torch 网
    接不住 numpy」这条（真漏过一次：`_gate_select_region` 里 `is_sel.unsqueeze` 当场 `AttributeError`）。
    """
    enc = getattr(sts.MonsterEncounter, name)
    _, b, _ = make_stage_battle(0, enc, floor)
    net = AZNet()
    net.eval()
    try:
        counts, _ = SearchTree(b, net).run(b, 8)
    except Exception as e:                          # noqa: BLE001
        return [f"{name}：裸 torch 网跑 MCTS 失败 {type(e).__name__}: {str(e)[:60]}"]
    return [] if counts else [f"{name}：裸 torch 网没给出 counts"]


def main() -> int:
    torch.manual_seed(0)
    torch.set_num_threads(1)
    net = AZNet()
    net.eval()
    ort = inference_net(net, intra_threads=1)

    bad: list[str] = []
    for name, floor in CASES:
        bad += _reuse_chain(name, floor, ort)
    for name, floor in CASES:
        bad += _must_raise(name, floor, ort)
    bad += _torch_path(CASES[0][0], CASES[0][1])

    print(f"  {'❌' if bad else '✅'} `SearchTree` 复用：状态逐位对齐 / nv 不泄漏 / 首步与从零一致"
          f" / 丢弃的子树立即回收 / 裸 torch 网可用（{len(CASES)} 个遭遇 × {STEPS} 步；"
          f"异常 {len(bad)} 处：{bad[:3]}）")
    return 2 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
