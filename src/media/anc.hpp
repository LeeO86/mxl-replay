#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace replay
{
// SMPTE ST 12-2 ATC inside an RFC 8331 / video/smpte291 grain.
// Same layout as mxl-test-player and mxl-decklink: 4096-byte grain, DID/SDID 0x60/0x60.
inline constexpr std::size_t kAncGrainSize = 4096;

enum class AtcKind
{
    Ltc,
    Vitc1,
    Vitc2
};

struct Timecode
{
    int hh = 0;
    int mm = 0;
    int ss = 0;
    int ff = 0;
    bool drop = false;
    int field = 0;

    [[nodiscard]] std::string format() const;
    bool operator==(Timecode const& other) const
    {
        return hh == other.hh && mm == other.mm && ss == other.ss && ff == other.ff && drop == other.drop && field == other.field;
    }
};

struct AncPacket
{
    Timecode tc;
    AtcKind kind = AtcKind::Ltc;
    int line = 10;
    bool interlaced = false;
    std::uint16_t sequence = 0;
};

[[nodiscard]] std::vector<std::uint8_t> encodeAncGrain(AncPacket const& packet);
bool decodeAncTimecode(std::uint8_t const* grain, std::size_t size, Timecode& out, AtcKind& kind);

// HH:MM:SS:FF or HH:MM:SS;FF from a TAI timestamp at the house rate.
[[nodiscard]] Timecode timecodeFromTai(std::uint64_t timestampNs, int rateNum, int rateDen, bool dropFrame);
} // namespace replay
