#ifndef STS_LIGHTSPEED_ZVALUE_H
#define STS_LIGHTSPEED_ZVALUE_H

#include <string>

#include "combat/BattleContext.h"

namespace sts::collect
{

    // `spec`：`hp` | `hp_turn` | `enginenorm`；不认识的当场抛（不静默兜底）。
    double terminalValue(const BattleContext &bc, const std::string &spec);

} // namespace sts::collect

#endif // STS_LIGHTSPEED_ZVALUE_H