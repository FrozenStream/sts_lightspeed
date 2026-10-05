r"""池化不变量检查：确认「逐卡嵌入 → 取 mean → 进主网络」真的成立。

## 为什么要它

三个牌堆都**只给集合**之后，**整堆牌的表示就是那一个池化向量**。口径必须是：

    pooled = Σ_{非空槽} e_i / n        （n = 真实张数，空槽不进「和」也不进「分母」）

两个容易漂开的地方：分母若用**槽数 S**，表示里就混进了「可见张数」这种冗余信息（`counts`
块里本就有），且量级由**传输格式常数 S** 决定 —— S 不该有任何语义；空槽行（行 0 是**可学**的
「空槽」表示，见 `aznet`）必须靠**掩码**从「和」里排掉，不能指望它自己是 0。

## 断言（每条对着一个明确的机理）

1. **是真正的均值**：某堆的池化向量 == 该堆**非空槽**嵌入的算术平均。
2. **置换不变**：把真实牌在槽内**打乱位置**，池化结果必须逐位不变 ⇒ 牌堆块给的是
   **容器序**也不构成序泄漏（网络看不见序）。

## 「大小无所谓」成立到什么程度

池化**结构上与槽数无关**；槽数唯一还能影响结果的地方只剩**截断**
（量那个的 `_chk_trunc.py` 已于 2026-09-30 退役，git 历史里有）。

用法：`python _chk_pool.py [遭遇名=THREE_SENTRIES] [层=12] [seed=1000]`

⚠️ 场景表原来在 `_gen_eval.ALL_TIERS`（`tier` = 怪池 × 卡组规格）⇒ `_gen_eval.py` 于 2026-09-30
删除后改回 `collect.make_stage_battle(seed, enc, floor)` 的 `(遭遇, 层)` 口径，
资源（牌组/遗物/药水/hp）由 `scene.shape_floor` 发 —— 与 `_chk_mask.py`/`_chk_actions.py` 同场景。
"""
import inspect
import random
import sys

import slaythespire as sts
import torch

from train import net_cfg
from aznet import AZNet, HAND_CARD_IDX, PILE_CARD_IDX, teacher_action
from collect import make_stage_battle

FAIL = []
STEPS = 6


def check(ok, msg):
    print(("  [ok]   " if ok else "  [FAIL] ") + msg)
    if not ok:
        FAIL.append(msg)


def pooled_of(net, card, pi):
    """从 `card_feats` 的输出里切出第 pi 个牌堆的池化段（宽度 E）。

    `card_feats` 的拼接顺序：手牌块（10E）→ 4 个牌堆池化段（各 E）→ 候选块。
    （4 块 = 三个牌堆 + 总牌组；每块只有**一个**池化向量 —— 不给堆序。）
    """
    e = net.card_embed.embedding_dim
    start = len(HAND_CARD_IDX) * e + pi * e
    return card[:, start:start + e][0]


def sample():
    """取一个**真实战斗**状态（默认 Act 1 精英 `THREE_SENTRIES@12`），并推进 `STEPS` 步。"""
    enc_name = sys.argv[1] if len(sys.argv) > 1 else "THREE_SENTRIES"
    floor = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
    _gc, b, _enc = make_stage_battle(seed, getattr(sts.MonsterEncounter, enc_name), floor)
    for _ in range(STEPS):                       # 推进几步，别停在"起手弃牌堆为空"的退化态
        if b.is_battle_over():
            break
        got = teacher_action(b, 300, random.Random(0))
        if got is None:
            break
        got[3].execute(b)
    return f"{enc_name}@{floor}", seed, torch.as_tensor(b.get_snapshot_np(),
                                                        dtype=torch.float32).unsqueeze(0)


def main():
    torch.manual_seed(0)
    scene, seed, obs = sample()
    # 建网配置：本守卫只需要**任意**一个能跑的真实网（比的是池化恒等式）⇒ 用模块默认那一份。
    cfg = net_cfg()
    # net_cfg() 的键名是手写的：与 AZNet 形参漂移就当场拦住。
    params = set(inspect.signature(AZNet.__init__).parameters) - {"self"}
    if set(cfg) != params:
        raise SystemExit(f"❌ net_cfg() 与 AZNet 形参不一致：{sorted(set(cfg) ^ params)}")
    print(f"场景 {scene} / seed {seed} / 推进 {STEPS} 步后取样")
    print(f"CONFIG = {cfg}\n")

    net = AZNet(**cfg)
    net.eval()
    with torch.no_grad():
        card = net.card_feats(obs)
        for pi, idxs in enumerate(PILE_CARD_IDX):
            ids = obs[0, idxs].long()
            real = ids[ids > 0]
            if len(real) == 0:
                print(f"  堆{pi}: 空，跳过")
                continue
            n, s = len(real), len(idxs)
            ref = net.card_embed(real).mean(dim=0)          # 真正的均值
            got = pooled_of(net, card, pi)
            check(torch.allclose(got, ref, atol=1e-6),
                  f"堆{pi}（{n}/{s} 张）池化 == 真均值  "
                  f"max|Δ|={float((got - ref).abs().max()):.2e}")

            # 置换不变：把真实牌放到**随机槽位**（先整堆清零）
            slots = torch.tensor(idxs, dtype=torch.long)
            perm = obs.clone()
            perm[0, idxs] = 0
            perm[0, slots[torch.randperm(s)[:n]]] = real.float()
            got2 = pooled_of(net, net.card_feats(perm), pi)
            check(torch.allclose(got, got2, atol=1e-6),
                  f"堆{pi} 置换不变  max|Δ|={float((got - got2).abs().max()):.2e}")

    print("FAILED:" if FAIL else "全部通过", *FAIL, sep="\n  " if FAIL else " ")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
