// Stored pictures decoded ahead (SPECIFICATION.md §5.1, 1.8.5): the frames the next grains show, the worker
// that decodes them into a few slots, and the playout that takes a decoded one or decodes the frame itself.
#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "playout/decodeahead.hpp"
#include "playout/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace replay;

namespace
{
constexpr std::int64_t kPeriod = 20000000;

// A 50p scheduler `frames` source frames in, at `speed` without a ramp.
Scheduler schedulerAt(double frames, double speed, bool playing = true)
{
    Scheduler scheduler;
    scheduler.positionNs = static_cast<std::int64_t>(frames * kPeriod);
    scheduler.setTargetSpeed(speed, 0);
    scheduler.playing = playing;
    return scheduler;
}

// The frames ahead of a channel at `frames` (the current grain) for the next 2 grains.
std::vector<std::int64_t> ahead(double frames, double speed, MotionMode motion = MotionMode::Interpolate, bool withB = true, bool playing = true)
{
    auto const scheduler = schedulerAt(frames, speed, playing);
    auto const current = pickSource(scheduler.positionNs, 0, kPeriod, 1.0, 0.1, motion);
    auto const next = framesAhead(scheduler, current, 1.0, 0.1, motion, withB, 2);
    return {next.frames.begin(), next.frames.begin() + next.count};
}

using Frames = std::vector<std::int64_t>;
} // namespace

TEST_CASE("the frames the next grains show at speeds 1, 2, 0.5, 0.25 and -1, without the current grain's")
{
    CHECK(ahead(10, 1) == Frames{11, 12});
    CHECK(ahead(10, 2) == Frames{12, 14});
    CHECK(ahead(10, -1) == Frames{9, 8});
    // 50 %: 10.5 is between 10 (shown now) and 11; 11 is the next exact frame.
    CHECK(ahead(10, 0.5) == Frames{11});
    // Between 10 and 11 now: 11 is held already, 11.5 needs 12.
    CHECK(ahead(10.5, 0.5) == Frames{12});
    // 25 %: both next grains interpolate 10 → 11.
    CHECK(ahead(10, 0.25) == Frames{11});
    CHECK(ahead(10.75, 0.25) == Frames{12});
    // Reverse slow motion: 9.5 needs 9 (and 10, shown now).
    CHECK(ahead(10, -0.5) == Frames{9});
    // Repeat picks the nearest frame: 10.5 rounds up to 11.
    CHECK(ahead(10, 0.5, MotionMode::Repeat) == Frames{11});
    // 10.25 rounds down to 10 (shown now), 10.5 up to 11.
    CHECK(ahead(10, 0.25, MotionMode::Repeat) == Frames{11});
    // Blend needs frame B like interpolate. Without B (the CPU path) a grain between two frames adds nothing, and
    // decodes none of its frames into a single picture: the next exact frame is needed even when it was B.
    CHECK(ahead(10, 0.5, MotionMode::Blend) == Frames{11});
    CHECK(ahead(10.5, 0.5, MotionMode::Blend, false) == Frames{11});
    CHECK(ahead(10.25, 0.25, MotionMode::Interpolate, false) == Frames{});
}

TEST_CASE("a paused channel shows its frame on the next grain and has the first frame of a play ready")
{
    CHECK(ahead(10, 1, MotionMode::Interpolate, true, false) == Frames{11});
    CHECK(ahead(10, 0.5, MotionMode::Interpolate, true, false) == Frames{11});
    CHECK(ahead(10, -1, MotionMode::Interpolate, true, false) == Frames{9});
    // A ramp: the copy steps as the channel will (0 → 1 over 2 grains: 10, 10.5, 11.5).
    Scheduler ramp = schedulerAt(10, 0);
    ramp.setTargetSpeed(1, 2);
    auto const current = pickSource(ramp.positionNs, 0, kPeriod, 1.0, 0.1, MotionMode::Interpolate);
    auto const next = framesAhead(ramp, current, 1.0, 0.1, MotionMode::Interpolate, true, 2);
    CHECK(Frames(next.frames.begin(), next.frames.begin() + next.count) == Frames{11, 12});
}

