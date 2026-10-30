// The read-ahead of stored frames (SPECIFICATION.md §5.3, 1.8.3): the frames ahead of a channel's position and the
// first frames of the clip it plays after OUT are read on a loader thread before the output needs them.
#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "record/ring.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace replay;

namespace
{
constexpr std::uint64_t kPeriod = 20000000ull;
constexpr std::uint64_t kBase = 1000 * kPeriod;

std::uint64_t at(int frame)
{
    return kBase + static_cast<std::uint64_t>(frame) * kPeriod;
}

// One camera, 64×32p50, frames 0–59 recorded.
std::unique_ptr<Engine> recordedEngine(std::filesystem::path const& dir)
{
    std::filesystem::remove_all(dir);
    auto const loaded = loadConfig({{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
                                       {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()},
                                       {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"REPLAY_PROTECT_MAX_PCT", "90"}, {"CH1_MOTION", "repeat"},
                                       {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
        {});
    auto engine = std::make_unique<Engine>(loaded.config, loaded.flat);
    engine->openBuffer();
    for (int i = 0; i < 60; ++i)
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
    REQUIRE(error.empty());
    return id;
}

// The read-aheads the loader did, as the observer saw them.
struct Seen
{
    std::mutex mutex;
    std::vector<ReadAhead::Request> requests;

    void watch(Engine& engine)
    {
        engine.setReadAheadObserver([this](ReadAhead::Request const& request, std::uint64_t bytes) {
            std::lock_guard lock{mutex};
            if (bytes > 0)
            {
                requests.push_back(request);
            }
        });
    }
    void clear()
    {
        std::lock_guard lock{mutex};
        requests.clear();
    }
    // Waits up to a second for a read that starts at `fromNs` (exact), or one that covers `fromNs`.
    bool startsAt(std::uint64_t fromNs, bool exact = true)
    {
        for (int i = 0; i < 100; ++i)
        {
            {
                std::lock_guard lock{mutex};
                for (auto const& request : requests)
                {
                    if (request.channel == 1 && request.camera == 1 && (exact ? request.fromNs == fromNs : request.fromNs <= fromNs && fromNs <= request.toNs))
                    {
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
};
} // namespace

TEST_CASE("a ring prefetch reads the records of the frames in the range, across segments")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-prefetch";
    std::filesystem::remove_all(dir);
    constexpr std::uint64_t kSecond = 1000000000ull;
    FrameRing ring(dir.string(), 3600 * kSecond, 1);
    for (int i = 0; i < 10; ++i)
    {
        StoredFrame frame;
        frame.taiNs = static_cast<std::uint64_t>(i + 1) * kSecond / 2;
        frame.jpeg.assign(static_cast<std::size_t>(100 + i), static_cast<std::uint8_t>(i));
        frame.audio.assign(10, 0.25F);
        REQUIRE(ring.push(std::move(frame)));
    }
    REQUIRE(ring.segmentCount() > 1);
    // Frames 2–5 (1.5 s to 3 s): a 16-byte header, the JPEG and 10 floats each.
    std::uint64_t expected = 0;
    for (int i = 2; i <= 5; ++i)
    {
        expected += 16 + static_cast<std::uint64_t>(100 + i) + 10 * sizeof(float);
    }
    CHECK(ring.prefetch(3 * kSecond / 2, 3 * kSecond) == expected);
    CHECK(ring.prefetch(20 * kSecond, 30 * kSecond) == 0);
    CHECK(ring.prefetch(3 * kSecond / 2 + 1, 3 * kSecond / 2 + 2) == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("the next playlist entry's first frames are read before OUT; the frames ahead of a cued clip at the cue")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-readahead";
    auto engine = recordedEngine(dir);
    auto const a = clipOf(*engine, 5, 14);
    auto const b = clipOf(*engine, 40, 49);
    Seen seen;
    seen.watch(*engine);
    std::uint64_t out = at(500);
    auto const render = [&](int grains) {
        for (int i = 0; i < grains; ++i)
        {
            (void)engine->render(1, out);
            out += kPeriod;
        }
    };

    SUBCASE("auto-advance and a cued next entry")
    {
        for (bool const advance : {true, false})
        {
            seen.clear();
            auto const list = engine->createPlaylist("x", {PlaylistEntry{a, 1, EndAction::Next, advance}, PlaylistEntry{b, 1, EndAction::Freeze, true}});
            engine->playPlaylist(1, list);
            // Entry a plays from frame 5: the frames ahead of it (read once: the second pass has them), and b's IN
            // (OUT is less than 2 s away), before b plays.
            render(1);
            CHECK(seen.startsAt(at(9), false) == advance);
            CHECK(seen.startsAt(at(40)));
        }
    }
    SUBCASE("the idle playlist: its next entry, and its first after a clip that ends")
    {
        auto const idle = engine->createPlaylist("idle", {PlaylistEntry{b}, PlaylistEntry{a}});
        std::string error;
        REQUIRE(engine->setIdlePlaylist(1, idle, error));
        engine->stop(1);
        render(1);
        CHECK(seen.startsAt(at(45), false));
        CHECK(seen.startsAt(at(5)));
        // A clip ending with freeze plays the idle playlist from b: b's IN is read before the clip's OUT.
        auto const c = clipOf(*engine, 20, 29);
        seen.clear();
        engine->playClip(1, c);
        render(1);
        CHECK(seen.startsAt(at(25), false));
        CHECK(seen.startsAt(at(40)));
    }
    SUBCASE("a cued clip (play on a red light) has its first frames read at the cue")
    {
        CHECK(engine->shotClick(1, b) == ShotState::Cued);
        render(1);
        CHECK(seen.startsAt(at(40)));
        CHECK(seen.startsAt(at(49), false));
    }
    SUBCASE("live reads nothing ahead")
    {
        engine->live(1);
        render(5);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard lock{seen.mutex};
        CHECK(seen.requests.empty());
    }
    engine->setReadAheadObserver({});
    engine.reset();
    std::filesystem::remove_all(dir);
}
