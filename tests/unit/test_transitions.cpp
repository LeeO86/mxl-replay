// Playlist transitions, the audio crossfade, the aux track and audio items (SPECIFICATION.md §5.4 and §7, 1.7.0).
#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "library/catalog.hpp"
#include "media/audio.hpp"
#include "media/convert.hpp"
#include "media/v210.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "playout/transition.hpp"
#include "util/json.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <filesystem>
#include <memory>
#include <numbers>

using namespace replay;

namespace
{
constexpr std::uint64_t kPeriod = 20000000ull;
constexpr std::uint64_t kBase = 1000 * kPeriod;

std::uint64_t at(int frame)
{
    return kBase + static_cast<std::uint64_t>(frame) * kPeriod;
}

HttpResponse request(Engine& engine, std::string method, std::string path, std::string body = {}, std::string query = {})
{
    HttpRequest req;
    req.method = std::move(method);
    req.path = std::move(path);
    req.body = std::move(body);
    req.query = std::move(query);
    return handleApi(engine, req, "");
}

// One camera, 64×32p50, `CH1_MOTION=repeat`: frames 0–29 grey 200 with audio at 0.5, frames 30–59 grey 800 with
// audio at −0.25 (both channels).
std::unique_ptr<Engine> twoToneEngine(std::filesystem::path const& dir)
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
        bool const first = i < 30;
        engine->ingestAudio(1, at(i), std::vector<float>(960 * 2, first ? 0.5F : -0.25F), 2);
        Frame10 frame;
        frame.allocate(64, 32);
        frame.fill(first ? 200 : 800, 512, 512);
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

// The luma of the first pixel of a v210 grain.
int lumaOf(RenderedFrame const& frame)
{
    REQUIRE(frame.v210.size() >= 16);
    std::uint32_t word = 0;
    std::memcpy(&word, frame.v210.data(), 4);
    return static_cast<int>((word >> 10) & 0x3ffu);
}
} // namespace

TEST_CASE("mix weights run (k+1)/(n+1) and the CPU blend is (a·(1024−w) + b·w) >> 10")
{
    CHECK(mixWeight(0, 25) == doctest::Approx(1.0 / 26));
    CHECK(mixWeight(12, 25) == doctest::Approx(0.5));
    CHECK(mixWeight(24, 25) == doctest::Approx(25.0 / 26));
    CHECK(mixWeight(25, 25) == 1.0);
    CHECK(mixWeight(-1, 25) == 0.0);
    CHECK(mixWeight(0, 0) == 1.0);
    // Symmetric: grain k of the incoming is grain n-1-k of the outgoing.
    for (int k = 0; k < 25; ++k)
    {
        CHECK(mixWeight(k, 25) + mixWeight(24 - k, 25) == doctest::Approx(1.0));
    }
    CHECK(blendWeight1024(0.5) == 512);
    CHECK(blendWeight1024(1.0 / 26) == 39);
    CHECK(blendSample(200, 800, 512) == 500);
    CHECK(blendSample(200, 800, 0) == 200);
    CHECK(blendSample(200, 800, 1024) == 800);
    CHECK(blendSample(1023, 0, 39) == 984);
}

TEST_CASE("transition plans: cut with its 2-frame audio crossfade, mix with its default, audio frames of the video's or their own")
{
    PlaylistEntry entry;
    // A cut crossfades the audio over 2 frames by default; 0 is a hard audio cut.
    CHECK(transitionPlan(entry).videoFrames == 0);
    CHECK(transitionPlan(entry).audioFrames == kDefaultCutAudioFrames);
    CHECK(transitionPlan(entry).length() == 2);
    entry.audioFrames = 0;
    CHECK(transitionPlan(entry).length() == 0);
    entry.audioFrames = -1;
    entry.transition = Transition::Mix;
    CHECK(transitionPlan(entry).videoFrames == kDefaultMixFrames);
    entry.transitionFrames = 10;
    CHECK(transitionPlan(entry).videoFrames == 10);
    CHECK(transitionPlan(entry).audioFrames == 10);
    entry.audioFrames = 0;
    CHECK(transitionPlan(entry).audioFrames == 0);
    CHECK(transitionPlan(entry).length() == 10);
    entry.audioFrames = 30;
    CHECK(transitionPlan(entry).length() == 30);
    // An audio crossfade on a cut: the entries overlap by its length, the picture cuts at its start.
    PlaylistEntry cut;
    cut.audioFrames = 5;
    CHECK(transitionPlan(cut).videoFrames == 0);
    CHECK(transitionPlan(cut).length() == 5);
}

