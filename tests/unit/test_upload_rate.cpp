#include <doctest/doctest.h>

#include "media/convert.hpp"
#include "media/format.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace replay;

namespace
{
// Source frames (their index) at `srcNum/srcDen` for `count` frames, converted to the house rate.
std::vector<int> convert(int srcNum, int srcDen, int count, int houseNum, int houseDen)
{
    FrameRateConverter rate(houseNum, houseDen);
    std::vector<int> out;
    for (int i = 1; i < count; ++i)
    {
        rate.emitUntil(static_cast<std::int64_t>(i) * 1000000000LL * srcDen / srcNum, i - 1, out);
    }
    rate.finish(static_cast<std::int64_t>(count) * 1000000000LL * srcDen / srcNum, count - 1, out);
    return out;
}

// A YUV4MPEG2 file of `frames` 16×8 frames at `rate` ("25:1"), frame i filled with luma 16 + 4·i.
std::vector<std::uint8_t> y4m(std::string const& rate, int frames)
{
    std::string text = "YUV4MPEG2 W16 H8 F" + rate + " Ip A1:1 C420jpeg\n";
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    for (int i = 0; i < frames; ++i)
    {
        std::string const header = "FRAME\n";
        bytes.insert(bytes.end(), header.begin(), header.end());
        bytes.insert(bytes.end(), 16 * 8, static_cast<std::uint8_t>(16 + 4 * i));
        bytes.insert(bytes.end(), 2 * 8 * 4, 128);
    }
    return bytes;
}
} // namespace

TEST_CASE("upload frame-rate conversion keeps the duration")
{
    // 25p into 50p: every frame twice.
    auto const up = convert(25, 1, 50, 50, 1);
    REQUIRE(up.size() == 100);
    CHECK(up[0] == 0);
    CHECK(up[1] == 0);
    CHECK(up[2] == 1);
    CHECK(up[99] == 49);
    // 50p into 25p: every second frame.
    auto const down = convert(50, 1, 100, 25, 1);
    REQUIRE(down.size() == 50);
    CHECK(down[1] == 2);
    CHECK(down[49] == 98);
    // Same rate: 1:1.
    auto const same = convert(50, 1, 7, 50, 1);
    CHECK(same == std::vector<int>{0, 1, 2, 3, 4, 5, 6});
    // 29.97 into 25 and back: the duration in house frames (10 s).
    CHECK(convert(30000, 1001, 300, 25, 1).size() == 251); // 300 frames are 10.01 s
    CHECK(convert(25, 1, 250, 30000, 1001).size() == 300);
    // One frame lasts at least one house frame.
    CHECK(convert(100, 1, 1, 25, 1).size() == 1);
}

TEST_CASE("a 25p y4m upload in a 50p house becomes 2 s of house frames")
{
    auto const file = y4m("25:1", 50);
    auto const converted = convertUpload(file.data(), file.size(), parseVideoFormat("64x32p50"));
    CHECK(converted.error.empty());
    REQUIRE(converted.frames.size() == 100);
    CHECK(converted.frames[0].y[0] == converted.frames[1].y[0]);
    CHECK(converted.frames[1].y[0] != converted.frames[2].y[0]);
    // 50p into a 25p house: every second frame, 1 s.
    auto const fast = y4m("50:1", 50);
    CHECK(convertUpload(fast.data(), fast.size(), parseVideoFormat("64x32p25")).frames.size() == 25);
    // Same rate: 1:1, as before.
    CHECK(convertUpload(file.data(), file.size(), parseVideoFormat("64x32p25")).frames.size() == 50);
}
