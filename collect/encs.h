#ifndef STS_LIGHTSPEED_ENCS_H
#define STS_LIGHTSPEED_ENCS_H

#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "constants/MonsterEncounters.h"

namespace sts::collect
{
    struct EncJob
    {
        std::uint64_t seed = 0;
        MonsterEncounter enc = MonsterEncounter::INVALID;
        MonsterEncounterPool::PoolStep step = MonsterEncounterPool::PoolStep::INVALID;
    };

    inline std::vector<EncJob> encJobs(std::uint32_t n_per, std::uint64_t start_seed = 0)
    {
        const auto &all = MonsterEncounterPool::allEnemies;
        std::vector<EncJob> jobs;
        jobs.reserve(std::size(all) * n_per);
        for (auto enc : all)
        {
            const MonsterEncounterPool::PoolStep step = MonsterEncounterPool::stepOf(enc);
            for (std::uint64_t i = 0; i < n_per; ++i)
                jobs.push_back(EncJob{start_seed + i, enc, step});
        }
        return jobs;
    }
} // namespace sts::collect

#endif // STS_LIGHTSPEED_ENCS_H
