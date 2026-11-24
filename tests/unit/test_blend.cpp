#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "flow/cuda_flow.hpp"
#include "media/jpeg.hpp"
#include "media/v210.hpp"
#include "playout/transition.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace replay;

namespace
{
constexpr std::int64_t kPeriod = 20000000;

Frame10 noise(int width, int height, std::uint32_t seed)
{
    Frame10 frame;
    frame.allocate(width, height);
    auto next = [&] { return seed = seed * 1103515245u + 12345u; };
    for (auto& v : frame.y)
    {
        v = static_cast<std::uint16_t>((next() >> 8) % 1024);
    }
    for (std::size_t i = 0; i < frame.cb.size(); ++i)
    {
        frame.cb[i] = static_cast<std::uint16_t>((next() >> 8) % 1024);
        frame.cr[i] = static_cast<std::uint16_t>((next() >> 8) % 1024);
    }
    return frame;
}

// The 16-bit route render took before: unpack, blendInto's formula, pack.
std::vector<std::uint8_t> referenceBlend(Frame10 a, Frame10 const& b, std::uint32_t w)
{
    auto blend = [w](std::vector<std::uint16_t>& l, std::vector<std::uint16_t> const& r) {
        for (std::size_t i = 0; i < l.size(); ++i)
        {
            l[i] = blendSample(l[i], r[i], w);
        }
    };
    blend(a.y, b.y);
    blend(a.cb, b.cb);
    blend(a.cr, b.cr);
    std::vector<std::uint8_t> out(v210Size(a.width, a.height));
    packV210(a, out.data(), 0);
    return out;
}

// A textured picture per frame (each JPEG differs), with a ramp so a blend is visible.
Frame10 texture(int index)
{
    Frame10 frame;
    frame.allocate(64, 32);
    for (int y = 0; y < 32; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            frame.y[static_cast<std::size_t>(y * 64 + x)] = static_cast<std::uint16_t>(80 + ((x * 11 + y * 7 + index * 53) % 800));
        }
    }
    for (std::size_t i = 0; i < frame.cb.size(); ++i)
    {
        frame.cb[i] = static_cast<std::uint16_t>(300 + (i * 3 + static_cast<std::size_t>(index) * 31) % 400);
        frame.cr[i] = static_cast<std::uint16_t>(700 - (i * 5 + static_cast<std::size_t>(index) * 17) % 400);
    }
    return frame;
}

std::string metric(Engine& engine, std::string const& series)
{
    auto const text = engine.metrics().render();
    auto const at = text.find("mxl_replay_" + series + " ");
    return at == std::string::npos ? std::string{} : text.substr(at + series.size() + 12, text.find('\n', at) - at - series.size() - 12);
}
} // namespace

TEST_CASE("blendV210 gives the samples of unpack, blend and pack, padding included, also in place")
{
    for (int const width : {1920, 64, 70})
    {
        auto const a = noise(width, 4, 1);
        auto const b = noise(width, 4, 2);
        std::vector<std::uint8_t> pa(v210Size(width, 4));
        std::vector<std::uint8_t> pb(pa.size());
        packV210(a, pa.data(), 0);
        packV210(b, pb.data(), 0);
        for (std::uint32_t const w : {0u, 1u, 39u, 512u, 1023u, 1024u})
        {
            std::vector<std::uint8_t> out(pa.size(), 0xEE);
            blendV210(pa.data(), pb.data(), out.data(), out.size(), w);
            CHECK(out == referenceBlend(a, b, w));
            auto inPlace = pa;
            blendV210(inPlace.data(), pb.data(), inPlace.data(), inPlace.size(), w);
            CHECK(inPlace == out);
        }
    }
}

TEST_CASE("a CPU slow motion blends in v210, bit-exact, with frame B decoded ahead and reused as the next frame A")
{
    if (cudaFlowAvailable())
    {
        MESSAGE("a GPU is visible: this is the CPU path's test");
        return;
    }
    {
        auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-blend";
        std::filesystem::remove_all(dir);
        std::map<std::string, std::string> env{{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"},
            {"REPLAY_BUFFER_HOURS", "0.01"}, {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()},
            {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"REPLAY_RAMP_FRAMES", "0"}, {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}};
        {
            auto const loaded = loadConfig(env, {});
            Engine engine(loaded.config);
            engine.openBuffer();
            // What each stored frame decodes to: the reference blends these.
            std::vector<Frame10> stored;
            for (int i = 0; i < 16; ++i)
            {
                engine.ingestVideo(1, 1, static_cast<std::uint64_t>(i) * kPeriod, texture(i));
                auto const bytes = encodeJpeg422(texture(i), loaded.config.jpegQuality);
                Frame10 decoded;
                REQUIRE(decodeJpeg422(bytes.data(), bytes.size(), decoded));
                stored.push_back(decoded);
            }
            engine.setMotion(1, MotionMode::Blend);
            engine.setPosition(1, 2 * kPeriod);
            engine.setSpeed(1, 0.5);
            engine.play(1);
            auto const settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); };
            // Grains at 2.5, 3.0, 3.5, … 6.0: a blend of k and k+1, then k+1 alone.
            double position = 2.5;
            for (int grain = 0; grain < 8; ++grain, position += 0.5)
            {
                settle();
                auto const frame = engine.render(1, static_cast<std::uint64_t>(grain) * kPeriod);
                int const k = static_cast<int>(position);
                std::vector<std::uint8_t> expected;
                if (position > k)
                {
                    expected = referenceBlend(stored[static_cast<std::size_t>(k)], stored[static_cast<std::size_t>(k + 1)], 512);
                    CHECK_FALSE(frame.exact);
                }
                else
                {
                    expected.resize(v210Size(64, 32));
                    packV210(stored[static_cast<std::size_t>(k)], expected.data(), 0);
                }
                CAPTURE(position);
                CHECK(frame.v210 == expected);
            }
            // Frames 2 to 6, each fetched once: the first grain's A and B decoded here (nothing was planned),
            // then one new B per blend from the worker. The exact grains show the B before; nothing is decoded
            // twice.
            auto const count = [&](char const* result) {
                auto const text = metric(engine, std::string("decode_ahead_frames_total{channel=\"1\",result=\"") + result + "\"}");
                return text.empty() ? 0 : std::stoi(text);
            };
            CHECK(count("miss") == 2);
            CHECK(count("hit") == 3);
            CHECK(count("late") == 0);
        }
        std::filesystem::remove_all(dir);
    }
}
