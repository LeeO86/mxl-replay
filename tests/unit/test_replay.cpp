#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "domain/scan.hpp"
#include "flow/cuda_flow.hpp"
#include "flow/dis.hpp"
#include "library/catalog.hpp"
#include "media/anc.hpp"
#include "media/audio.hpp"
#include "media/jpeg.hpp"
#include "media/timebase.hpp"
#include "media/v210.hpp"
#include "mxl/io.hpp"
#include "nmos/ids.hpp"
#include "ops/httpserver.hpp"
#include "ops/api.hpp"
#include "ops/metrics.hpp"
#include "playout/scheduler.hpp"
#include "playout/shotbox.hpp"
#include "record/hfr.hpp"
#include "record/ring.hpp"
#include "util/logging.hpp"
#include "util/uuid.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace replay;

TEST_CASE("tai index matches the MXL rounding vectors")
{
    CHECK(timestampToIndex(50, 1, 0) == 0);
    CHECK(timestampToIndex(30000, 1001, 0) == 0);
    auto const period = indexToTimestamp(30000, 1001, 1);
    CHECK(timestampToIndex(30000, 1001, period) == 1);
    CHECK(framePeriodNs(50, 1) == 20000000);
}

TEST_CASE("audio cadence is 960 at 50 and around 1601.6 at 29.97")
{
    CHECK(audioSamplesForFrame(0, 50, 1) == 960);
    CHECK(audioSamplesForFrame(10, 50, 1) == 960);
    int total = 0;
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        int const n = audioSamplesForFrame(i, 30000, 1001);
        CHECK(n >= 1601);
        CHECK(n <= 1602);
        total += n;
    }
    CHECK(total == 8008);
    int total60 = 0;
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        int const n = audioSamplesForFrame(i, 60000, 1001);
        CHECK(n >= 800);
        CHECK(n <= 801);
        total60 += n;
    }
    CHECK(total60 == 4004);
}

TEST_CASE("scheduler ramps speed and keeps the angle position")
{
    Scheduler scheduler;
    scheduler.framePeriodNs = 1000;
    scheduler.positionNs = 0;
    scheduler.playing = true;
    scheduler.speed = 1;
    scheduler.setTargetSpeed(0.5, 3);
    CHECK(scheduler.step() == doctest::Approx(1.0 + (0.5 - 1.0) * 1.0 / 3.0));
    CHECK(scheduler.step() == doctest::Approx(1.0 + (0.5 - 1.0) * 2.0 / 3.0));
    CHECK(scheduler.step() == doctest::Approx(0.5));
    auto const position = scheduler.positionNs;
    scheduler.camera = 2;
    CHECK(scheduler.positionNs == position);
    scheduler.playing = false;
    scheduler.scrubFrames(-2);
    CHECK(scheduler.positionNs == position - 2000);
}

TEST_CASE("source picker snaps HFR frames and interpolates the rest")
{
    auto const exact = pickSource(0, 0, 20000000, 1, 0.1, MotionMode::Interpolate);
    CHECK(exact.kind == SourcePick::Kind::Exact);
    auto const snapped = pickSource(400000, 0, 20000000, 3, 0.1, MotionMode::Interpolate);
    CHECK(snapped.snapped);
    auto const between = pickSource(5000000, 0, 20000000, 3, 0.1, MotionMode::Interpolate);
    CHECK(between.kind == SourcePick::Kind::Interpolate);
    CHECK(between.phase == doctest::Approx(0.75).epsilon(0.02));
    auto const repeated = pickSource(5000000, 0, 20000000, 3, 0.1, MotionMode::Repeat);
    CHECK(repeated.kind == SourcePick::Kind::Repeat);
}

TEST_CASE("phased HFR repeats a missing phase")
{
    std::vector<PhaseGrain> grains(3);
    grains[0] = PhaseGrain{0, 1000, true, 10};
    grains[1] = PhaseGrain{1, 1000, false, 0};
    grains[2] = PhaseGrain{2, 1000, true, 30};
    auto const result = interleavePhases(grains, 3, 30000);
    CHECK(result.missing == 1);
    CHECK(result.frames.size() == 3);
    bool sawRepeat = false;
    for (auto const& frame : result.frames)
    {
        if (frame.repeated)
        {
            sawRepeat = true;
            CHECK((frame.payload == 10 || frame.payload == 30));
        }
    }
    CHECK(sawRepeat);
    CHECK(result.frames[0].timeNs <= result.frames[1].timeNs);
    CHECK(result.frames[1].timeNs <= result.frames[2].timeNs);
}

TEST_CASE("a grain the writer never wrote is not a drop, a grain the reader missed is")
{
    std::uint64_t const start = 1000000000000ull;
    std::uint64_t const history = 49 * 20000000ull; // 50 grains at 50p
    ReaderWait wait;
    // Following the writer: a grain that left the history was missed (the encode stalled).
    CHECK(wait.lateIsDrop(start, history));
    // The writer had not written the reader's grain; 5 ms later it is gone: the writer
    // started after it (a player that opens its flows before it writes).
    wait.wait(start);
    CHECK_FALSE(wait.lateIsDrop(start + 5000000, history));
    // Longer than the history between two looks: the writer may have written it meanwhile.
    CHECK(wait.lateIsDrop(start + history, history));
    // Unknown history: every late grain counts, as before.
    CHECK(wait.lateIsDrop(start + 5000000, 0));
    // A grain read ends the wait.
    wait.read();
    CHECK(wait.lateIsDrop(start + 5000000, history));
}

TEST_CASE("an input without new grains stops once and resumes with its gap")
{
    std::uint64_t const start = 1000000000000ull;
    std::uint64_t const second = 1000000000ull;
    InputWatch watch{2 * second};
    // Not connected yet: nothing to watch.
    CHECK_FALSE(watch.idle(start + 10 * second));
    watch.connect(start);
    watch.recorded(start + second);
    // Grains every 20 ms, then the writer goes away: nothing until the timeout after the last one.
    CHECK_FALSE(watch.idle(start + 2 * second));
    CHECK_FALSE(watch.idle(start + 3 * second - 1));
    CHECK(watch.idle(start + 3 * second));
    CHECK(watch.reason(true) == std::string("no_grains"));
    CHECK(watch.reason(false) == std::string("flow_missing"));
    // Logged once, not on every step while it stays stopped.
    CHECK_FALSE(watch.idle(start + 4 * second));
    CHECK_FALSE(watch.idle(start + 60 * second));
    // The flow is back: the gap is the time since the last recorded grain.
    CHECK(watch.recorded(start + 8 * second) == 7 * second);
    CHECK_FALSE(watch.stopped);
    // A grain while running reports no gap; grains that arrive but cannot be recorded do not count.
    CHECK(watch.recorded(start + 8 * second + 20000000) == 0);
    watch.skipped = 3;
    CHECK(watch.idle(start + 11 * second));
    CHECK(watch.reason(true) == std::string("invalid_grains"));
    // A camera connected while its flow is missing stops after the timeout as well.
    InputWatch missing{2 * second};
    missing.connect(start);
    CHECK_FALSE(missing.idle(start + second));
    CHECK(missing.idle(start + 2 * second));
    // A clock that steps back does not stop an input.
    InputWatch back{2 * second};
    back.connect(start);
    CHECK_FALSE(back.idle(start - 10 * second));
}

