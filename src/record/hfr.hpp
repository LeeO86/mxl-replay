#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace replay
{
struct PhaseGrain
{
    int phase = 0;
    std::uint64_t grainTaiNs = 0;
    bool present = false;
    int payload = 0;
};

struct HfrFrame
{
    std::uint64_t timeNs = 0;
    int phase = 0;
    int payload = 0;
    bool repeated = false;
};

// Interleave N phase grains that share a house timestamp.
// Phase k is stored at grainTai + offset(k), default offset = k/N * housePeriod.
// A missing phase repeats the nearest present neighbour and is counted.
struct InterleaveResult
{
    std::vector<HfrFrame> frames;
    int missing = 0;
};

[[nodiscard]] InterleaveResult interleavePhases(std::vector<PhaseGrain> const& grains, int phases, std::uint64_t housePeriodNs);
} // namespace replay