TEST_CASE("the equal-power crossfade keeps the power, ends exactly and has no step")
{
    std::int64_t const total = 25 * 960;
    CHECK(crossfadeGains(-1, total).out == 1.F);
    CHECK(crossfadeGains(-1, total).in == 0.F);
    CHECK(crossfadeGains(total, total).out == 0.F);
    CHECK(crossfadeGains(total, total).in == 1.F);
    float maxStep = 0;
    auto previous = crossfadeGains(-1, total);
    for (std::int64_t n = 0; n <= total; ++n)
    {
        auto const g = crossfadeGains(n, total);
        CHECK(g.out * g.out + g.in * g.in == doctest::Approx(1.0).epsilon(1e-6));
        // Per sample: the reference cos/sin of π/2·(n+0.5)/total.
        if (n < total)
        {
            double const angle = (static_cast<double>(n) + 0.5) / static_cast<double>(total) * std::numbers::pi / 2;
            CHECK(g.out == static_cast<float>(std::cos(angle)));
            CHECK(g.in == static_cast<float>(std::sin(angle)));
        }
        maxStep = std::max({maxStep, std::fabs(g.out - previous.out), std::fabs(g.in - previous.in)});
        previous = g;
    }
    // The largest gain step is the slope π/(2·total) per sample, 6.5e-5 for 25 frames; the first and last are half
    // that (no step into or out of the crossfade): no click.
    CHECK(maxStep <= static_cast<float>(std::numbers::pi / (2.0 * static_cast<double>(total)) * 1.001));
    CHECK(crossfadeGains(0, total).in == doctest::Approx(std::numbers::pi / (4.0 * static_cast<double>(total))).epsilon(1e-3));
}

TEST_CASE("the duck ramps in dB over attack and release; end fade and edges")
{
    DuckRamp duck;
    double gain = 1;
    for (int i = 0; i < 4800; ++i)
    {
        gain = duck.next(true, -12, 4800, 9600);
    }
    CHECK(duck.db == doctest::Approx(-12));
    CHECK(gain == doctest::Approx(std::pow(10.0, -12.0 / 20)));
    for (int i = 0; i < 4800; ++i)
    {
        gain = duck.next(false, -12, 4800, 9600);
    }
    CHECK(duck.db == doctest::Approx(-6));
    for (int i = 0; i < 4800; ++i)
    {
        gain = duck.next(false, -12, 4800, 9600);
    }
    CHECK(duck.db == doctest::Approx(0));
    CHECK(gain == doctest::Approx(1));
    // No depth: no duck; no attack: at once.
    DuckRamp none;
    CHECK(none.next(true, 0, 4800, 4800) == 1.0);
    DuckRamp hard;
    CHECK(hard.next(true, -20, 0, 0) == doctest::Approx(0.1));

    CHECK(endFadeGain(-1, 1000) == 1.0);
    CHECK(endFadeGain(2000, 1000) == 1.0);
    CHECK(endFadeGain(500, 1000) == doctest::Approx(0.5));
    CHECK(endFadeGain(0, 1000) == 0.0);
    CHECK(endFadeGain(500, 0) == 1.0);

    CHECK(edgeGain(0, 48000) == doctest::Approx(0.5 / kAuxEdgeSamples));
    CHECK(edgeGain(kAuxEdgeSamples, 48000) == 1.0);
    CHECK(edgeGain(47999, 48000) == doctest::Approx(0.5 / kAuxEdgeSamples));
    CHECK(edgeGain(48000, 48000) == 0.0);
}

