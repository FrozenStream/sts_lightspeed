"""推理前向的守卫：`inference_net` 就是 `net` 本身。

以前这一项守的是「ONNX 导出与 torch 逐元素一致」（那时推理走 ORT）。2026-10-03 换回 torch
（探索阶段以语义简明优先，理由见 `aznet.inference_net`）⇒ 现在要守的是四件容易静默坏的事：

  ① `inference_net(net, t)` **返回的就是 net** —— 否则「换权重零成本」这个前提就没了；
  ② 前向**确定性**（同输入两次逐位相同；`eval()` 没切会破坏它）；
  ③ **numpy 与 torch 张量两种输入都收**且结果一致 —— `SearchTree.run` 的 obs 是 numpy
     （`sts.batch_snapshot` 产出），而 `net_action` 那条路给的是 torch 张量；
  ④ 线程数确实落到 `torch.set_num_threads`（32 个 worker 不设就是 32 倍超订）。
"""
import sys

import torch
import slaythespire as sts

from aznet import AZNet, inference_net
from collect import make_stage_battle


def main() -> int:
    torch.manual_seed(0)
    net = AZNet()
    net.eval()
    bad: list[str] = []

    r = inference_net(net, intra_threads=1)
    if r is not net:
        bad.append(f"inference_net 返回的不是 net 本身（{type(r).__name__}）")
    if torch.get_num_threads() != 1:
        bad.append(f"线程数没落到 torch（get_num_threads={torch.get_num_threads()}）")

    _, b, _ = make_stage_battle(0, sts.MonsterEncounter.CULTIST, 4)
    obs = sts.batch_snapshot([b, b.clone()])
    a = r(obs)
    c = r(torch.as_tensor(obs))
    for i, (x, y) in enumerate(zip(a, c)):
        if not torch.equal(x, y):
            bad.append(f"numpy 与 torch 输入的头 {i} 不一致（最大差 "
                       f"{float((x - y).abs().max()):.3g}）")
    d = r(obs)
    if not all(torch.equal(x, y) for x, y in zip(a, d)):
        bad.append("同输入两次前向不一致（eval 没切？）")

    print(f"  {'❌' if bad else '✅'} 推理前向：`inference_net` 就是 `net` 本身 / 确定 / "
          f"numpy 与 torch 输入一致 / 线程数落实（异常 {len(bad)} 处：{bad[:2]}）")
    return 2 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
