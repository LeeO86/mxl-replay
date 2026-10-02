#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "flow/dis.hpp"
#include "library/catalog.hpp"
#include "media/anc.hpp"
#include "media/audio.hpp"
#include "media/jpeg.hpp"
#include "media/timebase.hpp"
#include "media/v210.hpp"
#include "nmos/ids.hpp"
#include "ops/httpserver.hpp"
#include "ops/api.hpp"
#include "ops/metrics.hpp"
#include "playout/scheduler.hpp"
#include "playout/shotbox.hpp"
#include "record/hfr.hpp"
#include "record/ring.hpp"
#include "util/uuid.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>

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

TEST_CASE("ring buffer skips protected frames")
{
    FrameRing ring(4);
    for (int i = 0; i < 4; ++i)
    {
        StoredFrame frame;
        frame.taiNs = static_cast<std::uint64_t>(i);
        frame.jpeg = {static_cast<std::uint8_t>(i)};
        CHECK(ring.push(frame));
    }
    ring.protect(1, 2);
    for (int i = 4; i < 8; ++i)
    {
        StoredFrame frame;
        frame.taiNs = static_cast<std::uint64_t>(i);
        frame.jpeg = {static_cast<std::uint8_t>(i)};
        CHECK(ring.push(frame));
    }
    CHECK(ring.findNearest(1)->jpeg[0] == 1);
    CHECK(ring.findNearest(2)->jpeg[0] == 2);
    CHECK(ring.protectedCount() == 2);
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

TEST_CASE("jpeg 4:2:2 round trip and v210 pack")
{
    CHECK(jpegStorageBitDepth() == 8);
    CHECK_FALSE(jpegSupports10Bit());
    CHECK_FALSE(jpegSupports12Bit());
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
        {"REPLAY_ALLOW_CPU_INTERP", "true"},
        {"REPLAY_STORAGE_MIN_MBPS", "0"},
        {"REPLAY_PROTECT_MAX_PCT", "90"},
        {"HOST_ID", "test-host"},
    };
    Engine engine(loadConfig(env, {}).config);
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

TEST_CASE("metrics render the replay prefix")
{
    Metrics metrics;
    metrics.inc("record_dropped_total", {{"camera", "1"}});
    metrics.observe("frame_gpu_seconds", {{"stage", "decode"}}, 0.004);
    auto const text = metrics.render();
    CHECK(text.find("mxl_replay_record_dropped_total") != std::string::npos);
    CHECK(text.find("mxl_replay_frame_gpu_seconds_bucket") != std::string::npos);
}