TEST_CASE("the decode worker decodes the plan in order; the playout takes a decoded frame, waits a bounded time for one being decoded, else decodes it")
{
    std::mutex mutex;
    std::vector<std::uint64_t> decoded;
    std::atomic<int> slowMs{0};
    std::atomic<bool> fail{false};
    DecodeAhead worker([&](AheadFrame const& frame, int slot, std::uint64_t& storedNs) {
        CHECK(slot >= 0);
        CHECK(slot < DecodeAhead::kSlots);
        std::this_thread::sleep_for(std::chrono::milliseconds(slowMs.load()));
        {
            std::lock_guard lock{mutex};
            decoded.push_back(frame.taiNs);
        }
        storedNs = frame.taiNs + 1;
        return !fail.load();
    });
    auto const waitDecoded = [&](std::size_t count) {
        for (int i = 0; i < 200; ++i)
        {
            {
                std::lock_guard lock{mutex};
                if (decoded.size() >= count)
                {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    auto const plan = [&](std::vector<std::uint64_t> const& tais) {
        std::vector<AheadFrame> frames;
        for (auto tai : tais)
        {
            frames.push_back({1, tai});
        }
        worker.plan(frames.data(), static_cast<int>(frames.size()));
    };

    plan({10, 11});
    REQUIRE(waitDecoded(2));
    {
        std::lock_guard lock{mutex};
        CHECK(decoded == std::vector<std::uint64_t>{10, 11});
    }
    auto hit = worker.take({1, 10}, std::chrono::milliseconds(0));
    CHECK(hit.result == DecodeAhead::Result::Hit);
    CHECK(hit.storedNs == 11);
    CHECK(hit.planned);
    worker.give(hit.slot);
    // Not planned (a jump, a cue, another camera): a miss at once.
    auto const began = std::chrono::steady_clock::now();
    CHECK(worker.take({1, 99}, std::chrono::milliseconds(500)).result == DecodeAhead::Result::Miss);
    CHECK(worker.take({2, 10}, std::chrono::milliseconds(500)).result == DecodeAhead::Result::Miss);
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(100));

    // A slow decode: a short wait gives up (late), a long one gets it.
    slowMs = 150;
    plan({12});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto const late = worker.take({1, 12}, std::chrono::milliseconds(10));
    CHECK(late.result == DecodeAhead::Result::Late);
    CHECK(late.slot == -1);
    CHECK(late.planned);
    CHECK(late.waited >= std::chrono::milliseconds(10));
    auto const waited = worker.take({1, 12}, std::chrono::milliseconds(1000));
    CHECK(waited.result == DecodeAhead::Result::Hit);
    CHECK(waited.waited > std::chrono::milliseconds(0));
    worker.give(waited.slot);
    slowMs = 0;

    // A frame that cannot be decoded is a miss and is not tried again while it stays planned.
    fail = true;
    plan({13});
    REQUIRE(waitDecoded(4));
    CHECK(worker.take({1, 13}, std::chrono::milliseconds(0)).result == DecodeAhead::Result::Miss);
    plan({13});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::lock_guard lock{mutex};
        CHECK(decoded.size() == 4);
    }
    fail = false;
}

TEST_CASE("the decode worker keeps a held slot and the planned frames, and reuses the least recently used one")
{
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, int>> decoded;
    DecodeAhead worker([&](AheadFrame const& frame, int slot, std::uint64_t& storedNs) {
        std::lock_guard lock{mutex};
        decoded.emplace_back(frame.taiNs, slot);
        storedNs = frame.taiNs;
        return true;
    });
    auto const waitDecoded = [&](std::size_t count) {
        for (int i = 0; i < 200; ++i)
        {
            {
                std::lock_guard lock{mutex};
                if (decoded.size() >= count)
                {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    std::vector<AheadFrame> frames{{1, 1}, {1, 2}, {1, 3}, {1, 4}};
    worker.plan(frames.data(), 4);
    REQUIRE(waitDecoded(4));
    auto const held = worker.take({1, 1}, std::chrono::milliseconds(0));
    REQUIRE(held.result == DecodeAhead::Result::Hit);
    // Four new frames while frame 1 is held: three slots are free to reuse, the fourth frame waits for the give.
    frames = {{1, 5}, {1, 6}, {1, 7}, {1, 8}};
    worker.plan(frames.data(), 4);
    REQUIRE(waitDecoded(7));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::lock_guard lock{mutex};
        REQUIRE(decoded.size() == 7);
        for (std::size_t i = 4; i < 7; ++i)
        {
            CHECK(decoded[i].second != held.slot);
        }
    }
    worker.give(held.slot);
    REQUIRE(waitDecoded(8));
    std::lock_guard lock{mutex};
    CHECK(decoded[7] == std::pair<std::uint64_t, int>{8, held.slot});
}

namespace
{
constexpr std::uint64_t kBase = 1000 * static_cast<std::uint64_t>(kPeriod);

std::uint64_t at(int frame)
{
    return kBase + static_cast<std::uint64_t>(frame) * static_cast<std::uint64_t>(kPeriod);
}

int lumaOf(RenderedFrame const& frame)
{
    REQUIRE(frame.v210.size() >= 16);
    std::uint32_t word = 0;
    std::memcpy(&word, frame.v210.data(), 4);
    return static_cast<int>((word >> 10) & 0x3ffu);
}

// The value of the decode_ahead_frames_total series of channel 1 with `result`.
double decodeAhead(Engine& engine, std::string const& result)
{
    std::istringstream lines(engine.metrics().render());
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.rfind("mxl_replay_decode_ahead_frames_total{", 0) == 0 && line.find("channel=\"1\"") != std::string::npos &&
            line.find("result=\"" + result + "\"") != std::string::npos)
        {
            return std::stod(line.substr(line.rfind(' ') + 1));
        }
    }
    return -1;
}
} // namespace

TEST_CASE("a playing channel shows the pictures decoded ahead; a jump decodes its first picture on the output thread")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-decodeahead";
    std::filesystem::remove_all(dir);
    auto const loaded = loadConfig({{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
                                       {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()},
                                       {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"CH1_MOTION", "repeat"}, {"NMOS_HOST_ADDRESS", "10.4.4.4"},
                                       {"HOST_ID", "ci"}},
        {});
    auto engine = std::make_unique<Engine>(loaded.config, loaded.flat);
    engine->openBuffer();
    for (int i = 0; i < 80; ++i)
    {
        engine->ingestAudio(1, at(i), std::vector<float>(960 * 2, 0.5F), 2);
        Frame10 frame;
        frame.allocate(64, 32);
        frame.fill(static_cast<std::uint16_t>(100 + 10 * i), 512, 512);
        engine->ingestVideo(1, 1, at(i), std::move(frame));
    }
    CHECK(decodeAhead(*engine, "hit") == 0);
    CHECK(decodeAhead(*engine, "late") == 0);
    CHECK(decodeAhead(*engine, "miss") == 0);
    engine->setPosition(1, at(10));
    engine->play(1);
    std::uint64_t out = at(500);
    // Each grain shows the next frame (one luma step of 10 per frame, 8–12 after the 8-bit JPEG round trip).
    auto const play = [&](int grains) {
        std::vector<int> luma;
        for (int i = 0; i < grains; ++i)
        {
            luma.push_back(lumaOf(engine->render(1, out)));
            out += static_cast<std::uint64_t>(kPeriod);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (std::size_t i = 1; i < luma.size(); ++i)
        {
            CAPTURE(i);
            CHECK(std::abs(luma[i] - luma[i - 1] - 10) <= 2);
        }
        return luma;
    };
    auto const first = play(20);
    // The first grain of the play is decoded on the output thread; with a GPU the worker sets up its GPU state at
    // its first decode, so the next one or two can be late.
    CHECK(decodeAhead(*engine, "hit") >= 16);
    CHECK(decodeAhead(*engine, "miss") + decodeAhead(*engine, "late") <= 3);
    auto const misses = decodeAhead(*engine, "miss") + decodeAhead(*engine, "late");
    auto const hits = decodeAhead(*engine, "hit");
    engine->setPosition(1, at(50));
    auto const second = play(10);
    CHECK(std::abs(second.front() - (first.front() + 10 * 40)) <= 4);
    // The first grain after the jump is decoded on the output thread, the others are hits.
    CHECK(decodeAhead(*engine, "miss") + decodeAhead(*engine, "late") == misses + 1);
    CHECK(decodeAhead(*engine, "hit") == hits + 9);
    CHECK(engine->metrics().render().find("mxl_replay_decode_ahead_wait_seconds_bucket{le=\"+Inf\",channel=\"1\"}") != std::string::npos);
    engine.reset();
    std::filesystem::remove_all(dir);
}
