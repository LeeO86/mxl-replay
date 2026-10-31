// Stored pictures decoded ahead (SPECIFICATION.md §5.1, 1.8.5): the frames the next grains show, the worker
// that decodes them into a few slots, and the playout that takes a decoded one or decodes the frame itself.
#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "flow/cuda_flow.hpp"
#include "playout/decodeahead.hpp"
#include "playout/scheduler.hpp"

#include <algorithm>
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

TEST_CASE("the decode worker keeps a held slot, the planned and the kept frames, and reuses the least recently used one")
{
    static_assert(DecodeAhead::kSlots == DecodeAhead::kMaxPlan + DecodeAhead::kMaxKeep);
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
    auto const slotOf = [&](std::uint64_t tai) {
        std::lock_guard lock{mutex};
        for (auto const& [frame, slot] : decoded)
        {
            if (frame == tai)
            {
                return slot;
            }
        }
        return -1;
    };
    auto const frames = [](std::uint64_t first, int count) {
        std::vector<AheadFrame> out;
        for (int i = 0; i < count; ++i)
        {
            out.push_back({1, first + static_cast<std::uint64_t>(i)});
        }
        return out;
    };
    static_assert(DecodeAhead::kMaxPlan == 8 && DecodeAhead::kSlots == 10);
    auto plan = frames(1, DecodeAhead::kMaxPlan); // 1–8
    worker.plan(plan.data(), static_cast<int>(plan.size()));
    REQUIRE(waitDecoded(8));
    // Frame 1 is held, frame 2 kept for the current grain; 8 new frames: 2 free slots, then the least recently
    // used of 3–8, never the held or the kept one.
    auto const held = worker.take({1, 1}, std::chrono::milliseconds(0));
    REQUIRE(held.result == DecodeAhead::Result::Hit);
    AheadFrame const kept{1, 2};
    plan = frames(11, DecodeAhead::kMaxPlan); // 11–18
    worker.plan(plan.data(), static_cast<int>(plan.size()), &kept, 1);
    REQUIRE(waitDecoded(16));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::lock_guard lock{mutex};
        REQUIRE(decoded.size() == 16);
        for (std::size_t i = 8; i < 16; ++i)
        {
            CAPTURE(i);
            CHECK(decoded[i].second != held.slot);
        }
    }
    CHECK(slotOf(13) == slotOf(3));
    auto const second = worker.take({1, 2}, std::chrono::milliseconds(0));
    CHECK(second.result == DecodeAhead::Result::Hit);
    CHECK(worker.take({1, 8}, std::chrono::milliseconds(0)).result == DecodeAhead::Result::Miss);
    // A slot whose picture was taken goes before one not taken yet (frames decoded early for later grains): frame
    // 1, taken first, before 12–18, which were decoded later but never taken.
    worker.give(held.slot);
    worker.give(second.slot);
    auto const eleven = worker.take({1, 11}, std::chrono::milliseconds(0));
    REQUIRE(eleven.result == DecodeAhead::Result::Hit);
    worker.give(eleven.slot);
    AheadFrame const later{1, 21};
    worker.plan(&later, 1);
    REQUIRE(waitDecoded(17));
    std::lock_guard lock{mutex};
    CHECK(decoded[16].second == held.slot);
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

namespace
{
// A source at frame `frame` of camera `camera` (source period kPeriod) at `speed`, repeat.
AheadSource sourceAt(int camera, double frame, double speed, MotionMode motion = MotionMode::Repeat)
{
    AheadSource source;
    source.scheduler = schedulerAt(frame, speed);
    source.scheduler.camera = camera;
    source.current = pickSource(source.scheduler.positionNs, 0, kPeriod, 1.0, 0.1, motion);
    source.camera = camera;
    source.periodNs = static_cast<std::uint64_t>(kPeriod);
    source.motion = motion;
    return source;
}

AheadFrame frameOf(int camera, std::uint64_t frame)
{
    return {camera, frame * static_cast<std::uint64_t>(kPeriod)};
}

std::vector<AheadFrame> framesOf(DecodePlan const& plan)
{
    return {plan.frames.begin(), plan.frames.begin() + plan.count};
}

std::vector<AheadFrame> keptOf(DecodePlan const& plan)
{
    return {plan.keep.begin(), plan.keep.begin() + plan.keepCount};
}

using AheadFrames = std::vector<AheadFrame>;
} // namespace

