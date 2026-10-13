#include "zvalue.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "sim/search/BattleScumSearcher2.h" // evaluateEndState（`enginenorm` 口径）

namespace sts::collect
{

    double terminalValue(const BattleContext &bc, const std::string &spec)
    {
        const bool win = bc.outcome == Outcome::PLAYER_VICTORY;
        const double max_hp = std::max(1.0, static_cast<double>(bc.player.maxHp));
        const double ratio = static_cast<double>(bc.player.curHp) / max_hp;
        if (spec == "hp")
        {
            return win ? ratio : -1.0;
        }
        if (spec == "hp_turn")
        {
            return win ? ratio - 0.000125 * static_cast<double>(bc.turn) : -1.0;
        }
        if (spec == "enginenorm")
        {
            const double escore = search::BattleScumSearcher2::evaluateEndState(bc);
            return escore / (100.0 * max_hp);
        }
        throw std::runtime_error("不认识的值口径 " + spec + "（只有 hp / hp_turn / enginenorm）");
    }

} // namespace sts::collect