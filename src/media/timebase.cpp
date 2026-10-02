#include "media/timebase.hpp"

#include <cstdio>
#include <ctime>

namespace replay
{
std::uint64_t timestampToIndex(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t timestampNs)
{
    if (rateNumerator == 0 || rateDenominator == 0)
    {
        return UINT64_MAX;
    }
    auto const num = static_cast<__int128>(timestampNs) * static_cast<__int128>(rateNumerator) +
                     static_cast<__int128>(500000000) * static_cast<__int128>(rateDenominator);
    auto const den = static_cast<__int128>(1000000000) * static_cast<__int128>(rateDenominator);
    return static_cast<std::uint64_t>(num / den);
}

std::uint64_t indexToTimestamp(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t index)
{
    if (rateNumerator == 0 || rateDenominator == 0)
    {
        return UINT64_MAX;
    }
    auto const num = static_cast<__int128>(index) * static_cast<__int128>(rateDenominator) * static_cast<__int128>(1000000000) +
                     static_cast<__int128>(rateNumerator) / 2;
    return static_cast<std::uint64_t>(num / static_cast<__int128>(rateNumerator));
}

std::uint64_t taiNowNs()
{
    timespec ts{};
#if defined(CLOCK_TAI)
    if (clock_gettime(CLOCK_TAI, &ts) != 0)
#endif
    {
        clock_gettime(CLOCK_REALTIME, &ts);
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

bool parseRateToken(std::string const& text, int& numerator, int& denominator)
{
    numerator = 0;
    denominator = 1;
    if (text.empty())
    {
        return false;
    }
    if (text == "2398")
    {
        numerator = 24000;
        denominator = 1001;
        return true;
    }
    if (text == "2997")
    {
        numerator = 30000;
        denominator = 1001;
        return true;
    }
    if (text == "5994")
    {
        numerator = 60000;
        denominator = 1001;
        return true;
    }
    auto const slash = text.find('/');
    try
    {
        if (slash == std::string::npos)
        {
            numerator = std::stoi(text);
            denominator = 1;
        }
        else
        {
            numerator = std::stoi(text.substr(0, slash));
            denominator = std::stoi(text.substr(slash + 1));
        }
    }
    catch (...)
    {
        return false;
    }
    return numerator > 0 && denominator > 0;
}

std::string formatTimecode(std::uint64_t timestampNs, int numerator, int denominator, bool dropFrame)
{
    if (numerator <= 0 || denominator <= 0)
    {
        return {};
    }
    std::uint64_t const sec = timestampNs / 1000000000ull;
    std::uint64_t const secNs = sec * 1000000000ull;
    auto const index = timestampToIndex(numerator, denominator, timestampNs);
    auto const secIndex = timestampToIndex(numerator, denominator, secNs);
    int frame = index >= secIndex ? static_cast<int>(index - secIndex) : 0;
    int const nominal = (numerator + denominator / 2) / denominator;
    if (nominal > 0 && frame >= nominal)
    {
        frame %= nominal;
    }
    std::time_t const seconds = static_cast<std::time_t>(sec);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf), dropFrame ? "%02d:%02d:%02d;%02d" : "%02d:%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec, frame);
    return buf;
}

std::uint64_t audioSamplesUntil(std::uint64_t index, int rateNumerator, int rateDenominator, int sampleRate)
{
    if (rateNumerator <= 0)
    {
        return 0;
    }
    auto const num = static_cast<__int128>(index) * static_cast<__int128>(sampleRate) * static_cast<__int128>(rateDenominator) +
                     static_cast<__int128>(rateNumerator) / 2;
    return static_cast<std::uint64_t>(num / static_cast<__int128>(rateNumerator));
}

int audioSamplesForFrame(std::uint64_t index, int rateNumerator, int rateDenominator, int sampleRate)
{
    auto const a = audioSamplesUntil(index, rateNumerator, rateDenominator, sampleRate);
    auto const b = audioSamplesUntil(index + 1, rateNumerator, rateDenominator, sampleRate);
    return static_cast<int>(b - a);
}

std::int64_t framePeriodNs(int rateNumerator, int rateDenominator)
{
    if (rateNumerator <= 0)
    {
        return 0;
    }
    return static_cast<std::int64_t>((static_cast<__int128>(1000000000) * rateDenominator + rateNumerator / 2) / rateNumerator);
}
} // namespace replay
