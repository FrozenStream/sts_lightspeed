#include "action.h"

#include <stdexcept>
#include <utility>

#include "slaythespire.h"

#include "sim/search/BattleScumSearcher2.h"

namespace sts::collect
{

    std::vector<search::Action> enumerateActions(const BattleContext &bc)
    {
        search::BattleScumSearcher2 searcher(bc);
        searcher.enumerateActionsForNode(searcher.root, bc);
        std::vector<search::Action> out;
        out.reserve(searcher.root.edges.size());
        for (const auto &e : searcher.root.edges)
        {
            out.push_back(e.action);
        }
        return out;
    }

    std::vector<std::pair<search::Action, int>> legalActionsWithIdx(const BattleContext &bc)
    {
        std::vector<std::pair<search::Action, int>> out;
        const auto acts = enumerateActions(bc);
        out.reserve(acts.size());
        for (const auto &a : acts)
        {
            out.emplace_back(a, actionToIdx(bc, a));
        }
        return out;
    }

    int actionToIdx(const BattleContext &bc, const search::Action &a)
    {
        switch (a.getActionType())
        {
        case search::ActionType::CARD:
        {
            const int hand = a.getSourceIdx();
            // monster 0..4 -> 1..5
            const int t = bc.cards.hand[hand].requiresTarget() ? a.getTargetIdx() + 1 : 0;
            return hand * sts::py::handStride + t;
        }
        case search::ActionType::POTION:
        {
            const int slot = a.getSourceIdx();
            const auto p = bc.potions[slot];
            const int target = a.getTargetIdx();
            int pt;
            // 丢弃。唯一的正身是妖精药水 —— 引擎给它编的是 `idx2 = -1`
            // （`enumeratePotionActions`），进 13 位目标字段后回读成 0x1FFF。
            if (target > sts::collect::maxMonsters)
                pt = sts::py::potionStride - 1;
            else if (potionRequiresTarget(p))
                pt = target + 1; // monster 0..4 -> 1..5
            else
                pt = 0; // no target needed
            return sts::py::potionBase + slot * sts::py::potionStride + pt;
        }
        case search::ActionType::SINGLE_CARD_SELECT:
            return a.getSelectIdx();
        case search::ActionType::END_TURN:
            return sts::py::endTurnIdx;
        default:
            throw std::runtime_error("invalid action type");
        }
    }

    std::pair<std::vector<std::pair<int, search::Action>>,
              std::vector<std::unique_ptr<BattleContext>>>
    expandChildren(const BattleContext &bc)
    {
        const auto actions = enumerateActions(bc);
        std::pair<std::vector<std::pair<int, search::Action>>,
                  std::vector<std::unique_ptr<BattleContext>>>
            out;
        out.first.reserve(actions.size());
        out.second.reserve(actions.size());
        for (const auto &a : actions)
        {
            // 克隆 + 执行，与 Python 侧原来的 `child = node.b.clone(); a.execute(child)` 同序
            auto child = std::make_unique<BattleContext>(bc);
            a.execute(*child);
            out.first.emplace_back(actionToIdx(bc, a), a);
            out.second.push_back(std::move(child));
        }
        return out;
    }

    search::Action idxToAction(const BattleContext &bc, int idx)
    {
        if (bc.inputState == InputState::CARD_SELECT)
        {
            if (idx < 0)
            {
                return {};
            }
            return search::Action(search::ActionType::SINGLE_CARD_SELECT, idx);
        }
        if (idx == sts::py::endTurnIdx)
        {
            return search::Action(search::ActionType::END_TURN);
        }
        if (idx < 0 || idx >= sts::py::unifiedActionCount)
        {
            return {};
        }
        if (idx < sts::py::potionBase)
        {
            const int hand = idx / sts::py::handStride;
            const int t = idx % sts::py::handStride;
            if (hand >= bc.cards.cardsInHand)
            {
                return {};
            }
            const auto &c = bc.cards.hand[hand];
            if (c.requiresTarget())
            {
                if (t == 0)
                {
                    return {};
                } // must target a monster
                return search::Action(search::ActionType::CARD, hand, t - 1);
            }
            return search::Action(search::ActionType::CARD, hand);
        }
        const int pidx = idx - sts::py::potionBase;
        const int slot = pidx / sts::py::potionStride;
        const int pt = pidx % sts::py::potionStride;
        if (slot >= bc.potionCapacity)
        {
            return {};
        }
        const auto p = bc.potions[slot];
        if (p == Potion::INVALID || p == Potion::EMPTY_POTION_SLOT)
        {
            return {};
        }
        if (pt == sts::py::potionStride - 1)
        {
            return search::Action(search::ActionType::POTION, slot, 99);
        } // discard (>5)
        if (potionRequiresTarget(p))
        {
            if (pt == 0)
            {
                return {};
            }
            return search::Action(search::ActionType::POTION, slot, pt - 1);
        }
        return search::Action(search::ActionType::POTION, slot);
    }

} // namespace sts::collect