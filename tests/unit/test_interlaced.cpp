#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "media/anc.hpp"
#include "media/format.hpp"
#include "media/v210.hpp"
#include "mxl/io.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace replay;

namespace
{
// A v210 frame whose line y has every byte (y + 1): any line moved shows.
std::vector<std::uint8_t> lineNumbered(std::size_t rowBytes, int height)
{
    std::vector<std::uint8_t> frame(rowBytes * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
    {
        std::fill_n(frame.begin() + static_cast<std::ptrdiff_t>(rowBytes * static_cast<std::size_t>(y)), rowBytes, static_cast<std::uint8_t>(y + 1));
    }
    return frame;
}
} // namespace

TEST_CASE("interlaced house formats: 1080i25 and 1080i2997, top field first only")
{
    auto const i25 = parseVideoFormat("1080i25");
    CHECK(i25.interlaced);
    CHECK(i25.width == 1920);
    CHECK(i25.height == 1080);
    CHECK(i25.rateNum == 25);
    CHECK(i25.rateDen == 1);
    CHECK(i25.token() == "1920x1080i25");
    CHECK(parseVideoFormat(i25.token()) == i25);
    CHECK(parseVideoFormat("1080i25tff") == i25);
    CHECK(parseVideoFormat("1080I25") == i25);

    auto const i2997 = parseVideoFormat("1080i2997");
    CHECK(i2997.interlaced);
    CHECK(i2997.rateNum == 30000);
    CHECK(i2997.rateDen == 1001);
    CHECK(parseVideoFormat("1080i29.97") == i2997);
    CHECK(parseVideoFormat(i2997.token()) == i2997);

    // The number after i is the frame rate; MXL has interlaced flows at 25 and 29.97 only.
    CHECK_THROWS_AS(parseVideoFormat("1080i50"), std::runtime_error);
    CHECK_THROWS_AS(parseVideoFormat("1080i5994"), std::runtime_error);
    CHECK_THROWS_AS(parseVideoFormat("1080i25bff"), std::runtime_error);
    CHECK_THROWS_AS(parseVideoFormat("1920x1081i25"), std::runtime_error);

    auto const p50 = parseVideoFormat("1080p50");
    CHECK_FALSE(p50.interlaced);
    CHECK(p50.token() == "1920x1080p50");
    CHECK_FALSE(p50 == parseVideoFormat("1080i25"));
    // 29.97 written with a dot was read as 29/1.
    CHECK(parseVideoFormat("1080p29.97").rateNum == 30000);
    CHECK(parseVideoFormat("1080p29.97").rateDen == 1001);
}

TEST_CASE("video receivers accept interlaced_tff only in an interlaced house")
{
    CHECK(acceptedInterlaceModes(parseVideoFormat("1080p50")) == std::vector<std::string>{"progressive"});
    CHECK(acceptedInterlaceModes(parseVideoFormat("1080i25")) == std::vector<std::string>{"progressive", "interlaced_tff"});
}

TEST_CASE("how a video flow is read: woven in an interlaced house, refused otherwise")
{
    auto const p50 = parseVideoFormat("1080p50");
    auto const i25 = parseVideoFormat("1080i25");
    auto const flow = [](char const* mode, int num, int den) {
        return std::string(R"({"format":"urn:x-nmos:format:video","interlace_mode":")") + mode + R"(","grain_rate":{"numerator":)" +
               std::to_string(num) + R"(,"denominator":)" + std::to_string(den) + "}}";
    };
    // Progressive house: as 1.7.1, every interlaced flow is refused.
    CHECK(videoInputMode(flow("interlaced_tff", 25, 1), p50).unsupported == "interlaced");
    CHECK(videoInputMode(flow("interlaced_bff", 25, 1), p50).unsupported == "interlaced");
    CHECK(videoInputMode(flow("progressive", 50, 1), p50).unsupported.empty());
    CHECK_FALSE(videoInputMode(flow("progressive", 50, 1), p50).interlaced);

    // Interlaced house: tff at the house frame rate is woven.
    auto const tff = videoInputMode(flow("interlaced_tff", 25, 1), i25);
    CHECK(tff.interlaced);
    CHECK(tff.unsupported.empty());
    CHECK(videoInputMode(flow("interlaced_bff", 25, 1), i25).unsupported == "interlaced_bff");
    CHECK(videoInputMode(flow("interlaced_tff", 30000, 1001), i25).unsupported == "interlaced_rate");
    CHECK(videoInputMode(flow("interlaced_psf", 25, 1), i25).unsupported == "interlaced");
    CHECK(videoInputMode(flow("progressive", 25, 1), i25).unsupported.empty());
    CHECK_FALSE(videoInputMode(flow("progressive", 25, 1), i25).interlaced);
    CHECK(videoInputMode(flow("interlaced_tff", 30000, 1001), parseVideoFormat("1080i2997")).interlaced);

    // Nothing to go by: read as progressive, as before.
    CHECK(videoInputMode(R"({"format":"urn:x-nmos:format:video"})", i25).unsupported.empty());
    CHECK_FALSE(videoInputMode("", i25).interlaced);
}

TEST_CASE("field parity follows the convention: grain 2k is the first field")
{
    CHECK(fieldParity(0, false) == 0); // tff: first field = top = even lines
    CHECK(fieldParity(1, false) == 1);
    CHECK(fieldParity(1000, false) == 0);
    CHECK(fieldParity(0, true) == 1); // bff: first field = bottom = odd lines
    CHECK(fieldParity(1, true) == 0);
    CHECK(fieldParity(1001, true) == 0);
}

TEST_CASE("unweave and weave, top field first and bottom field first")
{
    int const height = 8;
    auto const rowBytes = v210RowBytes(12);
    auto const frame = lineNumbered(rowBytes, height);
    for (bool const bff : {false, true})
    {
        CAPTURE(bff);
        std::uint64_t const k = 21;
        std::vector<std::uint8_t> first(rowBytes * height / 2);
        std::vector<std::uint8_t> second(rowBytes * height / 2);
        extractField(frame.data(), rowBytes, height, fieldParity(2 * k, bff), first.data());
        extractField(frame.data(), rowBytes, height, fieldParity(2 * k + 1, bff), second.data());
        for (int i = 0; i < height / 2; ++i)
        {
            // tff: grain 2k row i is frame line 2i, grain 2k+1 row i frame line 2i+1; bff the other way round.
            CHECK(first[rowBytes * static_cast<std::size_t>(i)] == 2 * i + (bff ? 1 : 0) + 1);
            CHECK(second[rowBytes * static_cast<std::size_t>(i)] == 2 * i + (bff ? 0 : 1) + 1);
        }
        std::vector<std::uint8_t> woven(frame.size(), 0);
        weaveField(first.data(), rowBytes, height, fieldParity(2 * k, bff), woven.data());
        weaveField(second.data(), rowBytes, height, fieldParity(2 * k + 1, bff), woven.data());
        CHECK(woven == frame);
    }
}

TEST_CASE("a woven 1080i frame keeps every v210 sample")
{
    Frame10 picture;
    picture.allocate(1920, 1080);
    for (int y = 0; y < picture.height; ++y)
    {
        for (int x = 0; x < picture.width; ++x)
        {
            picture.y[static_cast<std::size_t>(y * picture.width + x)] = static_cast<std::uint16_t>(64 + (x * 7 + y * 13) % 876);
        }
    }
    auto const rowBytes = v210RowBytes(1920);
    std::vector<std::uint8_t> packed(v210Size(1920, 1080));
    packV210(picture, packed.data(), static_cast<int>(rowBytes));
    std::vector<std::uint8_t> top(packed.size() / 2);
    std::vector<std::uint8_t> bottom(packed.size() / 2);
    extractField(packed.data(), rowBytes, 1080, fieldParity(0, false), top.data());
    extractField(packed.data(), rowBytes, 1080, fieldParity(1, false), bottom.data());
    CHECK(top.size() == 2764800); // 5120 × 540, MXL's grain of a 1080i v210 field
    std::vector<std::uint8_t> woven(packed.size());
    weaveField(top.data(), rowBytes, 1080, fieldParity(0, false), woven.data());
    weaveField(bottom.data(), rowBytes, 1080, fieldParity(1, false), woven.data());
    Frame10 back;
    back.allocate(1920, 1080);
    unpackV210(woven.data(), static_cast<int>(rowBytes), back);
    CHECK(back.y == picture.y);
}

TEST_CASE("an interlaced house records and plays frames, with one ANC grain per field")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-interlaced";
    std::filesystem::remove_all(dir);
    std::map<std::string, std::string> env{
        {"REPLAY_FORMAT", "64x32i25"},
        {"REPLAY_INPUTS", "1"},
        {"REPLAY_CHANNELS", "1"},
        {"REPLAY_BUFFER_HOURS", "0.01"},
        {"REPLAY_STORAGE_DIR", dir.string()},
        {"REPLAY_STATE_DIR", (dir / "state").string()},
        {"NMOS_HOST_ADDRESS", "10.1.2.3"},
        {"REPLAY_ALLOW_CPU_INTERP", "true"},
        {"REPLAY_STORAGE_MIN_MBPS", "0"},
        {"HOST_ID", "test-host"},
    };
    {
        Engine engine(loadConfig(env, {}).config);
        CHECK(engine.config().format.interlaced);
        engine.openBuffer();
        // Frames at the frame rate (40 ms), as the reader stores woven ones.
        for (int i = 0; i < 6; ++i)
        {
            Frame10 frame;
            frame.allocate(64, 32);
            frame.fill(static_cast<std::uint16_t>(100 + i * 100), 512, 512);
            engine.ingestVideo(1, 1, static_cast<std::uint64_t>(i) * 40000000ull, std::move(frame));
        }
        engine.pause(1);
        engine.setAngle(1, 1);
        // Jog is frame-accurate: one step is one frame of 40 ms.
        engine.setPosition(1, 80000000ull);
        auto const third = engine.render(1, 0);
        CHECK(third.positionNs == 80000000ull);
        CHECK(third.v210.size() == v210Size(64, 32));
        Frame10 picture;
        picture.allocate(64, 32);
        unpackV210(third.v210.data(), static_cast<int>(v210RowBytes(64)), picture);
        CHECK(picture.y[0] == 300);
        CHECK(picture.y[static_cast<std::size_t>(64 * 31)] == 300); // both fields from the same frame

        // ANC: the first field's grain has F = 0b10, the second's F = 0b11, the same timecode.
        REQUIRE(third.anc.size() == kAncGrainSize);
        REQUIRE(third.ancSecond.size() == kAncGrainSize);
        CHECK(((third.anc[3] >> 6) & 0x3) == 0x2);
        CHECK(((third.ancSecond[3] >> 6) & 0x3) == 0x3);
        Timecode first;
        Timecode second;
        AtcKind kind = AtcKind::Ltc;
        REQUIRE(decodeAncTimecode(third.anc.data(), third.anc.size(), first, kind));
        REQUIRE(decodeAncTimecode(third.ancSecond.data(), third.ancSecond.size(), second, kind));
        CHECK(first.field == 0);
        CHECK(second.field == 1);
        CHECK(first.format() == second.format());
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a progressive house writes no second ANC grain")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-progressive-anc";
    std::filesystem::remove_all(dir);
    std::map<std::string, std::string> env{
        {"REPLAY_FORMAT", "64x32p50"},         {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
        {"REPLAY_STORAGE_DIR", dir.string()},  {"REPLAY_STATE_DIR", (dir / "state").string()}, {"NMOS_HOST_ADDRESS", "10.1.2.3"},
        {"REPLAY_STORAGE_MIN_MBPS", "0"},      {"HOST_ID", "test-host"},
    };
    {
        Engine engine(loadConfig(env, {}).config);
        engine.openBuffer();
        auto const rendered = engine.render(1, 0);
        CHECK(rendered.anc.size() == kAncGrainSize);
        CHECK(((rendered.anc[3] >> 6) & 0x3) == 0x0);
        CHECK(rendered.ancSecond.empty());
    }
    std::filesystem::remove_all(dir);
}
