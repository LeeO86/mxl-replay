#pragma once

#include <cstdint>
#include <string>

namespace replay
{
// Same 128-bit rounding as dmf-mxl/mxl IndexConversion.hpp at 218ddaa.
std::uint64_t timestampToIndex(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t timestampNs);
std::uint64_t indexToTimestamp(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t index);
std::uint64_t taiNowNs();
bool parseRateToken(std::string const& text, int& numerator, int& denominator);
std::string formatTimecode(std::uint64_t timestampNs, int numerator, int denominator, bool dropFrame = false);

// 48 kHz samples belonging to video frame `index` (0-based). 50 fps is 960.
// 30000/1001 alternates around 1601.6. 60000/1001 alternates around 800.8.
int audioSamplesForFrame(std::uint64_t index, int rateNumerator, int rateDenominator, int sampleRate = 48000);
std::uint64_t audioSamplesUntil(std::uint64_t index, int rateNumerator, int rateDenominator, int sampleRate = 48000);

std::int64_t framePeriodNs(int rateNumerator, int rateDenominator);
} // namespace replay
