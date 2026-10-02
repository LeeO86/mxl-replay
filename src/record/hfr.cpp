#include "record/hfr.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace replay
{
InterleaveResult interleavePhases(std::vector<PhaseGrain> const& grains, int phases, std::uint64_t housePeriodNs)
{
    InterleaveResult result;
    if (phases < 1)
    {
        return result;
    }
    std::vector<PhaseGrain> slot(static_cast<std::size_t>(phases));
    for (int i = 0; i < phases; ++i)
    {
        slot[static_cast<std::size_t>(i)].phase = i;
        slot[static_cast<std::size_t>(i)].present = false;
    }
    for (auto const& grain : grains)
    {
        if (grain.phase < 0 || grain.phase >= phases)
        {
            continue;
        }
        slot[static_cast<std::size_t>(grain.phase)] = grain;
        slot[static_cast<std::size_t>(grain.phase)].present = grain.present;
    }
    std::uint64_t base = 0;
    bool haveBase = false;
    for (auto const& grain : slot)
    {
        if (grain.present)
        {
            base = grain.grainTaiNs;
            haveBase = true;
            break;
        }
    }
    if (!haveBase)
    {
        result.missing = phases;
        return result;
    }
    for (int k = 0; k < phases; ++k)
    {
        auto& grain = slot[static_cast<std::size_t>(k)];
        std::uint64_t const offset = static_cast<std::uint64_t>((static_cast<__int128>(housePeriodNs) * k) / phases);
        HfrFrame frame;
        frame.phase = k;
        frame.timeNs = base + offset;
        if (grain.present)
        {
            frame.payload = grain.payload;
            frame.repeated = false;
            if (grain.grainTaiNs != 0)
            {
                frame.timeNs = grain.grainTaiNs + offset;
            }
        }
        else
        {
            int best = -1;
            int bestDist = std::numeric_limits<int>::max();
            for (int j = 0; j < phases; ++j)
            {
                if (!slot[static_cast<std::size_t>(j)].present)
                {
                    continue;
                }
                int const dist = std::abs(j - k);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    best = j;
                }
            }
            if (best >= 0)
            {
                frame.payload = slot[static_cast<std::size_t>(best)].payload;
                frame.phase = best;
            }
            frame.repeated = true;
            ++result.missing;
        }
        result.frames.push_back(frame);
    }
    std::sort(result.frames.begin(), result.frames.end(), [](HfrFrame const& a, HfrFrame const& b) { return a.timeNs < b.timeNs; });
    return result;
}
} // namespace replay