TEST_CASE("the decode plan: one source, the next clip's first frames before OUT, and both entries of a mix")
{
    // One source at speed 1: the next 2 frames; the current one is kept.
    auto const one = sourceAt(1, 10, 1);
    auto plan = planAhead(&one, 1, nullptr, 0, 2);
    CHECK(framesOf(plan) == AheadFrames{frameOf(1, 11), frameOf(1, 12)});
    CHECK(keptOf(plan) == AheadFrames{frameOf(1, 10)});
    // Between two frames (interpolate on the GPU): A and B are kept.
    auto const slow = sourceAt(1, 10.5, 0.5, MotionMode::Interpolate);
    plan = planAhead(&slow, 1, nullptr, 0, 2);
    CHECK(framesOf(plan) == AheadFrames{frameOf(1, 12)});
    CHECK(keptOf(plan) == AheadFrames{frameOf(1, 10), frameOf(1, 11)});
    // Without B (the CPU path) a grain between two frames keeps nothing.
    auto cpu = slow;
    cpu.withB = false;
    CHECK(planAhead(&cpu, 1, nullptr, 0, 2).keepCount == 0);

    // Before OUT (frame 11): the next clip's IN and the frame after it follow the first grain; frame 12 is after
    // OUT and is not shown.
    auto cut = sourceAt(1, 10, 1);
    cut.untilNs = 11 * static_cast<std::uint64_t>(kPeriod);
    AheadFrame const next[] = {frameOf(0, 500), frameOf(0, 501)};
    plan = planAhead(&cut, 1, next, 2, 2);
    CHECK(framesOf(plan) == AheadFrames{frameOf(1, 11), frameOf(0, 500), frameOf(0, 501)});
    // A loop back to IN on the same camera.
    AheadFrame const loop[] = {frameOf(1, 2), frameOf(1, 3)};
    plan = planAhead(&cut, 1, loop, 2, 2);
    CHECK(framesOf(plan) == AheadFrames{frameOf(1, 11), frameOf(1, 2), frameOf(1, 3)});

    // A mix: the incoming entry (camera 0 from 500) and the outgoing one (camera 1, holding at its OUT 21), grain
    // by grain; both current frames kept.
    AheadSource mix[] = {sourceAt(0, 500, 1), sourceAt(1, 20, 1)};
    mix[1].untilNs = 21 * static_cast<std::uint64_t>(kPeriod);
    plan = planAhead(mix, 2, nullptr, 0, 2);
    CHECK(framesOf(plan) == AheadFrames{frameOf(0, 501), frameOf(1, 21), frameOf(0, 502)});
    CHECK(keptOf(plan) == AheadFrames{frameOf(0, 500), frameOf(1, 20)});
    // The incoming entry further ahead than the outgoing one (a mix needs two new pictures a grain).
    mix[0].grains = 6;
    plan = planAhead(mix, 2, nullptr, 0, 2);
    CHECK(framesOf(plan) == AheadFrames{frameOf(0, 501), frameOf(1, 21), frameOf(0, 502), frameOf(0, 503), frameOf(0, 504), frameOf(0, 505), frameOf(0, 506)});
    // Two sources at 200 % and the next clip's 6 frames: at most kMaxPlan frames, the soonest first.
    AheadSource busy[] = {sourceAt(0, 500, 2), sourceAt(1, 20, 2)};
    AheadFrame const more[] = {frameOf(2, 900), frameOf(2, 901), frameOf(2, 902), frameOf(2, 903), frameOf(2, 904), frameOf(2, 905)};
    plan = planAhead(busy, 2, more, 6, 2);
    CHECK(plan.count == DecodeAhead::kMaxPlan);
    CHECK(framesOf(plan) == AheadFrames{frameOf(0, 502), frameOf(1, 22), frameOf(2, 900), frameOf(2, 901), frameOf(2, 902), frameOf(2, 903), frameOf(2, 904),
                                frameOf(2, 905)});
}