TEST_CASE("entry and aux JSON read and write the transition fields")
{
    Json item;
    REQUIRE(parseJson(R"({"clip_id":"c","transition":"mix","transition_frames":12,"audio_frames":6,"own_audio":false})", item));
    PlaylistEntry entry;
    std::string problem;
    REQUIRE(readEntryTransition(item, entry, problem));
    CHECK(entry.transition == Transition::Mix);
    CHECK(entry.transitionFrames == 12);
    CHECK(entry.audioFrames == 6);
    CHECK_FALSE(entry.ownAudio);
    CHECK(entryTransitionJson(entry).find("\"transition\":\"mix\",\"transition_frames\":12,\"audio_frames\":6") != std::string::npos);
    Json wipe;
    REQUIRE(parseJson(R"({"transition":"wipe"})", wipe));
    CHECK_FALSE(readEntryTransition(wipe, entry, problem));
    CHECK(problem == "transition must be cut or mix");
    Json tooLong;
    REQUIRE(parseJson(R"({"transition_frames":251})", tooLong));
    CHECK_FALSE(readEntryTransition(tooLong, entry, problem));
    // A mix without a length gets the default; a cut has none.
    PlaylistEntry fresh;
    Json mix;
    REQUIRE(parseJson(R"({"transition":"mix"})", mix));
    REQUIRE(readEntryTransition(mix, fresh, problem));
    CHECK(fresh.transitionFrames == kDefaultMixFrames);

    AuxTrack aux;
    Json auxItem;
    REQUIRE(parseJson(R"({"source":"audio-3","level_db":-6,"duck_db":-18})", auxItem));
    REQUIRE(readAux(auxItem, aux, problem));
    CHECK(aux.source == "audio-3");
    CHECK(aux.levelDb == -6);
    CHECK(aux.duckDb == -18);
    CHECK(aux.loop);
    CHECK(aux.releaseMs == 500);
    Json up;
    REQUIRE(parseJson(R"({"duck_db":3})", up));
    CHECK_FALSE(readAux(up, aux, problem));
    CHECK(auxJson(aux).find("\"source\":\"audio-3\",\"level_db\":-6") != std::string::npos);
}