namespace
{
constexpr std::uint64_t kSecond = 1000000000ull;

std::filesystem::path freshDir(char const* name)
{
    auto const dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

StoredFrame frameAt(std::uint64_t tai, std::uint8_t value)
{
    StoredFrame frame;
    frame.taiNs = tai;
    frame.jpeg = {value, static_cast<std::uint8_t>(value + 1)};
    frame.audio = {static_cast<float>(value), 0.5F};
    return frame;
}
} // namespace

TEST_CASE("disk ring reads frames back and survives a restart")
{
    auto const dir = freshDir("mxl-replay-ring-reopen");
    {
        FrameRing ring(dir.string(), 3600 * kSecond, 1);
        for (int i = 0; i < 10; ++i)
        {
            CHECK(ring.push(frameAt(static_cast<std::uint64_t>(i + 1) * kSecond / 2, static_cast<std::uint8_t>(i))));
        }
        CHECK(ring.size() == 10);
        auto const frame = ring.findNearest(3 * kSecond / 2 + 1);
        REQUIRE(frame);
        CHECK(frame->jpeg == std::vector<std::uint8_t>{2, 3});
        CHECK(frame->audio == std::vector<float>{2.0F, 0.5F});
        CHECK_FALSE(ring.push(frameAt(kSecond, 99)));
        CHECK(ring.segmentCount() == 4);
    }
    // 1.0.x files are removed; a record cut off at the end of a segment is ignored.
    std::ofstream(dir / "cam1_seg0.bin") << "old";
    std::vector<std::filesystem::path> segments;
    for (auto const& item : std::filesystem::directory_iterator(dir))
    {
        if (item.path().filename().string().rfind("seg-", 0) == 0)
        {
            segments.push_back(item.path());
        }
    }
    std::sort(segments.begin(), segments.end());
    REQUIRE(!segments.empty());
    {
        std::ofstream tail(segments.back(), std::ios::binary | std::ios::app);
        std::string const garbage(20, '\xff');
        tail << garbage;
    }
    FrameRing again(dir.string(), 3600 * kSecond, 1);
    CHECK(again.size() == 10);
    CHECK(again.findAtOrBefore(5 * kSecond)->jpeg[0] == 9);
    CHECK_FALSE(std::filesystem::exists(dir / "cam1_seg0.bin"));
    CHECK(again.push(frameAt(6 * kSecond, 20)));
    CHECK(again.findAfter(5 * kSecond)->jpeg[0] == 20);
    std::filesystem::remove_all(dir);
}

TEST_CASE("disk ring deletes expired segments unless a clip protects them")
{
    auto const dir = freshDir("mxl-replay-ring-retention");
    {
        FrameRing ring(dir.string(), 3 * kSecond, 1);
        ring.protect(kSecond, kSecond);
        // 0.5 s steps, one segment per 1.5 s: [0.5-1.5] [2-3] [3.5-4.5] [5-6] [6.5-7.5] [8-9] [9.5-10]
        for (int i = 0; i < 20; ++i)
        {
            CHECK(ring.push(frameAt(static_cast<std::uint64_t>(i + 1) * kSecond / 2, static_cast<std::uint8_t>(i))));
        }
        ring.enforceRetention();
        // Newest 10 s, retention 3 s: segments ending before 7 s go, except the protected first one.
        CHECK(ring.segmentCount() == 4);
        CHECK(ring.size() == 11);
        CHECK(ring.protectedCount() == 1);
        CHECK(ring.findNearest(kSecond)->jpeg[0] == 1);
        CHECK(ring.findAtOrBefore(6 * kSecond)->taiNs == 3 * kSecond / 2);
        ring.unprotect(kSecond, kSecond);
        ring.enforceRetention();
        CHECK(ring.segmentCount() == 3);
        CHECK_FALSE(ring.findAtOrBefore(6 * kSecond));
        CHECK(ring.protectedCount() == 0);
    }
    // The files are deleted in the background; the ring's destructor waits for that.
    auto const files = std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{});
    CHECK(files == 3);
    std::filesystem::remove_all(dir);
}

TEST_CASE("disk ring counts the segments a clip keeps and reads audio alone")
{
    auto const dir = freshDir("mxl-replay-ring-protected");
    FrameRing ring(dir.string(), 3600 * kSecond, 1);
    // 0.5 s steps, one segment per 1.5 s: [0.5-1.5] [2-3] [3.5-4.5]
    for (int i = 0; i < 9; ++i)
    {
        CHECK(ring.push(frameAt(static_cast<std::uint64_t>(i + 1) * kSecond / 2, static_cast<std::uint8_t>(i))));
    }
    CHECK(ring.protectedBytes() == 0);
    ring.protect(2 * kSecond, 2 * kSecond);
    // One protected frame keeps its whole segment: the file header and three records of
    // 16 + 2 (JPEG) + 8 (audio) bytes.
    CHECK(ring.protectedBytes() == 8 + 3 * (16 + 2 + 8));
    CHECK(ring.findNearestAudio(2 * kSecond) == std::vector<float>{3.0F, 0.5F});
    CHECK(ring.newestNs() == 9 * kSecond / 2);
    CHECK(ring.writeFailures() == 0);
    CHECK(ring.waitFor(4 * kSecond, std::chrono::milliseconds(0)));
    std::thread later([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ring.push(frameAt(5 * kSecond, 9));
    });
    CHECK(ring.waitFor(5 * kSecond, std::chrono::seconds(5)));
    later.join();
    CHECK_FALSE(ring.waitFor(6 * kSecond, std::chrono::milliseconds(10)));
    std::filesystem::remove_all(dir);
}

