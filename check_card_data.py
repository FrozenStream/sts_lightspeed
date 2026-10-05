#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
检查 include/constants/Cards.h 与 Card_data.csv 中卡牌数据的差异。

比较维度:
  - 卡牌集合（存在性）
  - 颜色 / 类型 / 稀有度
  - 费用（基础费用 / 升级后费用）

匹配规则:
  - 以 Cards.h 的 cardStringIds 作为唯一匹配键（忽略 cardNames）;
  - CSV 的 card_id 列归一化（去空格/标点、小写）后与 cardStringIds 匹配;
  - 匹配不上的 CSV 行会列在「CSV 中有、Cards.h 中没有」。

费用说明:
  - C++ 端 getEnergyCost() 返回 -1 表示 X 费用卡, -2 表示状态卡, -3 表示诅咒卡;
  - CSV 端诅咒卡/状态卡都用 -2 表示「不可打出」;
  - 脚本将 -2 / -3 视为同等的「不可打出」哨兵值, 不再报为差异。

用法:  python check_card_data.py [Cards.h路径] [Card_data.csv路径]
"""
import csv
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
HEADER_PATH = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "include" / "constants" / "Cards.h"
CSV_PATH = Path(sys.argv[2]) if len(sys.argv) > 2 else REPO / "Card_data.csv"

COLOR_MAP = {"红色": "RED", "绿色": "GREEN", "蓝色": "BLUE",
             "紫色": "PURPLE", "无色": "COLORLESS", "诅咒": "CURSE"}
TYPE_MAP = {"攻击": "ATTACK", "技能": "SKILL", "能力": "POWER",
            "诅咒": "CURSE", "状态": "STATUS"}
RARITY_MAP = {"普通": "COMMON", "罕见": "UNCOMMON", "稀有": "RARE",
              "初始": "BASIC", "特殊": "SPECIAL", "诅咒": "CURSE"}

# -2 / -3 都表示「不可打出」（状态卡、诅咒卡）
UNPLAYABLE = {-2, -3}


def norm(s: str) -> str:
    """归一化: 转小写并去掉所有非字母数字字符, 用于模糊匹配卡名。"""
    return re.sub(r"[^a-z0-9]", "", s.lower())


# ---------------------------------------------------------------- 解析 Cards.h
def parse_enum_cardid(header: str):
    """解析 enum class CardId, 返回按顺序排列的枚举名列表（下标即枚举值）。"""
    m = re.search(r"enum\s+class\s+CardId\s*:\s*std::uint16_t\s*\{(.*?)\};", header, re.S)
    if not m:
        raise SystemExit("错误: 在 Cards.h 中找不到 enum class CardId")
    names = re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*(?:=|,)?", m.group(1), re.M)
    # 只保留看起来像标识符的行, 过滤掉注释/噪音
    names = [n for n in names if re.fullmatch(r"[A-Z][A-Z0-9_]*", n)]
    return names


def parse_string_array(header: str, name: str):
    """解析 static constexpr const char* name[] = {...}; 返回字符串列表。"""
    m = re.search(
        r"static\s+constexpr\s+const\s+char\s*\*\s*" + name + r"\s*\[\]\s*=\s*\{(.*?)\}\s*;",
        header, re.S)
    if not m:
        raise SystemExit(f"错误: 在 Cards.h 中找不到数组 {name}")
    return re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))


def parse_enum_array(header: str, name: str, enum_type: str):
    """解析 static constexpr EnumType name[] = {EnumType::X, ...}; 返回枚举值列表。"""
    m = re.search(
        r"static\s+constexpr\s+" + enum_type + r"\s+" + name + r"\s*\[\]\s*=\s*\{(.*?)\}\s*;",
        header, re.S)
    if not m:
        raise SystemExit(f"错误: 在 Cards.h 中找不到数组 {name}")
    vals = re.findall(r"\b" + enum_type + r"::(\w+)", m.group(1))
    # 数组中不允许出现非枚举值, 若长度对不上则报错
    return vals


def parse_get_energy_cost(header: str):
    """
    解析 getEnergyCost() 的 switch, 返回 {CardId枚举名: (非升级费用, 升级费用)}。
    未命中 switch 的卡默认费用为 1。
    """
    # 先定位 getEnergyCost 函数, 再取其内部的 switch (避免匹配到前面的 isCardEthereal 等函数)
    pos = header.find("getEnergyCost")
    if pos < 0:
        raise SystemExit("错误: 在 Cards.h 中找不到 getEnergyCost")
    m = re.search(r"switch\s*\(id\)\s*\{(.*?)default\s*:", header[pos:], re.S)
    if not m:
        raise SystemExit("错误: 在 Cards.h 中找不到 getEnergyCost 的 switch")
    body = m.group(1)
    costs = {}
    current_cases = []
    for mm in re.finditer(r"case\s+CardId::(\w+)\s*:|return\s+([^;]+);", body):
        if mm.group(1):          # case 标签
            current_cases.append(mm.group(1))
        else:                    # return 表达式
            expr = mm.group(2).strip()
            m2 = re.match(r"upgraded\s*\?\s*(-?\d+)\s*:\s*(-?\d+)", expr)
            if m2:               # upgraded ? A : B  => 升级=A, 非升级=B
                non, up = int(m2.group(2)), int(m2.group(1))
            else:
                non = up = int(expr)
            for c in current_cases:
                costs[c] = (non, up)
            current_cases = []
    default = (1, 1)
    return costs, default


# ---------------------------------------------------------------- 解析 CSV
def parse_csv(path: Path):
    rows = []
    with open(path, newline="", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


# ---------------------------------------------------------------- 主逻辑
def main():
    header = HEADER_PATH.read_text(encoding="utf-8")
    enum_names = parse_enum_cardid(header)
    n_cards = len(enum_names) - 1          # 去掉 INVALID
    print(f"[Cards.h] 卡牌数: {n_cards} (INVALID 除外, 共 {len(enum_names)} 个枚举值)")

    # 按 CardId 顺序读取各属性数组
    card_str_ids = parse_string_array(header, "cardStringIds")
    card_colors = parse_enum_array(header, "cardColors", "CardColor")
    card_rarities = parse_enum_array(header, "cardRarities", "CardRarity")
    card_types = parse_enum_array(header, "cardTypes", "CardType")
    costs, default_cost = parse_get_energy_cost(header)

    for arr, label in [(card_str_ids, "cardStringIds"),
                       (card_colors, "cardColors"), (card_rarities, "cardRarities"),
                       (card_types, "cardTypes")]:
        if len(arr) != len(enum_names):
            print(f"  [警告] {label} 长度 {len(arr)} != 枚举数 {len(enum_names)}")

    # 归一化名 -> CardId 下标 (只以 cardStringIds 为准, 忽略 cardNames)
    idx_by_norm = {norm(s): i for i, s in enumerate(card_str_ids)}

    # 读取 CSV
    rows = parse_csv(CSV_PATH)
    print(f"[Card_data.csv] 数据行数: {len(rows)}")

    matched, not_found = [], []
    for r in rows:
        key = norm(r["card_id"]) or norm(r["name_en"])
        i = idx_by_norm.get(key)
        if i is None:
            not_found.append(r)
        else:
            matched.append((i, r))

    # ---------------- 集合差异 ----------------
    csv_found_idx = {i for i, _ in matched}
    hdr_only = [enum_names[i] for i in range(1, len(enum_names)) if i not in csv_found_idx]
    print("\n===== 集合差异 =====")
    print(f"  CSV 中有、Cards.h 中没有: {len(not_found)} 张")
    for r in not_found:
        print(f"    - [{r['page']}] card_id={r['card_id']!r} name_en={r['name_en']!r}")
    print(f"  Cards.h 中有、CSV 中没有: {len(hdr_only)} 张")
    for name in hdr_only:
        print(f"    - {name}")

    # ---------------- 属性差异 ----------------
    mism = {"color": [], "type": [], "rarity": [], "cost": [], "cost_up": []}
    for i, r in matched:
        en = enum_names[i]
        csv_color = COLOR_MAP.get(r["color"], r["color"])
        csv_type = TYPE_MAP.get(r["type"], r["type"])
        csv_rarity = RARITY_MAP.get(r["rarity"], r["rarity"])
        if csv_color != card_colors[i]:
            mism["color"].append((en, r, card_colors[i], csv_color))
        if csv_type != card_types[i]:
            mism["type"].append((en, r, card_types[i], csv_type))
        if csv_rarity != card_rarities[i]:
            mism["rarity"].append((en, r, card_rarities[i], csv_rarity))

        # 费用
        non_up, up = costs.get(en, default_cost)
        try:
            csv_cost = int(r["cost"])
            csv_up = int(r["upgrade_cost"])
        except ValueError:
            csv_cost = csv_up = None

        if csv_cost is not None and not cost_equal(non_up, csv_cost):
            mism["cost"].append((en, r, non_up, csv_cost))
        if csv_up is not None and not cost_equal(up, csv_up):
            mism["cost_up"].append((en, r, up, csv_up))

    def show(title, items, fmt):
        print(f"\n===== {title}: {len(items)} 处 =====")
        for it in items:
            print("    " + fmt(it))

    show("颜色不一致", mism["color"],
         lambda t: f"[{t[0]}] 中文={t[1]['page']}  Cards.h={t[2]}  CSV={t[3]}")
    show("类型不一致", mism["type"],
         lambda t: f"[{t[0]}] 中文={t[1]['page']}  Cards.h={t[2]}  CSV={t[3]}")
    show("稀有度不一致", mism["rarity"],
         lambda t: f"[{t[0]}] 中文={t[1]['page']}  Cards.h={t[2]}  CSV={t[3]}")
    show("基础费用不一致 (Cards.h vs CSV)", mism["cost"],
         lambda t: f"[{t[0]}] 中文={t[1]['page']}  Cards.h={t[2]}  CSV={t[3]}")
    show("升级后费用不一致 (Cards.h vs CSV)", mism["cost_up"],
         lambda t: f"[{t[0]}] 中文={t[1]['page']}  Cards.h={t[2]}  CSV={t[3]}")

    total = sum(len(v) for v in mism.values()) + len(not_found) + len(hdr_only)
    print(f"\n===== 汇总 ===== 共 {total} 处差异 (已匹配 {len(matched)} 张)")


def cost_equal(a, b):
    """费用相等性: -2 / -3 都表示「不可打出」(状态/诅咒), 视为相等。"""
    if a == b:
        return True
    if a in UNPLAYABLE and b in UNPLAYABLE:
        return True
    return False


if __name__ == "__main__":
    main()