namespace
{
// One camera, 64×32p50, frames 0–79 recorded, each a luma step of 10 brighter.
std::unique_ptr<Engine> recordedEngine(std::filesystem::path const& dir)
{
    std::filesystem::remove_all(dir);
    auto const loaded = loadConfig({{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.1"},
                                       {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()},
                                       {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"REPLAY_PROTECT_MAX_PCT", "90"}, {"CH1_MOTION", "repeat"},
                                       {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
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
    return engine;
}

std::string clipOf(Engine& engine, int in, int out)
{
    engine.setPosition(1, at(in));
    engine.markIn(1);
    engine.setPosition(1, at(out));
    engine.markOut(1);
    std::string error;
    auto id = engine.createClip(1, "", false, false, error);
    INFO(error);
    REQUIRE(error.empty());
    return id;
}
} // namespace

TEST_CASE("the first frames after OUT are decoded ahead: the next entry, a loop to IN, the idle playlist, a mix; a cued shot's frame at the cue")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-decodeahead-ends";
    auto engine = recordedEngine(dir);
    auto const a = clipOf(*engine, 10, 20);
    auto const b = clipOf(*engine, 40, 50);
    std::uint64_t out = at(500);
    // Renders `grains` grains 10 ms apart; the luma of each.
    auto const render = [&](int grains) {
        std::vector<int> luma;
        for (int i = 0; i < grains; ++i)
        {
            luma.push_back(lumaOf(engine->render(1, out)));
            out += static_cast<std::uint64_t>(kPeriod);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return luma;
    };
    auto const notAhead = [&] { return decodeAhead(*engine, "miss") + decodeAhead(*engine, "late"); };
    // The worker sets up its GPU state at its first decode: time for it before the grains are counted.
    auto const settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };
    // One frame of the luma ramp (100 + 10·frame, ±4 after the JPEG round trip).
    auto const shows = [](int luma, int frame) { return std::abs(luma - (100 + 10 * frame)) <= 4; };

    SUBCASE("the next entry at OUT, cut")
    {
        auto const list = engine->createPlaylist("cut", {PlaylistEntry{a, 1, EndAction::Next, true}, PlaylistEntry{b, 1, EndAction::Freeze, true}});
        engine->playPlaylist(1, list);
        settle();
        auto const luma = render(20);
        // a plays from 11 (the cut's audio crossfade starts b a few grains before a's OUT), then b from its IN; b
        // freezes at its OUT (no decode).
        auto const join = std::find_if(luma.begin(), luma.end(), [&](int y) { return shows(y, 40); });
        REQUIRE(join != luma.end());
        REQUIRE(join != luma.begin());
        CHECK(*(join - 1) < 100 + 10 * 21);
        CHECK(notAhead() == 0);
        CHECK(decodeAhead(*engine, "hit") >= 17);
    }
    SUBCASE("a loop back to IN")
    {
        auto const list = engine->createPlaylist("loop", {PlaylistEntry{a, 1, EndAction::Loop, true}});
        engine->playPlaylist(1, list);
        settle();
        auto const luma = render(25);
        CHECK(std::count_if(luma.begin(), luma.end(), [&](int y) { return shows(y, 10); }) >= 1);
        CHECK(notAhead() == 0);
    }
    SUBCASE("the idle playlist's next entry and its first again")
    {
        auto const idle = engine->createPlaylist("idle", {PlaylistEntry{a}, PlaylistEntry{b}});
        std::string error;
        REQUIRE(engine->setIdlePlaylist(1, idle, error));
        engine->stop(1);
        (void)render(1); // the idle playlist starts at a's IN
        settle();
        auto const before = notAhead();
        (void)render(30); // a → b → a
        CHECK(notAhead() == before);
    }
    SUBCASE("a cued shot")
    {
        CHECK(engine->shotClick(1, b) == ShotState::Cued);
        settle();
        auto const luma = render(3);
        CHECK(shows(luma.front(), 40));
        CHECK(notAhead() == 0);
        CHECK(decodeAhead(*engine, "hit") == 1);
        // The play's first grain was decoded while the channel was cued.
        engine->play(1);
        (void)render(3);
        CHECK(notAhead() == 0);
    }
    SUBCASE("a mix (GPU only: the CPU mix decodes its frames itself)")
    {
        if (cudaFlowAvailable())
        {
            PlaylistEntry second{b, 1, EndAction::Freeze, true};
            second.transition = Transition::Mix;
            second.transitionFrames = 4;
            auto const list = engine->createPlaylist("mix", {PlaylistEntry{a, 1, EndAction::Next, true}, second});
            engine->playPlaylist(1, list);
            settle();
            (void)render(20);
            CHECK(notAhead() == 0);
            CHECK(decodeAhead(*engine, "hit") >= 20);
        }
    }
    engine.reset();
    std::filesystem::remove_all(dir);
}