TEST_CASE("disk ring counts the frames it cannot write")
{
    auto const dir = freshDir("mxl-replay-ring-failures");
    FrameRing ring(dir.string(), 3600 * kSecond, 1);
    std::filesystem::remove_all(dir);
    CHECK_FALSE(ring.push(frameAt(kSecond, 1)));
    CHECK_FALSE(ring.push(frameAt(2 * kSecond, 2)));
    CHECK(ring.writeFailures() == 2);
    std::filesystem::create_directories(dir);
    CHECK(ring.push(frameAt(3 * kSecond, 3)));
    CHECK(ring.writeFailures() == 2);
    CHECK(ring.size() == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("disk ring reads while it writes and deletes segments")
{
    auto const dir = freshDir("mxl-replay-ring-threads");
    {
        FrameRing ring(dir.string(), 2 * kSecond, 1);
        std::atomic<bool> done{false};
        std::atomic<int> reads{0};
        std::thread reader([&] {
            while (!done)
            {
                auto const newest = ring.newestNs();
                if (auto const frame = ring.findNearest(newest > kSecond ? newest - kSecond : newest); frame && frame->jpeg.size() == 2)
                {
                    ++reads;
                }
                (void)ring.findNearestAudio(newest);
                (void)ring.protectedBytes();
            }
        });
        bool pushed = true;
        // At least 400 frames, and on until the reader has read one (a fast disk can finish the
        // 400 before the reader thread runs at all); at most 5 s.
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (int i = 0; i < 400 || (reads.load() == 0 && std::chrono::steady_clock::now() < deadline); ++i)
        {
            pushed = ring.push(frameAt(static_cast<std::uint64_t>(i + 1) * kSecond / 20, static_cast<std::uint8_t>(i))) && pushed;
        }
        done = true;
        reader.join();
        CHECK(pushed);
        CHECK(reads.load() > 0);
        // 20 s recorded, 2 s kept: the old segments are gone.
        CHECK(ring.segmentCount() <= 4);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("shotbox cues then plays and a playlist advances")
{
    Shotbox box;
    CHECK(box.click() == ShotState::Cued);
    CHECK(box.click() == ShotState::Playing);
    CHECK(box.click() == ShotState::Paused);
    box.playOnFirstClick = true;
    box.state = ShotState::Idle;
    CHECK(box.click() == ShotState::Playing);
    box.end = EndAction::Loop;
    CHECK(box.finished() == ShotState::Playing);
    box.end = EndAction::Freeze;
    CHECK(box.finished() == ShotState::Ended);
    std::vector<PlaylistEntry> entries(2);
    CHECK(playlistNext(entries, 0) == 1);
    CHECK(playlistNext(entries, 1) == -1);
}

TEST_CASE("time stretch length and mute fade")
{
    std::vector<float> input(2000, 0.25f);
    auto const stretched = timeStretch(input, 1, 0.5);
    CHECK(stretched.samples.size() == 4000);
    auto const followed = resampleLinear(input, 1, 2.0);
    CHECK(followed.size() == 1000);
    std::vector<float> fade(4, 1.f);
    applyMuteFade(fade, 0, 2);
    CHECK(fade.front() == doctest::Approx(1.0));
    CHECK(fade.back() == doctest::Approx(0.5));
    applyMuteFade(fade, 1, 2);
    CHECK(fade.back() < 0.01f);
}

TEST_CASE("jpeg straight from and to v210 gives the bytes of the 16-bit path")
{
    for (int const width : {64, 1920})
    {
        CAPTURE(width);
        int const height = width == 64 ? 20 : 1080;
        Frame10 frame;
        frame.allocate(width, height);
        std::uint32_t seed = 12345;
        auto next = [&] { return seed = seed * 1103515245u + 12345u; };
        for (auto& v : frame.y)
        {
            v = static_cast<std::uint16_t>(64 + (next() >> 8) % 877);
        }
        for (std::size_t i = 0; i < frame.cb.size(); ++i)
        {
            frame.cb[i] = static_cast<std::uint16_t>(64 + (next() >> 8) % 897);
            frame.cr[i] = static_cast<std::uint16_t>(64 + (next() >> 8) % 897);
        }
        std::vector<std::uint8_t> packed(v210Size(width, height));
        packV210(frame, packed.data(), 0);
        Frame10 unpacked;
        unpacked.allocate(width, height);
        unpackV210(packed.data(), 0, unpacked);
        auto const reference = encodeJpeg422(unpacked, 92);
        auto const direct = encodeJpegV210(packed.data(), width, height, 0, 92);
        CHECK(direct == reference);
        // Twice on the same thread: the kept codec gives the same bytes again.
        CHECK(encodeJpegV210(packed.data(), width, height, 0, 92) == reference);

        Frame10 decoded;
        REQUIRE(decodeJpeg422(reference.data(), reference.size(), decoded));
        std::vector<std::uint8_t> viaFrame(v210Size(width, height));
        packV210(decoded, viaFrame.data(), 0);
        std::vector<std::uint8_t> straight(v210Size(width, height), 0xAA);
        CHECK(decodeJpegToV210(reference.data(), reference.size(), width, height, 0, straight.data()));
        CHECK(straight == viaFrame);
        CHECK_FALSE(decodeJpegToV210(reference.data(), reference.size(), width + 6, height, 0, straight.data()));
    }
}

TEST_CASE("jpeg 4:2:2 round trip and v210 pack")
{
    CHECK(jpegStorageBitDepth() == 8);
    CHECK_FALSE(jpegSupports10Bit());
    CHECK_FALSE(jpegSupports12Bit());
    if (!cudaFlowAvailable())
    {
        CHECK(std::string(jpegRuntimeBackend()) == "libjpeg-turbo");
    }
    Frame10 frame;
    frame.allocate(32, 16);
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 32; ++x)
        {
            frame.y[static_cast<std::size_t>(y * 32 + x)] = static_cast<std::uint16_t>((x * 20) & 1023);
        }
    }
    auto const encoded = encodeJpeg422(frame, 95);
    Frame10 decoded;
    CHECK(decodeJpeg422(encoded.data(), encoded.size(), decoded));
    CHECK(decoded.width == 32);
    CHECK(decoded.y[0] / 4 == frame.y[0] / 4);
    std::vector<std::uint8_t> packed(v210Size(32, 16));
    packV210(frame, packed.data(), 0);
    Frame10 unpacked;
    unpacked.allocate(32, 16);
    unpackV210(packed.data(), 0, unpacked);
    CHECK(unpacked.y[5] == frame.y[5]);
    CHECK(unpacked.cb[3] == frame.cb[3]);
}

TEST_CASE("anc timecode round trip matches the source time")
{
    auto const tc = timecodeFromTai(3'661'000'000'000ull, 50, 1, false);
    AncPacket packet;
    packet.tc = tc;
    packet.kind = AtcKind::Vitc1;
    auto const grain = encodeAncGrain(packet);
    Timecode decoded;
    AtcKind kind = AtcKind::Ltc;
    CHECK(decodeAncTimecode(grain.data(), grain.size(), decoded, kind));
    CHECK(decoded == tc);
    CHECK(kind == AtcKind::Vitc1);
    CHECK(decoded.format() == formatTimecode(3'661'000'000'000ull, 50, 1, false));
}

TEST_CASE("config env overrides the file and unknown env keys are ignored")
{
    std::map<std::string, std::string> file{{"REPLAY_INPUTS", "2"}, {"WEB_PORT", "9000"}};
    std::map<std::string, std::string> env{{"REPLAY_INPUTS", "3"}, {"NMOS_CPP_REF", "abc"}, {"REPLAY_FORMAT", "720p25"}};
    auto const loaded = loadConfig(env, file);
    CHECK(loaded.config.inputs == 3);
    CHECK(loaded.config.webPort == 9000);
    CHECK(loaded.config.format.height == 720);
    CHECK(loaded.config.format.rateNum == 25);
    CHECK(loaded.config.nmosSeed.find("-replay") != std::string::npos);
    file["NOT_A_KEY"] = "1";
    CHECK_THROWS_AS(loadConfig({}, file), ConfigError);
}

TEST_CASE("nmos ids are deterministic uuid v5 and flows follow the format")
{
    auto const a = makeNmosIds("host-replay");
    auto const b = makeNmosIds("host-replay");
    CHECK(a.node == b.node);
    CHECK(isUuid(a.node));
    CHECK(a.videoFlow(1, "1080p50") != a.videoFlow(1, "1080p25"));
    CHECK(a.videoReceiver(1, 1) != a.videoReceiver(1, 2));
    CHECK(a.device != a.node);
}

TEST_CASE("catalog stores a clip and a playlist")
{
    auto const path = std::filesystem::temp_directory_path() / "mxl-replay-catalog.sqlite";
    std::filesystem::remove(path);
    Catalog catalog;
    catalog.open(path.string());
    ClipRef clip;
    clip.id = "c1";
    clip.name = "goal";
    clip.camera = 2;
    clip.inNs = 10;
    clip.outNs = 20;
    clip.speed = 0.5;
    clip.motion = "blend";
    clip.audio = "mute";
    clip.end = EndAction::Loop;
    catalog.upsertClip(clip);
    auto const stored = catalog.clip("c1");
    CHECK(stored.name == "goal");
    CHECK(stored.end == EndAction::Loop);
    Catalog::Playlist playlist;
    playlist.id = "p1";
    playlist.name = "highlights";
    playlist.entries.push_back(PlaylistEntry{"c1", 0.5, EndAction::Next, true});
    catalog.upsertPlaylist(playlist);
    CHECK(catalog.playlists().size() == 1);
    CHECK(catalog.playlists()[0].entries[0].autoAdvance);
    std::filesystem::remove(path);
}

TEST_CASE("storage estimate is about 70 GB per hour at 1080p50 q92")
{
    auto const bytes = bytesPerHourEstimate(1920, 1080, 50, 1, 92);
    CHECK(bytes > 60e9);
    CHECK(bytes < 80e9);
}

TEST_CASE("optical flow recovers a translation and the midpoint matches")
{
    constexpr int W = 64;
    constexpr int H = 32;
    auto make = [](float cx) {
        Yuv422 frame;
        frame.width = W;
        frame.height = H;
        frame.y.assign(W * H, 0.1f);
        frame.cb.assign((W / 2) * H, 0.5f);
        frame.cr.assign((W / 2) * H, 0.5f);
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                float const dx = static_cast<float>(x) - cx;
                float const dy = static_cast<float>(y) - 16.f;
                frame.y[static_cast<std::size_t>(y * W + x)] = 0.1f + 0.8f * std::exp(-(dx * dx + dy * dy) / (2.f * 6.f * 6.f));
            }
        }
        return frame;
    };
    auto const a = make(20.f);
    auto const mid = make(24.f);
    auto const b = make(28.f);
    OperatingPoint op = operatingPoint(InterpPreset::Fast);
    op.finestLevel = 0;
    op.coarsestLevel = 3;
    op.searchIterations = 12;
    auto const pair = computeFlowPair(grayFromLuma(a), grayFromLuma(b), op);
    double su = 0;
    int n = 0;
    for (int y = 10; y < 22; ++y)
    {
        for (int x = 16; x < 32; ++x)
        {
            su += pair.forward.u[static_cast<std::size_t>(y * W + x)];
            ++n;
        }
    }
    CHECK(su / n == doctest::Approx(8.0).epsilon(0.25));
    auto const got = interpolateFrames(a, b, pair.forward, pair.backward, 0.5f, op);
    CHECK(psnrLuma(got, mid) > 40.0);
    CHECK(ssimLuma(got, mid) > 0.95);
    CHECK(psnrLuma(interpolateFrames(a, b, pair.forward, pair.backward, 0.f, op), a) > 40.0);
}