TEST_CASE("a catalog from before transitions and aux tracks gets their columns with the defaults")
{
    auto const path = std::filesystem::temp_directory_path() / "mxl-replay-catalog-pre17.sqlite";
    std::filesystem::remove(path);
    {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
        CHECK(sqlite3_exec(db,
                  "CREATE TABLE playlists (id TEXT PRIMARY KEY, name TEXT, loop INTEGER NOT NULL DEFAULT 0);"
                  "INSERT INTO playlists VALUES ('p1','old',1);"
                  "CREATE TABLE playlist_entries (playlist TEXT, position INTEGER, clip_id TEXT, speed REAL, end_action TEXT, auto_advance INTEGER);"
                  "INSERT INTO playlist_entries VALUES ('p1',0,'clip-1',0.5,'next',1);",
                  nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    Catalog catalog;
    catalog.open(path.string());
    auto lists = catalog.playlists();
    REQUIRE(lists.size() == 1);
    CHECK(lists[0].loop);
    CHECK(lists[0].aux.source.empty());
    CHECK(lists[0].aux.duckDb == -12);
    REQUIRE(lists[0].entries.size() == 1);
    CHECK(lists[0].entries[0].transition == Transition::Cut);
    CHECK(lists[0].entries[0].audioFrames == -1);
    CHECK(lists[0].entries[0].ownAudio);
    // Stored and read back.
    lists[0].entries[0].transition = Transition::Mix;
    lists[0].entries[0].transitionFrames = 8;
    lists[0].entries[0].ownAudio = false;
    lists[0].aux.source = "audio-2";
    lists[0].aux.levelDb = -3;
    catalog.upsertPlaylist(lists[0]);
    Catalog::AudioItem item{"audio-2", "bed", 96000, 1};
    catalog.upsertAudioItem(item);
    catalog.close();
    catalog.open(path.string());
    lists = catalog.playlists();
    CHECK(lists[0].entries[0].transition == Transition::Mix);
    CHECK(lists[0].entries[0].transitionFrames == 8);
    CHECK_FALSE(lists[0].entries[0].ownAudio);
    CHECK(lists[0].aux.source == "audio-2");
    CHECK(lists[0].aux.levelDb == -3);
    CHECK(catalog.audioItem("audio-2").samples == 96000);
    catalog.close();
    std::filesystem::remove(path);
}

TEST_CASE("a mix blends the outgoing and incoming frames at the mix weights, bit-exact, with an equal-power audio crossfade")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-mix";
    auto engine = twoToneEngine(dir);
    auto const a = clipOf(*engine, 10, 20);
    auto const b = clipOf(*engine, 40, 50);
    PlaylistEntry first{a, 1, EndAction::Next, true};
    PlaylistEntry second{b, 1, EndAction::Freeze, true};
    second.transition = Transition::Mix;
    second.transitionFrames = 4;
    auto const list = engine->createPlaylist("mix", {first, second});
    engine->playPlaylist(1, list);
    std::uint64_t out = at(100);
    std::vector<int> luma;
    std::vector<float> left;
    for (int i = 0; i < 18; ++i)
    {
        auto const frame = engine->render(1, out);
        out += kPeriod;
        luma.push_back(lumaOf(frame));
        REQUIRE(frame.audio.size() == 960 * 2);
        for (std::size_t s = 0; s < frame.audio.size(); s += 2)
        {
            left.push_back(frame.audio[s]);
        }
    }
    // Pure pictures before and after (the JPEG round trip of each grey).
    int const ya = luma.front();
    int const yb = luma.back();
    CHECK(ya != yb);
    // The mix starts when clip A has 4 grains left: A shows frames 10…20 on grains 0…10 (the first step lands on
    // 11); grains of the mix k = 0…3, each weight (k+1)/5 in 1/1024 steps.
    auto const start = std::find_if(luma.begin(), luma.end(), [&](int y) { return y != ya; }) - luma.begin();
    REQUIRE(start + 4 < static_cast<long>(luma.size()));
    for (int k = 0; k < 4; ++k)
    {
        auto const expected = blendSample(static_cast<std::uint16_t>(ya), static_cast<std::uint16_t>(yb), blendWeight1024(mixWeight(k, 4)));
        CHECK(luma[static_cast<std::size_t>(start + k)] == expected);
    }
    CHECK(luma[static_cast<std::size_t>(start + 4)] == yb);
    // The overlap shortens the playlist: A has 11 grains with the first on IN, the mix replaces its last 4 (start
    // after 7 grains, the 7th being IN+7… the first grain renders IN+1).
    CHECK(start >= 5);
    CHECK(start <= 8);
    // Audio: 0.5 before, −0.25 after; across the 4-frame crossfade each sample is 0.5·cos + (−0.25)·sin.
    std::size_t const s0 = static_cast<std::size_t>(start) * 960;
    std::int64_t const total = 4 * 960;
    float maxError = 0;
    float maxStep = 0;
    for (std::int64_t n = 0; n < total; ++n)
    {
        auto const g = crossfadeGains(n, total);
        maxError = std::max(maxError, std::fabs(left[s0 + static_cast<std::size_t>(n)] - (0.5F * g.out - 0.25F * g.in)));
    }
    for (std::size_t i = 1; i < left.size(); ++i)
    {
        maxStep = std::max(maxStep, std::fabs(left[i] - left[i - 1]));
    }
    CHECK(maxError < 1e-6F);
    CHECK(left[s0 - 1] == doctest::Approx(0.5F));
    CHECK(left[s0 + static_cast<std::size_t>(total)] == doctest::Approx(-0.25F));
    // The largest step is the crossfade's: 0.75·π/(2·3840) ≈ 3e-4 per sample (a cut is a 0.75 step).
    CHECK(maxStep < 1e-3F);
    // The status names the transition while it runs and the metric counts it.
    CHECK(engine->metrics().render().find("mxl_replay_transitions_total{channel=\"1\",kind=\"mix\"} 1") != std::string::npos);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("an audio crossfade on a cut, a hard audio cut on a mix, and own audio off")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-xfade";
    auto engine = twoToneEngine(dir);
    auto const a = clipOf(*engine, 10, 20);
    auto const b = clipOf(*engine, 40, 50);
    auto const run = [&](PlaylistEntry const& first, PlaylistEntry const& second, std::vector<int>& luma) {
        auto const list = engine->createPlaylist("x", {first, second});
        engine->playPlaylist(1, list);
        std::vector<float> left;
        std::uint64_t out = at(200);
        for (int i = 0; i < 18; ++i)
        {
            auto const frame = engine->render(1, out);
            out += kPeriod;
            luma.push_back(lumaOf(frame));
            for (std::size_t s = 0; s < frame.audio.size(); s += 2)
            {
                left.push_back(frame.audio[s]);
            }
        }
        return left;
    };
    // A cut with a 6-frame audio crossfade: the picture cuts where the audio starts to cross.
    {
        PlaylistEntry first{a, 1, EndAction::Next, true};
        PlaylistEntry second{b, 1, EndAction::Freeze, true};
        second.audioFrames = 6;
        std::vector<int> luma;
        auto const left = run(first, second, luma);
        auto const cut = std::adjacent_find(luma.begin(), luma.end(), std::not_equal_to<>()) - luma.begin() + 1;
        std::size_t const s0 = static_cast<std::size_t>(cut) * 960;
        CHECK(left[s0 - 1] == doctest::Approx(0.5F));
        CHECK(left[s0 + 3 * 960] == doctest::Approx(0.5F * std::cos(std::numbers::pi / 4 * (3 * 960 + 0.5) / (3 * 960)) -
                                                    0.25F * std::sin(std::numbers::pi / 4 * (3 * 960 + 0.5) / (3 * 960))).epsilon(1e-4));
        CHECK(left[s0 + 6 * 960] == doctest::Approx(-0.25F));
    }
    // A mix with audio_frames 0: the audio cuts at the start of the mix.
    {
        PlaylistEntry first{a, 1, EndAction::Next, true};
        PlaylistEntry second{b, 1, EndAction::Freeze, true};
        second.transition = Transition::Mix;
        second.transitionFrames = 4;
        second.audioFrames = 0;
        std::vector<int> luma;
        auto const left = run(first, second, luma);
        auto const start = std::adjacent_find(luma.begin(), luma.end(), std::not_equal_to<>()) - luma.begin() + 1;
        std::size_t const s0 = static_cast<std::size_t>(start) * 960;
        CHECK(left[s0 - 1] == doctest::Approx(0.5F));
        CHECK(left[s0] == doctest::Approx(-0.25F));
    }
    // Own audio off on the second entry: the crossfade runs into silence.
    {
        PlaylistEntry first{a, 1, EndAction::Next, true};
        PlaylistEntry second{b, 1, EndAction::Freeze, true};
        second.transition = Transition::Mix;
        second.transitionFrames = 4;
        second.ownAudio = false;
        std::vector<int> luma;
        auto const left = run(first, second, luma);
        auto const start = std::adjacent_find(luma.begin(), luma.end(), std::not_equal_to<>()) - luma.begin() + 1;
        std::size_t const s0 = static_cast<std::size_t>(start) * 960;
        CHECK(left[s0 - 1] == doctest::Approx(0.5F));
        CHECK(left[s0 + 4 * 960 + 10] == doctest::Approx(0.F));
        float maxStep = 0;
        for (std::size_t i = 1; i < left.size(); ++i)
        {
            maxStep = std::max(maxStep, std::fabs(left[i] - left[i - 1]));
        }
        CHECK(maxStep < 1e-3F);
    }
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("an uploaded WAV is an audio item; the aux track plays at its level, ducks under own audio and fades out at the end")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-aux";
    auto engine = twoToneEngine(dir);
    auto const a = clipOf(*engine, 10, 20);
    auto const b = clipOf(*engine, 40, 50);
    // A 2 s WAV at 44.1 kHz, constant 0.4: 48 kHz float after the upload.
    std::vector<float> tone(44100 * 2 * 2, 0.4F);
    auto const wav = writeWav(tone, 2, 44100);
    auto const uploaded = request(*engine, "POST", "/api/v1/uploads", std::string(wav.begin(), wav.end()), "name=bed");
    REQUIRE(uploaded.status == 201);
    CHECK(uploaded.body.find("\"kind\":\"audio\"") != std::string::npos);
    auto const items = request(*engine, "GET", "/api/v1/audio");
    CHECK(items.body.find("\"name\":\"bed\"") != std::string::npos);
    auto const item = engine->audioItems().at(0);
    CHECK(item.samples == doctest::Approx(96000).epsilon(0.01));

    // Entry 1 without its own audio (aux at −6 dB, full), entry 2 with it (aux ducked by 12 dB), 3-frame fade to the end.
    auto const body = "{\"name\":\"bed\",\"entries\":[{\"clip_id\":\"" + a + "\",\"own_audio\":false},{\"clip_id\":\"" + b +
                      "\",\"end\":\"freeze\"}],\"aux\":{\"source\":\"" + item.id +
                      "\",\"level_db\":-6,\"duck_db\":-12,\"attack_ms\":20,\"release_ms\":20,\"fade_out_frames\":3}}";
    auto const created = request(*engine, "POST", "/api/v1/playlists", body);
    REQUIRE(created.status == 201);
    auto const id = created.body.substr(created.body.find("\"id\":\"") + 6, created.body.find('"', created.body.find("\"id\":\"") + 6) - created.body.find("\"id\":\"") - 6);
    auto const got = request(*engine, "GET", "/api/v1/playlists/" + id);
    CHECK(got.body.find("\"own_audio\":false") != std::string::npos);
    CHECK(got.body.find("\"aux\":{\"source\":\"" + item.id + "\",\"level_db\":-6") != std::string::npos);
    CHECK(request(*engine, "PUT", "/api/v1/playlists/" + id, "{\"aux\":{\"source\":\"audio-99\"}}").status == 400);
    CHECK(request(*engine, "PUT", "/api/v1/playlists/" + id, "{\"aux\":{\"level_db\":-6}}").status == 200);

    engine->playPlaylist(1, id);
    std::uint64_t out = at(300);
    std::vector<float> left;
    for (int i = 0; i < 26; ++i)
    {
        auto const frame = engine->render(1, out);
        out += kPeriod;
        for (std::size_t s = 0; s < frame.audio.size(); s += 2)
        {
            left.push_back(frame.audio[s]);
        }
    }
    double const half = std::pow(10.0, -6.0 / 20);
    // Grain 5 of entry 1: only the aux, at −6 dB (after its 2-grain fade in).
    CHECK(left[5 * 960 + 100] == doctest::Approx(0.4 * half).epsilon(1e-3));
    // Entry 2 plays its −0.25 under the aux ducked to −18 dB (−6 −12), well before the end fade.
    CHECK(left[13 * 960 + 100] == doctest::Approx(-0.25 + 0.4 * std::pow(10.0, -18.0 / 20)).epsilon(1e-3));
    // At the end (frame 50 reached after 11 grains of entry 2) the aux is faded out: entry 2 froze, all silent.
    CHECK(std::fabs(left.back()) < 1e-6F);
    float maxStep = 0;
    for (std::size_t i = 1; i < left.size(); ++i)
    {
        maxStep = std::max(maxStep, std::fabs(left[i] - left[i - 1]));
    }
    // The cut from entry 1 to 2 brings in −0.25 at once (a cut has no crossfade): the only step that large.
    CHECK(maxStep < 0.26F);
    CHECK(engine->statusJson().find("\"own_audio\":true") != std::string::npos);

    // Deleting the item removes it from the playlist.
    CHECK(request(*engine, "DELETE", "/api/v1/audio/" + item.id).status == 200);
    CHECK(engine->playlist(id).aux.source.empty());
    CHECK(request(*engine, "GET", "/api/v1/audio/" + item.id).status == 404);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("a clip's audio as the aux track loops with edge fades and pauses with the channel")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-aux-clip";
    auto engine = twoToneEngine(dir);
    auto const a = clipOf(*engine, 10, 20); // 11 frames of 0.5: 10560 samples
    auto const b = clipOf(*engine, 40, 59);
    PlaylistEntry only{b, 1, EndAction::Freeze, true};
    only.ownAudio = false;
    AuxTrack aux;
    aux.source = a;
    aux.fadeOutFrames = 0;
    auto const list = engine->createPlaylist("bed", {only}, false, aux);
    engine->playPlaylist(1, list);
    std::uint64_t out = at(400);
    std::vector<float> left;
    for (int i = 0; i < 16; ++i)
    {
        auto const frame = engine->render(1, out);
        out += kPeriod;
        for (std::size_t s = 0; s < frame.audio.size(); s += 2)
        {
            left.push_back(frame.audio[s]);
        }
    }
    // After its 2-grain fade in the aux is the clip's 0.5; at its loop point (sample 10560) the edges dip to 0.
    CHECK(left[3 * 960] == doctest::Approx(0.5F));
    CHECK(left[10560 - 1] < 0.01F);
    CHECK(left[10560] < 0.01F);
    CHECK(left[10560 + 480] == doctest::Approx(0.5F));
    float maxStep = 0;
    for (std::size_t i = 1; i < left.size(); ++i)
    {
        maxStep = std::max(maxStep, std::fabs(left[i] - left[i - 1]));
    }
    CHECK(maxStep < 0.003F);
    CHECK(engine->statusJson().find("\"aux\":{\"source\":\"" + a + "\"") != std::string::npos);
    // Paused, the aux fades out over two grains and is silent; it resumes where it was.
    engine->pause(1);
    (void)engine->render(1, out);
    out += kPeriod;
    (void)engine->render(1, out);
    out += kPeriod;
    auto const paused = engine->render(1, out);
    out += kPeriod;
    CHECK(std::all_of(paused.audio.begin(), paused.audio.end(), [](float s) { return s == 0.F; }));
    CHECK(engine->statusJson().find("\"aux\":null") != std::string::npos);
    engine.reset();
    std::filesystem::remove_all(dir);
}
