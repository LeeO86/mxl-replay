#include <doctest/doctest.h>

#include "media/convert.hpp"
#include "media/format.hpp"

#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace replay;

namespace
{
// House frames (source frame per house frame) for source frames starting at `starts` (ns), the last ending at `endNs`.
std::vector<std::uint32_t> convert(std::vector<std::int64_t> const& starts, std::int64_t endNs, int houseNum, int houseDen)
{
    FrameRateConverter rate(houseNum, houseDen);
    std::vector<std::uint32_t> order;
    for (std::size_t i = 0; i < starts.size(); ++i)
    {
        rate.add(static_cast<std::uint32_t>(i), starts[i], order);
    }
    rate.finish(endNs, order);
    return order;
}

// `count` frames at `num/den` frames a second.
std::vector<std::uint32_t> convert(int num, int den, int count, int houseNum, int houseDen)
{
    std::vector<std::int64_t> starts;
    for (int i = 0; i < count; ++i)
    {
        starts.push_back(static_cast<std::int64_t>(i) * 1000000000LL * den / num);
    }
    return convert(starts, static_cast<std::int64_t>(count) * 1000000000LL * den / num, houseNum, houseDen);
}

std::vector<std::uint32_t> identity(std::size_t n)
{
    std::vector<std::uint32_t> out(n);
    std::iota(out.begin(), out.end(), 0u);
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

struct Riff
{
    std::vector<std::uint8_t> b;
    void tag(char const* t) { b.insert(b.end(), t, t + 4); }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void u16(std::uint16_t v)
    {
        b.push_back(static_cast<std::uint8_t>(v));
        b.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void append(Riff const& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};
Riff chunk(char const* id, Riff const& body)
{
    Riff c;
    c.tag(id);
    c.u32(static_cast<std::uint32_t>(body.b.size()));
    c.append(body);
    return c;
}
Riff list(char const* type, Riff const& body)
{
    Riff l;
    l.tag(type);
    l.append(body);
    return chunk("LIST", l);
}
// An AVI with one 16×8 video stream of an unknown codec (fourcc ZZZZ): no decoder and no pixel format.
// sws_getContext asserts on that (the process aborted before 1.8.1).
std::vector<std::uint8_t> unknownCodecAvi()
{
    Riff avih;
    for (std::uint32_t v : {40000u, 0u, 0u, 0x10u, 2u, 0u, 1u, 0u, 16u, 8u, 0u, 0u, 0u, 0u})
    {
        avih.u32(v);
    }
    Riff strh;
    strh.tag("vids");
    strh.tag("ZZZZ");
    for (std::uint32_t v : {0u, 0u, 0u, 1u, 25u, 0u, 2u, 0u, 0xFFFFFFFFu, 0u})
    {
        strh.u32(v);
    }
    for (int i = 0; i < 4; ++i)
    {
        strh.u16(0);
    }
    Riff strf;
    strf.u32(40);
    strf.u32(16);
    strf.u32(8);
    strf.u16(1);
    strf.u16(24);
    strf.tag("ZZZZ");
    for (int i = 0; i < 5; ++i)
    {
        strf.u32(0);
    }
    Riff strl;
    strl.append(chunk("strh", strh));
    strl.append(chunk("strf", strf));
    Riff hdrl;
    hdrl.append(chunk("avih", avih));
    hdrl.append(list("strl", strl));
    Riff frame;
    frame.b.assign(64, 0x55);
    Riff movi;
    movi.append(chunk("00dc", frame));
    movi.append(chunk("00dc", frame));
    Riff avi;
    avi.tag("AVI ");
    avi.append(list("hdrl", hdrl));
    avi.append(list("movi", movi));
    return chunk("RIFF", avi).b;
}
} // namespace

TEST_CASE("upload frame-rate conversion: nearest source frame, the duration stays")
{
    // 25p into 50p: every frame twice.
    auto const up = convert(25, 1, 50, 50, 1);
    REQUIRE(up.size() == 100);
    CHECK(up[0] == 0);
    CHECK(up[1] == 0);
    CHECK(up[2] == 1);
    CHECK(up[3] == 1);
    CHECK(up[99] == 49);
    // 50p into 25p: every second frame.
    auto const down = convert(50, 1, 100, 25, 1);
    REQUIRE(down.size() == 50);
    CHECK(down[0] == 0);
    CHECK(down[1] == 2);
    CHECK(down[49] == 98);
    // Same rate: 1:1.
    CHECK(convert(50, 1, 7, 50, 1) == identity(7));
    // 29.97 into 25 and back: the duration in house frames (10.01 s and 10 s).
    CHECK(convert(30000, 1001, 300, 25, 1).size() == 250);
    CHECK(convert(25, 1, 250, 30000, 1001).size() == 300);
    // One frame lasts at least one house frame.
    CHECK(convert(100, 1, 1, 25, 1).size() == 1);
}

TEST_CASE("upload frame-rate conversion stays 1:1 at equal rates with timestamp jitter")
{
    // ±3 ms on 50p timestamps: "newest at or before" stuttered here; the nearest frame does not.
    std::mt19937 random(7);
    std::uniform_int_distribution<int> jitter(-3000000, 3000000);
    for (auto const& [num, period] : {std::pair{50, 20000000LL}, std::pair{25, 40000000LL}})
    {
        std::vector<std::int64_t> starts;
        for (int i = 0; i < 500; ++i)
        {
            starts.push_back(std::max<std::int64_t>(0, i * period + (i == 0 ? 0 : jitter(random))));
        }
        CHECK(convert(starts, 500 * period, num, 1) == identity(500));
    }
}

TEST_CASE("upload source clock: a frame without a timestamp keeps its place")
{
    SourceClock clock{40000000};
    CHECK(clock.start(0, 0) == 0);
    CHECK(clock.start(40000000, 0) == 40000000);
    CHECK(clock.start(-1, 0) == 80000000);           // no timestamp: after the previous one by its duration
    CHECK(clock.start(120000000, 0) == 120000000);   // timestamps again
    CHECK(clock.start(100000000, 0) == 120000000);   // never before the previous one
    CHECK(clock.start(-1, 20000000) == 160000000);   // previous duration 40 ms (from the deltas)
    CHECK(clock.endNs() == 180000000);               // its own duration, 20 ms
    SourceClock none{40000000};
    CHECK(none.start(-1, 0) == 0);
    CHECK(none.start(-1, 0) == 40000000);
    CHECK(none.start(-1, 0) == 80000000);
}

TEST_CASE("a 25p y4m upload in a 50p house becomes 2 s of house frames, each source frame kept once")
{
    auto const file = y4m("25:1", 50);
    auto const converted = convertUpload(file.data(), file.size(), parseVideoFormat("64x32p50"));
    CHECK(converted.error.empty());
    CHECK(converted.frames.size() == 50); // not copied per repeat
    REQUIRE(converted.order.size() == 100);
    CHECK(converted.order[0] == 0);
    CHECK(converted.order[1] == 0);
    CHECK(converted.order[2] == 1);
    CHECK(converted.frames[0].y[0] != converted.frames[1].y[0]);
    // 50p into a 25p house: every second frame, 1 s.
    auto const fast = y4m("50:1", 50);
    CHECK(convertUpload(fast.data(), fast.size(), parseVideoFormat("64x32p25")).order.size() == 25);
    // Same rate: 1:1, as before.
    CHECK(convertUpload(file.data(), file.size(), parseVideoFormat("64x32p25")).order == identity(50));
}

TEST_CASE("an upload whose video cannot be decoded or scaled fails with a reason, without a crash")
{
    auto const file = unknownCodecAvi();
    auto const converted = convertUpload(file.data(), file.size(), parseVideoFormat("64x32p50"));
    CHECK(converted.frames.empty());
    CHECK(converted.order.empty());
    CHECK(converted.error == "unsupported video: no decoder for an unknown codec");
}