TEST_CASE("engine records two angles, plays at half speed and creates a clip")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-engine";
    std::filesystem::remove_all(dir);
    std::map<std::string, std::string> env{
        {"REPLAY_FORMAT", "64x32p50"},
        {"REPLAY_INPUTS", "2"},
        {"REPLAY_CHANNELS", "2"},
        {"REPLAY_BUFFER_HOURS", "0.01"},
        {"REPLAY_STORAGE_DIR", dir.string()},
        {"REPLAY_STATE_DIR", (dir / "state").string()},
        {"NMOS_HOST_ADDRESS", "10.1.2.3"},
        {"REPLAY_ALLOW_CPU_INTERP", "true"},
        {"REPLAY_STORAGE_MIN_MBPS", "0"},
        {"REPLAY_PROTECT_MAX_PCT", "90"},
        {"HOST_ID", "test-host"},
    };
    Engine engine(loadConfig(env, {}).config);
    engine.openBuffer();
    for (int i = 0; i < 8; ++i)
    {
        for (int camera = 1; camera <= 2; ++camera)
        {
            Frame10 frame;
            frame.allocate(64, 32);
            frame.fill(static_cast<std::uint16_t>(100 + i * 40 + camera), 400, 600);
            engine.ingestVideo(camera, 1, static_cast<std::uint64_t>(i) * 20000000ull, std::move(frame));
        }
    }
    engine.pause(1);
    engine.setPosition(1, 0);
    engine.setAngle(1, 1);
    auto const before = engine.render(1, 0).positionNs;
    engine.setAngle(1, 2);
    CHECK(engine.render(1, 20000000).positionNs == before);
    engine.setSpeed(1, 0.5);
    engine.setMotion(1, MotionMode::Repeat);
    engine.play(1);
    auto const first = engine.render(1, 0);
    auto const second = engine.render(1, 20000000);
    CHECK(first.v210.size() == v210Size(64, 32));
    CHECK(second.speed < 1.0);
    CHECK(second.speed > 0.5);
    CHECK(second.positionNs > first.positionNs);
    CHECK(first.anc.size() == kAncGrainSize);
    Timecode tc;
    AtcKind kind = AtcKind::Ltc;
    CHECK(decodeAncTimecode(first.anc.data(), first.anc.size(), tc, kind));
    engine.setPosition(1, 0);
    engine.markIn(1);
    engine.setPosition(1, 4 * 20000000ull);
    engine.markOut(1);
    std::string error;
    auto const id = engine.createClip(1, "goal", true, false, error);
    CHECK(error.empty());
    CHECK_FALSE(id.empty());
    CHECK(engine.clips().size() == 2);
    engine.shotClick(1, id);
    CHECK(engine.shotClick(1, id) == ShotState::Playing);
    auto const played = engine.render(1, 0);
    CHECK(played.camera == 1);
    Frame10 still;
    still.allocate(64, 32);
    still.fill(700, 512, 512);
    auto const jpeg = encodeJpeg422(still, 90);
    auto const upload = engine.upload(jpeg.data(), jpeg.size(), "still", error);
    CHECK(error.empty());
    CHECK_FALSE(upload.empty());
    auto const exported = engine.exportClip(id, error);
    CHECK(std::filesystem::exists(exported + "/audio.wav"));
    HttpServer server;
    server.start(0, [&](HttpRequest const& request) { return handleApi(engine, request, "<html>replay</html>"); });
    CHECK(server.port() > 0);
    server.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("engine answers livez while the buffer is indexed and removes unused camera buffers")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-ready";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "media" / "cam7");
    std::ofstream(dir / "media" / "cam7" / "seg-00000000000000000001.bin") << "MXLR";
    auto const loaded = loadConfig(
        {{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
            {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()}, {"REPLAY_STORAGE_MIN_MBPS", "0"},
            {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
        {});
    Engine engine(loaded.config, loaded.flat);
    // Camera 7 is not configured and no clip uses it.
    CHECK_FALSE(std::filesystem::exists(dir / "media" / "cam7"));
    HttpRequest request;
    request.method = "GET";
    request.path = "/livez";
    CHECK(handleApi(engine, request, "").status == 200);
    request.path = "/readyz";
    CHECK(handleApi(engine, request, "").status == 503);
    request.path = "/api/v1/status";
    CHECK(handleApi(engine, request, "").status == 503);
    engine.openBuffer();
    CHECK(engine.bufferReady());
    request.path = "/readyz";
    CHECK(handleApi(engine, request, "").status == 200);
    request.path = "/metrics";
    CHECK(handleApi(engine, request, "").body.find("mxl_replay_storage_write_failed_total{camera=\"1\"} 0") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("engine plays camera audio and an upload made after recording")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-audio";
    std::filesystem::remove_all(dir);
    auto const loaded = loadConfig(
        {{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
            {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()}, {"REPLAY_STORAGE_MIN_MBPS", "0"},
            {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
        {});
    Engine engine(loaded.config, loaded.flat);
    engine.openBuffer();
    std::uint64_t const period = 20000000ull;
    std::uint64_t const base = 1000 * period;
    for (int i = 0; i < 6; ++i)
    {
        auto const tai = base + static_cast<std::uint64_t>(i) * period;
        engine.ingestAudio(1, tai, std::vector<float>(960 * 2, 0.25F + 0.01F * static_cast<float>(i)), 2);
        Frame10 frame;
        frame.allocate(64, 32);
        frame.fill(300, 512, 512);
        engine.ingestVideo(1, 1, tai, std::move(frame));
    }
    // Live, two frames behind: the output carries frame 2's audio.
    auto const live = engine.render(1, base + 4 * period);
    REQUIRE(live.audio.size() == 960 * 2);
    CHECK(live.audio.front() == doctest::Approx(0.27F));
    CHECK(live.audio.back() == doctest::Approx(0.27F));

    // Camera 1 has recorded, so its ring only takes newer frames: the upload must go elsewhere.
    Frame10 still;
    still.allocate(64, 32);
    still.fill(700, 512, 512);
    auto const jpeg = encodeJpeg422(still, 90);
    std::string error;
    auto const id = engine.upload(jpeg.data(), jpeg.size(), "still", error);
    REQUIRE(error.empty());
    ClipRef clip;
    for (auto const& item : engine.clips())
    {
        if (item.id == id)
        {
            clip = item;
        }
    }
    CHECK(clip.camera == kLibraryCamera);
    engine.playClip(1, id);
    engine.pause(1);
    engine.setPosition(1, clip.inNs);
    auto const played = engine.render(1, base + 10 * period);
    CHECK(played.camera == kLibraryCamera);
    REQUIRE(played.v210.size() == v210Size(64, 32));
    Frame10 picture;
    picture.allocate(64, 32);
    unpackV210(played.v210.data(), static_cast<int>(v210RowBytes(64)), picture);
    CHECK(std::abs(static_cast<int>(picture.y[0]) - 700) < 16);
    std::filesystem::remove_all(dir);
}

TEST_CASE("clip ids continue after a restart")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-serial";
    std::filesystem::remove_all(dir);
    auto const loaded = loadConfig(
        {{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
            {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()}, {"REPLAY_STORAGE_MIN_MBPS", "0"},
            {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
        {});
    Frame10 still;
    still.allocate(64, 32);
    still.fill(500, 512, 512);
    auto const jpeg = encodeJpeg422(still, 90);
    std::string first;
    {
        Engine engine(loaded.config, loaded.flat);
        engine.openBuffer();
        std::string error;
        first = engine.upload(jpeg.data(), jpeg.size(), "first", error);
        REQUIRE(error.empty());
    }
    Engine engine(loaded.config, loaded.flat);
    engine.openBuffer();
    std::string error;
    auto const second = engine.upload(jpeg.data(), jpeg.size(), "second", error);
    REQUIRE(error.empty());
    CHECK(second != first);
    CHECK(engine.clips().size() == 2);
    std::filesystem::remove_all(dir);
}

TEST_CASE("metrics render the replay prefix")
{
    Metrics metrics;
    metrics.inc("record_dropped_total", {{"camera", "1"}});
    metrics.observe("frame_gpu_seconds", {{"stage", "decode"}}, 0.004);
    auto const text = metrics.render();
    CHECK(text.find("mxl_replay_record_dropped_total") != std::string::npos);
    CHECK(text.find("mxl_replay_frame_gpu_seconds_bucket") != std::string::npos);
}

TEST_CASE("last-frame age and write rate follow the recorder, and the rate drops to 0 when idle")
{
    auto const dir = freshDir("mxl-replay-stall");
    std::map<std::string, std::string> env{{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "2"}, {"REPLAY_CHANNELS", "1"},
        {"REPLAY_BUFFER_HOURS", "0.01"}, {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()},
        {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"HOST_ID", "test-host"}};
    Engine engine(loadConfig(env, {}).config);
    engine.openBuffer();
    auto const value = [&](std::string const& series) {
        auto const text = engine.metrics().render();
        auto const at = text.find("mxl_replay_" + series + " ");
        REQUIRE(at != std::string::npos);
        return std::stod(text.substr(at + series.size() + 12));
    };
    auto const start = std::chrono::steady_clock::now();
    auto const now = taiNowNs();
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        Frame10 frame;
        frame.allocate(64, 32);
        frame.fill(300, 512, 512);
        engine.ingestVideo(1, 1, now - (4 - i) * 20000000ull, std::move(frame));
    }
    engine.updateMetrics(start + std::chrono::seconds(1));
    CHECK(value("write_bytes_per_second") > 0);
    CHECK(value("record_last_frame_age_seconds{camera=\"1\"}") < 1);
    // Camera 2 has recorded nothing: its age counts from the start.
    CHECK(value("record_last_frame_age_seconds{camera=\"2\"}") >= 0);
    CHECK(engine.statusJson().find("\"last_frame_age_s\":") != std::string::npos);
    // Nothing written for another second: the rate is 0, not the last value.
    engine.updateMetrics(start + std::chrono::milliseconds(2001));
    CHECK(value("write_bytes_per_second") == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("platform settings keep aliases and reject bad values")
{
    std::map<std::string, std::string> env{{"NMOS_HOST_ADDRESS", "10.8.0.4"}, {"NMOS_REGISTRY_PORT", "4000"}, {"HOST_ID", "pod"}};
    auto const loaded = loadConfig(env, {});
    CHECK(loaded.config.nmosQueryAddress == "");
    CHECK(loaded.config.nmosQueryPort == 4001);
    CHECK(loaded.config.nmosHostAddress == "10.8.0.4");
    CHECK(loaded.config.nmosDnsSd == false);
    CHECK(loaded.config.stateDir == "/config");
    CHECK(loaded.config.cleanupOnExit == false);
    CHECK(loaded.config.shutdownTimeoutS == 10);
    CHECK(loaded.config.historyDurationNs == 2000000000ull);
    CHECK(loaded.config.inputStallS == 2);
    CHECK(nodeLabel(loaded.config) == "pod");
    CHECK(deviceLabel(loaded.config) == "MXL Replay");
    CHECK(loaded.config.outputDomainId == makeNmosIds(loaded.config.nmosSeed).domain);

    env["NMOS_LABEL"] = "Sport Replay";
    env["NMOS_QUERY_ADDRESS"] = "10.0.0.5";
    env["NMOS_QUERY_PORT"] = "4008";
    env["NMOS_TAGS"] = "{\"urn:x-srf:production\":[\"sport-sa\"],\"urn:x-srf:function\":[\"replay\"]}";
    auto const tagged = loadConfig(env, {});
    CHECK(nodeLabel(tagged.config) == "Sport Replay");
    CHECK(deviceLabel(tagged.config) == "Sport Replay");
    CHECK(tagged.config.nmosQueryAddress == "10.0.0.5");
    CHECK(tagged.config.nmosQueryPort == 4008);
    CHECK(tagged.config.nmosTags.at("urn:x-srf:production").at(0) == "sport-sa");

    std::map<std::string, std::string> alias{{"HOST_ID", "10.9.9.9"}};
    CHECK(loadConfig(alias, {}).config.nmosHostAddress == "10.9.9.9");
    alias["NMOS_HOST_ADDRESS"] = "10.9.9.8";
    CHECK(loadConfig(alias, {}).config.nmosHostAddress == "10.9.9.8");

    std::map<std::string, std::string> state{{"REPLAY_INPUTS", "2"}, {"NMOS_HOST_ADDRESS", "10.1.1.1"}};
    std::map<std::string, std::string> file{{"REPLAY_INPUTS", "3"}};
    std::map<std::string, std::string> over{{"REPLAY_INPUTS", "4"}, {"NMOS_HOST_ADDRESS", "10.1.1.1"}};
    CHECK(loadConfig(over, file, state).config.inputs == 4);
    CHECK(loadConfig({}, file, state).config.inputs == 3);

    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "replay.local"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "127.0.0.1"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_TAGS", "[]"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"WEB_PORT", "nope"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}), ConfigError);
    CHECK(loadConfig({{"REPLAY_INPUT_STALL_S", "0.5"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}).config.inputStallS == 0.5);
    CHECK_THROWS_AS(loadConfig({{"REPLAY_INPUT_STALL_S", "0"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}), ConfigError);
}

TEST_CASE("output domain is created once and only removed when the id matches")
{
    auto const root = std::filesystem::temp_directory_path() / "mxl-replay-domain";
    std::filesystem::remove_all(root);
    auto const dir = root / "replay-a";
    auto const created = ensureOutputDomain(dir.string(), "11111111-1111-1111-1111-111111111111", 42);
    CHECK(created.status == DomainStatus::Ready);
    auto const def = std::filesystem::path(dir) / "domain_def.json";
    auto const options = std::filesystem::path(dir) / "options.json";
    auto const beforeDef = std::filesystem::last_write_time(def);
    auto const beforeOptions = std::filesystem::file_size(options);
    auto const again = ensureOutputDomain(dir.string(), "11111111-1111-1111-1111-111111111111", 99);
    CHECK(again.status == DomainStatus::Ready);
    CHECK(std::filesystem::file_size(options) == beforeOptions);
    CHECK(std::filesystem::last_write_time(def) == beforeDef);
    auto const mismatch = ensureOutputDomain(dir.string(), "22222222-2222-2222-2222-222222222222", 42);
    CHECK(mismatch.status == DomainStatus::Mismatch);
    std::ifstream in(def);
    std::string body;
    std::getline(in, body);
    CHECK(body.find("11111111-1111-1111-1111-111111111111") != std::string::npos);
    // BCP-007-03 schema: id, label, description and tags are required.
    CHECK(body.find("\"label\":\"") != std::string::npos);
    CHECK(body.find("\"description\":\"") != std::string::npos);
    CHECK(body.find("\"tags\":{}") != std::string::npos);
    auto const other = root / "other";
    ensureOutputDomain(other.string(), "33333333-3333-3333-3333-333333333333", 1);
    std::string error;
    CHECK_FALSE(removeOwnDomain(other.string(), "11111111-1111-1111-1111-111111111111", error));
    CHECK(std::filesystem::exists(other));
    CHECK(removeOwnDomain(dir.string(), "11111111-1111-1111-1111-111111111111", error));
    CHECK_FALSE(std::filesystem::exists(dir));
    std::filesystem::remove_all(root);
}

TEST_CASE("config export is json and import restores a clip")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-import";
    std::filesystem::remove_all(dir);
    auto const loaded = loadConfig(
        {{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
            {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()}, {"REPLAY_STORAGE_MIN_MBPS", "0"},
            {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
        {});
    Engine engine(loaded.config, loaded.flat);
    engine.openBuffer();
    auto const exported = engine.exportConfigJson();
    CHECK(exported.find("\"secrets\":false") != std::string::npos);
    CHECK(exported.find("10.4.4.4") != std::string::npos);
    CHECK(exported.find("password") == std::string::npos);
    auto const imported = engine.importConfigJson(
        "{\"settings\":{\"CH1_LABEL\":\"PVW\",\"CH1_MOTION\":\"repeat\"},\"clips\":[{\"id\":\"c9\",\"name\":\"goal\",\"camera\":1,\"in_ns\":1,\"out_ns\":2,"
        "\"speed\":0.5,\"motion\":\"blend\",\"audio\":\"mute\",\"colour\":\"#fff\",\"tags\":\"\",\"end\":\"freeze\",\"library\":true,\"group\":\"\"}]}");
    CHECK(imported.ok);
    CHECK(engine.config().channelList[0].label == "PVW");
    CHECK(engine.config().channelList[0].motion == MotionMode::Repeat);
    CHECK(engine.clips().size() == 1);
    CHECK(engine.clips()[0].name == "goal");
    CHECK(std::filesystem::exists(dir / "state" / "config.json"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("a busy web port exits as a bind failure")
{
    HttpServer first;
    first.start(0, [](HttpRequest const&) { return HttpResponse{}; });
    HttpServer second;
    CHECK_THROWS_AS(second.start(first.port(), [](HttpRequest const&) { return HttpResponse{}; }), StartupError);
    first.stop();
}

TEST_CASE("query api readiness uses the node id")
{
    HttpServer query;
    std::string nodeId;
    query.start(0, [&](HttpRequest const& request) {
        HttpResponse response;
        response.status = request.path.find(nodeId) != std::string::npos ? 200 : 404;
        response.body = response.status == 200 ? "{\"id\":\"node\"}" : "{}";
        response.contentType = "application/json";
        return response;
    });
    nodeId = makeNmosIds("ready-replay").node;
    CHECK(httpGetStatus("127.0.0.1", query.port(), "/x-nmos/query/v1.3/nodes/" + nodeId, 500) == 200);
    CHECK(httpGetStatus("127.0.0.1", query.port(), "/x-nmos/query/v1.3/nodes/missing", 500) == 404);
    query.stop();
}

TEST_CASE("disk ring keeps a range that a second clip still protects")
{
    auto const dir = freshDir("mxl-replay-ring-twice");
    FrameRing ring(dir.string(), 3600 * kSecond, 1);
    CHECK(ring.push(frameAt(kSecond, 1)));
    CHECK(ring.push(frameAt(2 * kSecond, 2)));
    ring.protect(kSecond, 2 * kSecond);
    ring.protect(kSecond, 2 * kSecond);
    ring.unprotect(kSecond, 2 * kSecond);
    CHECK(ring.isProtected(kSecond));
    ring.unprotect(2 * kSecond, kSecond);
    CHECK_FALSE(ring.isProtected(kSecond));
    CHECK(ring.oldestNs() == kSecond);
    CHECK(ring.nearestNs(kSecond + kSecond / 3) == kSecond);
    CHECK(ring.nearestNs(2 * kSecond - kSecond / 3) == 2 * kSecond);
    std::filesystem::remove_all(dir);
}

TEST_CASE("json strings escape control characters")
{
    CHECK(jsonEscape("a\tb\"c") == "a\\u0009b\\\"c");
}

namespace
{
HttpResponse call(Engine& engine, std::string method, std::string path, std::string body = {}, std::string query = {})
{
    HttpRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.body = std::move(body);
    request.query = std::move(query);
    return handleApi(engine, request, "");
}

std::string idOf(std::string const& body)
{
    auto const start = body.find("\"id\":\"") + 6;
    return body.substr(start, body.find('"', start) - start);
}

// One camera with frames 0 … count-1 on the 50p grid (TAI 1000 s onward), one channel.
// `cap`: REPLAY_PROTECT_MAX_PCT.
std::unique_ptr<Engine> recordedEngine(std::filesystem::path const& dir, int count, std::string const& cap = "100")
{
    auto const loaded = loadConfig(
        {{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
            {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()}, {"REPLAY_STORAGE_MIN_MBPS", "0"},
            {"REPLAY_PROTECT_MAX_PCT", cap}, {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
        {});
    auto engine = std::make_unique<Engine>(loaded.config, loaded.flat);
    engine->openBuffer();
    for (int i = 0; i < count; ++i)
    {
        Frame10 frame;
        frame.allocate(64, 32);
        frame.fill(static_cast<std::uint16_t>(100 + i * 20), 512, 512);
        engine->ingestVideo(1, 1, 1000 * kSecond + static_cast<std::uint64_t>(i) * 20000000ull, std::move(frame));
    }
    return engine;
}

std::uint64_t frameTime(int i)
{
    return 1000 * kSecond + static_cast<std::uint64_t>(i) * 20000000ull;
}
} // namespace

TEST_CASE("api checks request bodies instead of failing on them")
{
    auto const dir = freshDir("mxl-replay-api-checks");
    auto engine = recordedEngine(dir, 4);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/speed", "{}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/speed", "{\"speed\":\"fast\"}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/speed", "{\"speed\":3}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/speed", "{\"speed\":\"0.5\"}").status == 200);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/speed", "{\"speed\":0.5").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/2/transport", "{\"command\":\"play\"}").status == 404);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/transport", "{\"command\":\"rewind\"}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/marks", "{\"which\":\"middle\"}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/position", "{\"frames\":\"x\"}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/position", "{}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/angle", "{\"camera\":2}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/mode", "{\"motion\":\"warp\",\"audio\":\"stretch\"}").status == 400);
    CHECK(engine->config().channelList[0].audio == AudioMode::Mute);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/mode", "{\"audio\":\"stretch\",\"timecode\":\"output\"}").status == 200);
    CHECK(engine->config().channelList[0].audio == AudioMode::Stretch);
    CHECK(call(*engine, "POST", "/api/v1/channels/1/lock", "{}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/control", "{\"channel\":1,\"action\":\"jump\"}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/control", "{\"channel\":\"one\",\"action\":\"play\"}").status == 400);
    CHECK(call(*engine, "POST", "/api/v1/clips", "{\"channel\":1}").status == 400); // no marks yet
    CHECK(call(*engine, "POST", "/api/v1/shotbox/nothing", "{}").status == 404);
    CHECK(call(*engine, "POST", "/api/v1/uploads", "").status == 400);
    CHECK(call(*engine, "GET", "/api/v1/cameras/2/preview.jpg").status == 404);

    // A handler that throws answers 500 and the server keeps running.
    HttpServer server;
    server.start(0, [](HttpRequest const& request) -> HttpResponse {
        if (request.path == "/boom")
        {
            throw std::runtime_error("boom");
        }
        return HttpResponse{};
    });
    CHECK(httpGetStatus("127.0.0.1", server.port(), "/boom", 1000) == 500);
    CHECK(httpGetStatus("127.0.0.1", server.port(), "/", 1000) == 200);
    server.stop();
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("api edits clips and playlists and serves previews")
{
    auto const dir = freshDir("mxl-replay-api-edit");
    auto engine = recordedEngine(dir, 12);
    engine->setPosition(1, frameTime(2));
    engine->markIn(1);
    engine->setPosition(1, frameTime(6));
    engine->markOut(1);
    // Set while paused: the clip keeps the speed that was set, not the one the ramp reached.
    engine->setSpeed(1, 0.5);
    auto const created = call(*engine, "POST", "/api/v1/clips", R"({"channel":1,"name":"say \"hi\"\tnow","colour":"#123456","tags":"goal"})");
    REQUIRE(created.status == 201);
    auto const id = idOf(created.body);
    auto clip = engine->clip(id);
    CHECK(clip.name == "say \"hi\"\tnow");
    CHECK(clip.colour == "#123456");
    CHECK(clip.tags == "goal");
    CHECK(clip.speed == doctest::Approx(0.5));
    CHECK(engine->statusJson().find("\"target_speed\":0.5") != std::string::npos);
    auto const list = call(*engine, "GET", "/api/v1/clips").body;
    CHECK(list.find(R"(say \"hi\"\u0009now)") != std::string::npos);
    CHECK(list.find("\"in_tc\":\"") != std::string::npos);

    // IN lands on the nearest recorded frame; the other fields change as given.
    auto const patched = call(*engine, "PATCH", "/api/v1/clips/" + id,
        "{\"name\":\"goal\",\"in_ns\":" + std::to_string(frameTime(3) + 4000000) + ",\"speed\":0.5,\"end\":\"loop\",\"motion\":\"blend\"}");
    CHECK(patched.status == 200);
    clip = engine->clip(id);
    CHECK(clip.name == "goal");
    CHECK(clip.inNs == frameTime(3));
    CHECK(clip.outNs == frameTime(6));
    CHECK(clip.speed == doctest::Approx(0.5));
    CHECK(clip.end == EndAction::Loop);
    CHECK(clip.motion == "blend");
    CHECK(call(*engine, "PATCH", "/api/v1/clips/" + id, "{\"end\":\"sideways\"}").status == 400);
    CHECK(call(*engine, "PATCH", "/api/v1/clips/" + id, "{\"out_ns\":" + std::to_string(frameTime(1)) + "}").status == 400);
    CHECK(call(*engine, "PATCH", "/api/v1/clips/clip-999", "{\"name\":\"x\"}").status == 404);

    // Previews: the channel output, the camera's newest frame, the clip's IN frame.
    (void)engine->render(1, frameTime(12));
    auto const preview = call(*engine, "GET", "/api/v1/channels/1/preview.jpg");
    CHECK(preview.status == 200);
    CHECK(preview.contentType == "image/jpeg");
    CHECK(preview.body.rfind("\xff\xd8", 0) == 0);
    CHECK(call(*engine, "GET", "/api/v1/cameras/1/preview.jpg").body.rfind("\xff\xd8", 0) == 0);
    auto const thumbnail = call(*engine, "GET", "/api/v1/clips/" + id + "/thumbnail.jpg");
    CHECK(thumbnail.status == 200);
    CHECK(thumbnail.cacheControl == "max-age=3600");

    // Playlists: clips as an array, the entries, a replacement, delete.
    auto const playlist = call(*engine, "POST", "/api/v1/playlists", "{\"name\":\"best\",\"clips\":[\"" + id + "\"]}");
    REQUIRE(playlist.status == 201);
    auto const pid = idOf(playlist.body);
    CHECK(call(*engine, "GET", "/api/v1/playlists/" + pid).body.find("\"clip_id\":\"" + id + "\"") != std::string::npos);
    CHECK(call(*engine, "GET", "/api/v1/playlists").body.find("\"entries\":1") != std::string::npos);
    CHECK(call(*engine, "POST", "/api/v1/playlists", "{\"clips\":[\"clip-999\"]}").status == 400);
    auto const replaced = call(*engine, "PUT", "/api/v1/playlists/" + pid,
        "{\"name\":\"better\",\"entries\":[{\"clip_id\":\"" + id + "\",\"speed\":0.25,\"end\":\"freeze\",\"auto_advance\":false}]}");
    CHECK(replaced.status == 200);
    auto const stored = engine->playlist(pid);
    CHECK(stored.name == "better");
    REQUIRE(stored.entries.size() == 1);
    CHECK(stored.entries[0].speed == doctest::Approx(0.25));
    CHECK(stored.entries[0].end == EndAction::Freeze);
    CHECK_FALSE(stored.entries[0].autoAdvance);
    CHECK(call(*engine, "POST", "/api/v1/playlists/" + pid + "/play", "{\"channel\":1}").status == 200);
    CHECK(call(*engine, "DELETE", "/api/v1/playlists/" + pid).status == 200);
    CHECK(call(*engine, "DELETE", "/api/v1/playlists/" + pid).status == 404);

    // An upload takes its name from the query.
    Frame10 still;
    still.allocate(64, 32);
    still.fill(700, 512, 512);
    auto const jpeg = encodeJpeg422(still, 90);
    auto const uploaded = call(*engine, "POST", "/api/v1/uploads", std::string(jpeg.begin(), jpeg.end()), "name=My%20still");
    REQUIRE(uploaded.status == 201);
    CHECK(engine->clip(idOf(uploaded.body)).name == "My still");
    // Live after an upload follows a camera again.
    engine->playClip(1, idOf(uploaded.body));
    CHECK(engine->render(1, frameTime(13)).camera == kLibraryCamera);
    engine->live(1);
    CHECK(engine->render(1, frameTime(14)).camera == 1);
    CHECK(call(*engine, "DELETE", "/api/v1/clips/" + id).status == 200);
    CHECK(call(*engine, "DELETE", "/api/v1/clips/" + id).status == 404);

    auto const status = engine->statusJson();
    CHECK(status.find("\"newest_ns\":" + std::to_string(frameTime(11))) != std::string::npos);
    CHECK(status.find("\"in_tc\":\"") != std::string::npos);
    CHECK(status.find("\"tc_mode\":\"source\"") != std::string::npos);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("playlist entries bring their speed, end action and auto-advance; a paused shot resumes")
{
    auto const dir = freshDir("mxl-replay-playlist-play");
    auto engine = recordedEngine(dir, 20);
    auto const clipFrom = [&](int in, int out) {
        engine->setPosition(1, frameTime(in));
        engine->markIn(1);
        engine->setPosition(1, frameTime(out));
        engine->markOut(1);
        std::string error;
        return engine->createClip(1, "", false, false, error);
    };
    auto const first = clipFrom(0, 4);
    auto const second = clipFrom(10, 14);
    auto const id = engine->createPlaylist("two", {PlaylistEntry{first, 0.5, EndAction::Next, false}, PlaylistEntry{second, 1.0, EndAction::Freeze, true}});
    engine->playPlaylist(1, id);
    CHECK(engine->render(1, 0).speed == doctest::Approx(0.5));
    // Four frames at half speed are eight output frames; then the second entry is cued, not played.
    for (int i = 0; i < 10; ++i)
    {
        (void)engine->render(1, 0);
    }
    auto status = engine->statusJson();
    CHECK(status.find("\"shot\":\"cued\"") != std::string::npos);
    CHECK(status.find("\"clip\":\"" + second + "\"") != std::string::npos);
    CHECK(status.find("\"playing\":false") != std::string::npos);
    CHECK(engine->render(1, 0).positionNs == frameTime(10));
    engine->play(1);
    for (int i = 0; i < 8; ++i)
    {
        (void)engine->render(1, 0);
    }
    status = engine->statusJson();
    CHECK(status.find("\"shot\":\"ended\"") != std::string::npos);
    CHECK(status.find("\"playing\":false") != std::string::npos);

    // Shotbox: cue, play, pause, and the next click resumes where it paused.
    CHECK(engine->shotClick(1, first) == ShotState::Cued);
    CHECK(engine->render(1, 0).positionNs == frameTime(0));
    CHECK(engine->shotClick(1, first) == ShotState::Playing);
    (void)engine->render(1, 0);
    (void)engine->render(1, 0);
    CHECK(engine->shotClick(1, first) == ShotState::Paused);
    auto const paused = engine->render(1, 0).positionNs;
    CHECK(paused > frameTime(0));
    CHECK(engine->shotClick(1, first) == ShotState::Playing);
    CHECK(engine->render(1, 0).positionNs > paused);
    // A playlist is a shotbox button too.
    CHECK(engine->shotClick(1, id) == ShotState::Cued);
    CHECK(engine->statusJson().find("\"playlist\":\"" + id + "\"") != std::string::npos);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("clip edits stay on recorded frames, under the protection cap and forward")
{
    auto const dir = freshDir("mxl-replay-edit-checks");
    auto engine = recordedEngine(dir, 40, "1");
    engine->setPosition(1, frameTime(2));
    engine->markIn(1);
    // An OUT between frames, as a mark in slow motion gives.
    engine->setPosition(1, frameTime(6) + 12000000);
    engine->markOut(1);
    std::string error;
    auto const id = engine->createClip(1, "edit", false, false, error);
    REQUIRE(error.empty());
    auto const path = "/api/v1/clips/" + id;
    // IN lands on frame 7, after the unchanged OUT.
    CHECK(call(*engine, "PATCH", path, "{\"in_ns\":" + std::to_string(frameTime(6) + 11000000) + "}").status == 400);
    CHECK(call(*engine, "PATCH", path, "{\"in_ns\":0}").status == 400);
    CHECK(call(*engine, "PATCH", path, "{\"out_ns\":" + std::to_string(frameTime(60)) + "}").status == 400);
    CHECK(call(*engine, "PATCH", path, "{\"speed\":0}").status == 400);
    CHECK(call(*engine, "PATCH", path, "{\"speed\":-0.5}").status == 400);
    CHECK(call(*engine, "PATCH", path, "{\"speed\":\"\"}").status == 400);
    CHECK(engine->clip(id).inNs == frameTime(2));
    // The clip keeps 1 % or more of this small budget: a longer range needs force, a shorter one not.
    CHECK(call(*engine, "PATCH", path, "{\"out_ns\":" + std::to_string(frameTime(9)) + "}").status == 409);
    CHECK(call(*engine, "PATCH", path, "{\"out_ns\":" + std::to_string(frameTime(9)) + ",\"force\":true}").status == 200);
    CHECK(engine->clip(id).outNs == frameTime(9));
    CHECK(call(*engine, "PATCH", path, "{\"in_ns\":" + std::to_string(frameTime(4)) + "}").status == 200);
    CHECK(call(*engine, "POST", "/api/v1/playlists", "{\"entries\":[{\"clip_id\":\"" + id + "\",\"speed\":0}]}").status == 400);
    // A thumbnail needs the IN frame: an imported clip far outside the buffer has none.
    REQUIRE(engine->importConfigJson("{\"clips\":[{\"id\":\"far\",\"name\":\"far\",\"camera\":1,\"in_ns\":5,\"out_ns\":9}]}").ok);
    CHECK(call(*engine, "GET", "/api/v1/clips/far/thumbnail.jpg").status == 404);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("ranges: a consolidated clip releases once, an import protects at once, a name round-trips")
{
    auto const dir = freshDir("mxl-replay-ranges");
    auto engine = recordedEngine(dir, 20);
    engine->setPosition(1, frameTime(2));
    engine->markIn(1);
    engine->setPosition(1, frameTime(6));
    engine->markOut(1);
    std::string error;
    auto const first = engine->createClip(1, "a \"b\"\tc", false, false, error);
    auto const second = engine->createClip(1, "same range", false, false, error);
    auto const keeps = [&] { return engine->statusJson().find("\"protected_bytes\":0,") == std::string::npos; };
    REQUIRE(engine->consolidate(first, error));
    REQUIRE(engine->consolidate(first, error));
    CHECK(engine->deleteClip(first));
    CHECK(keeps()); // the second clip's range is still protected
    auto const exported = engine->exportConfigJson();
    CHECK(engine->deleteClip(second));
    CHECK_FALSE(keeps());
    // Import brings the clip back with its name and protects its range without a restart.
    REQUIRE(engine->importConfigJson(exported).ok);
    CHECK(engine->clip(second).name == "same range");
    CHECK(keeps());
    REQUIRE(engine->importConfigJson(R"({"clips":[{"id":"q","name":"a \"b\"\tc","camera":1,"in_ns":1,"out_ns":2}]})").ok);
    CHECK(engine->clip("q").name == "a \"b\"\tc");
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("a playlist keeps deleted clips as skipped entries and a playing channel on its entry")
{
    auto const dir = freshDir("mxl-replay-playlist-edit");
    auto engine = recordedEngine(dir, 20);
    auto const clipFrom = [&](int in, int out) {
        engine->setPosition(1, frameTime(in));
        engine->markIn(1);
        engine->setPosition(1, frameTime(out));
        engine->markOut(1);
        std::string error;
        return engine->createClip(1, "", false, false, error);
    };
    auto const first = clipFrom(0, 4);
    auto const second = clipFrom(10, 14);
    auto const gone = clipFrom(15, 16);
    auto const id = engine->createPlaylist("p", {PlaylistEntry{first, 1, EndAction::Next, true}, PlaylistEntry{second, 1, EndAction::Next, true},
                                                    PlaylistEntry{gone, 1, EndAction::Next, true}});
    CHECK(engine->deleteClip(gone));
    auto const entries = [&](std::vector<std::string> const& ids) {
        std::string out;
        for (auto const& clip : ids)
        {
            out += (out.empty() ? "" : ",") + std::string("{\"clip_id\":\"") + clip + "\"}";
        }
        return "{\"name\":\"renamed\",\"entries\":[" + out + "]}";
    };
    CHECK(call(*engine, "PUT", "/api/v1/playlists/" + id, entries({first, second, gone})).status == 200);
    CHECK(call(*engine, "PUT", "/api/v1/playlists/" + id, entries({first, "clip-999"})).status == 400);
    // Play into the second entry, then move it to the front: the channel stays on it.
    engine->playPlaylist(1, id);
    for (int i = 0; i < 6; ++i)
    {
        (void)engine->render(1, 0);
    }
    REQUIRE(engine->statusJson().find("\"playlist_index\":1") != std::string::npos);
    CHECK(call(*engine, "PUT", "/api/v1/playlists/" + id, entries({second, first})).status == 200);
    CHECK(engine->statusJson().find("\"playlist_index\":0") != std::string::npos);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("return to live at the end of an upload follows a camera")
{
    auto const dir = freshDir("mxl-replay-upload-live");
    auto engine = recordedEngine(dir, 6);
    Frame10 still;
    still.allocate(64, 32);
    still.fill(700, 512, 512);
    auto const jpeg = encodeJpeg422(still, 90);
    std::string error;
    auto const id = engine->upload(jpeg.data(), jpeg.size(), "still", error);
    REQUIRE_FALSE(id.empty());
    REQUIRE(call(*engine, "PATCH", "/api/v1/clips/" + id, "{\"end\":\"return-to-live\"}").status == 200);
    engine->playClip(1, id);
    (void)engine->render(1, frameTime(8));
    CHECK(engine->render(1, frameTime(9)).camera == 1);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("preview decode is scaled down by libjpeg")
{
    Frame10 frame;
    frame.allocate(64, 32);
    frame.fill(500, 512, 512);
    auto const jpeg = encodeJpeg422(frame, 90);
    Frame10 small;
    REQUIRE(decodeJpegPreview(jpeg.data(), jpeg.size(), 16, small));
    CHECK(small.width == 16);
    CHECK(small.height == 8);
    CHECK(std::abs(static_cast<int>(small.y[0]) - 500) < 16);
    REQUIRE(decodeJpegPreview(jpeg.data(), jpeg.size(), 64, small));
    CHECK(small.width == 64);
}

TEST_CASE("a websocket client that does not read is dropped, not waited for")
{
    HttpServer server;
    server.start(0, [](HttpRequest const& request) {
        HttpResponse response;
        response.websocket = request.path == "/ws";
        return response;
    });
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    int const small = 4096;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(server.port()));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    std::string const upgrade = "GET /ws HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    REQUIRE(::send(fd, upgrade.data(), upgrade.size(), 0) == static_cast<ssize_t>(upgrade.size()));
    char answer[256];
    auto const n = ::recv(fd, answer, sizeof(answer), 0);
    REQUIRE(n > 0);
    CHECK(std::string(answer, static_cast<std::size_t>(n)).rfind("HTTP/1.1 101", 0) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // 10 MB to a client that reads nothing: a blocking send would never return.
    std::string const message(256 * 1024, 'x');
    auto const started = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i)
    {
        server.broadcast(message);
    }
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    CHECK(httpGetStatus("127.0.0.1", server.port(), "/", 1000) == 200);
    ::close(fd);
    server.stop();
}
