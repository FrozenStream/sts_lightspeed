#include "collect.h"

#include "snapshot.h"

namespace sts::collect
{
    // 两个采集器共用的终局读数；列序与 `zspec.Terminal` 一致。
    Term readTerm(const BattleContext &b)
    {
        Term t;
        t.is_win = b.outcome == Outcome::PLAYER_VICTORY;
        t.cur_hp = b.player.curHp;
        t.max_hp = b.player.maxHp;
        t.turn = b.turn;
        for (int i = 0; i < b.monsters.monsterCount; ++i)
        {
            t.enemy_cur_hp += b.monsters.arr[i].curHp;
            t.enemy_max_hp += b.monsters.arr[i].maxHp;
        }
        return t;
    }

    std::array<float, Snap::snapshotLen> snapshotObs(const BattleContext &b)
    {
        return snapshotOf(b);
    }
} // namespace sts::collect
