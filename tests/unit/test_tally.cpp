#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "tally/tsl.hpp"
#include "util/json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace replay;

namespace
{
std::vector<std::uint8_t> hex(std::string const& text)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
    {
        out.push_back(static_cast<std::uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

void put16(std::vector<std::uint8_t>& out, int value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

// PBC is the number of bytes after it.
void setPbc(std::vector<std::uint8_t>& body)
{
    body[0] = static_cast<std::uint8_t>((body.size() - 2) & 0xff);
    body[1] = static_cast<std::uint8_t>(((body.size() - 2) >> 8) & 0xff);
}

// Byte vectors from the platform's reference codec (mxl-poc-platform tsl5.py, a1883cb), as in mxl-webrtc-monitor.
// Screen 0, UTF-16LE: display 0 LH red, text amber, RH green "Kamera Zürich"; display 1 RH red,
// brightness 1, U+1F3A5 " Cam 2"; display 254 LH and text green, brightness 2, "þ".
std::string const kUtf16 =
    "4200000100000000de001a004b0061006d0065007200610020005a00fc0072006900630068000100410010003cd8a5df2000430061006d0020003200fe00a8000200fe00";
// Screen 3, ASCII: display 2 LH amber "CAM 3"; display 3 text red, RH amber, brightness 0, "VT".
std::string const kAscii = "1700000003000200f000050043414d20330300070002005654";
// kUtf16 in DLE/STX ... DLE/ETX for TCP; its two 0xFE bytes are stuffed.
std::string const kUtf16Tcp =
    "fe024200000100000000de001a004b0061006d0065007200610020005a00fc0072006900630068000100410010003cd8a5df2000430061006d0020003200fefe00a8000200fefe00fe03";
// kAscii in DLE/STX without DLE/ETX.
std::string const kAsciiTcp = "fe021700000003000200f000050043414d20330300070002005654";

// Tally values in a display's control word.
constexpr int kLhRed = 1 << 4;
constexpr int kTextRed = 1 << 2;
constexpr int kRhRed = 1;
constexpr int kLhGreen = 2 << 4;

// A TSL 5.0 message (ASCII, no text) from (display, control) pairs, parsed.
TslMessage tally(std::vector<std::pair<int, int>> const& displays, int screen = 0)
{
    std::vector<std::uint8_t> body{0, 0, 0, 0};
    put16(body, screen);
    for (auto const& [index, control] : displays)
    {
        put16(body, index);
        put16(body, control);
        put16(body, 0);
    }
    setPbc(body);
    return parseTsl5(body.data(), body.size());
}

// An engine with one camera (frames 0–29 recorded), `channels` channels and a clip of frames 10–20.
struct Bench
{
    std::filesystem::path dir;
    std::unique_ptr<Engine> engine;
    std::string clip;

    Bench(char const* name, int channels, std::map<std::string, std::string> extra = {})
        : dir(std::filesystem::temp_directory_path() / name)
    {
        std::filesystem::remove_all(dir);
        std::map<std::string, std::string> env{{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", std::to_string(channels)},
            {"REPLAY_BUFFER_HOURS", "0.01"}, {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()},
            {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"REPLAY_PROTECT_MAX_PCT", "90"}, {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"},
            {"TSL_ENABLE", "true"}};
        for (auto const& [key, value] : extra)
        {
            env[key] = value;
        }
        auto const loaded = loadConfig(env, {});
        engine = std::make_unique<Engine>(loaded.config, loaded.flat);
        engine->openBuffer();
        std::uint64_t const period = 20000000ull;
        for (int i = 0; i < 30; ++i)
        {
            Frame10 frame;
            frame.allocate(64, 32);
            frame.fill(300, 512, 512);
            engine->ingestVideo(1, 1, static_cast<std::uint64_t>(i) * period, std::move(frame));
        }
        engine->setPosition(1, 10 * period);
        engine->markIn(1);
        engine->setPosition(1, 20 * period);
        engine->markOut(1);
        std::string error;
        clip = engine->createClip(1, "goal", false, false, error);
        REQUIRE(error.empty());
        engine->live(1);
    }
    ~Bench() { std::filesystem::remove_all(dir); }

    // The channel's status object.
    [[nodiscard]] std::string field(int channel, char const* key) const
    {
        Json doc;
        REQUIRE(parseJson(engine->statusJson(), doc));
        auto const& item = doc.get("channels")->items.at(static_cast<std::size_t>(channel - 1));
        auto const* value = item.get(key);
        REQUIRE(value != nullptr);
        return value->kind == Json::Kind::String ? value->text : value->kind == Json::Kind::Bool ? (value->boolean ? "true" : "false") : value->text;
    }
    [[nodiscard]] std::string plays(int channel) const
    {
        auto const metrics = engine->metrics().render();
        auto const key = "mxl_replay_tally_plays_total{channel=\"" + std::to_string(channel) + "\"} ";
        auto const at = metrics.find(key);
        return at == std::string::npos ? "none" : metrics.substr(at + key.size(), metrics.find('\n', at) - at - key.size());
    }
};
} // namespace

TEST_CASE("tsl 5.0 matches the reference codec: utf-16, several displays")
{
    auto const packet = hex(kUtf16);
    auto const message = parseTsl5(packet.data(), packet.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 3);
    auto const& first = message.displays[0];
    CHECK(first.screen == 0);
    CHECK(first.index == 0);
    CHECK(first.lh == 1);
    CHECK(first.text == 3);
    CHECK(first.rh == 2);
    CHECK(first.brightness == 3);
    CHECK(first.textValue == "Kamera Z\xC3\xBCrich");
    auto const& second = message.displays[1];
    CHECK(second.index == 1);
    CHECK((second.lh == 0 && second.text == 0 && second.rh == 1));
    CHECK(second.brightness == 1);
    CHECK(second.textValue == "\xF0\x9F\x8E\xA5 Cam 2");
    auto const& third = message.displays[2];
    CHECK(third.index == 254);
    CHECK((third.lh == 2 && third.text == 2 && third.rh == 0));
    CHECK(third.brightness == 2);
    CHECK(third.textValue == "\xC3\xBE");
}

TEST_CASE("tsl 5.0 matches the reference codec: ascii")
{
    auto const packet = hex(kAscii);
    auto const message = parseTsl5(packet.data(), packet.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 2);
    CHECK(message.displays[0].screen == 3);
    CHECK(message.displays[0].index == 2);
    CHECK((message.displays[0].lh == 3 && message.displays[0].text == 0 && message.displays[0].rh == 0));
    CHECK(message.displays[0].textValue == "CAM 3");
    CHECK(message.displays[1].index == 3);
    CHECK((message.displays[1].lh == 0 && message.displays[1].text == 1 && message.displays[1].rh == 3));
    CHECK(message.displays[1].brightness == 0);
    CHECK(message.displays[1].textValue == "VT");
}

TEST_CASE("tsl tcp framing: dle stuffing, dle/etx optional, split reads")
{
    auto stream = hex(kUtf16Tcp);
    auto const ascii = hex(kAsciiTcp);
    stream.insert(stream.end(), ascii.begin(), ascii.end());
    stream.insert(stream.end(), ascii.begin(), ascii.end());
    // Every chunk size, so reads also split DLE pairs and the DLE/STX marks.
    for (std::size_t chunk = 1; chunk <= 8; ++chunk)
    {
        TslFramer framer;
        std::vector<std::vector<std::uint8_t>> packets;
        for (std::size_t i = 0; i < stream.size(); i += chunk)
        {
            for (auto& packet : framer.feed(stream.data() + i, std::min(chunk, stream.size() - i)))
            {
                packets.push_back(std::move(packet));
            }
        }
        REQUIRE(packets.size() == 3);
        CHECK(packets[0] == hex(kUtf16));
        CHECK(packets[1] == hex(kAscii));
        CHECK(packets[2] == hex(kAscii));
    }
    // Without DLE/ETX a packet is complete at PBC, not only when the next one starts.
    TslFramer framer;
    CHECK(framer.feed(ascii.data(), ascii.size()).size() == 1);

    // UDP: a bare packet, or one wrapped like TCP.
    auto const wrapped = hex(kUtf16Tcp);
    auto const fromWrapped = tslDatagram(wrapped.data(), wrapped.size());
    REQUIRE(fromWrapped.size() == 1);
    CHECK(fromWrapped[0] == hex(kUtf16));
    auto const bare = hex(kAscii);
    auto const fromBare = tslDatagram(bare.data(), bare.size());
    REQUIRE(fromBare.size() == 1);
    CHECK(fromBare[0] == bare);
}

TEST_CASE("tsl 5.0 skips control data and screen control, repairs text, rejects short packets")
{
    std::vector<std::uint8_t> body{0, 0, 0, 0x01};
    put16(body, 0);
    put16(body, 0);
    put16(body, 0x8000 | 1); // control data: skipped
    put16(body, 2);
    body.push_back('x');
    body.push_back('x');
    put16(body, 1);
    put16(body, 2);
    // "A", a lone high surrogate, "B", NUL.
    put16(body, 8);
    for (int unit : std::vector<int>{'A', 0xD800, 'B', 0})
    {
        put16(body, unit);
    }
    setPbc(body);
    auto message = parseTsl5(body.data(), body.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 1);
    CHECK(message.displays[0].index == 1);
    CHECK(message.displays[0].rh == 2);
    CHECK(message.displays[0].textValue == "A\xEF\xBF\xBD" "B");

    body[3] = 0x03; // screen control data
    CHECK(parseTsl5(body.data(), body.size()).displays.empty());

    // ASCII: a byte above 0x7F becomes U+FFFD, as the reference decodes it.
    std::vector<std::uint8_t> ascii{0, 0, 0, 0};
    put16(ascii, 0);
    put16(ascii, 0);
    put16(ascii, 1);
    put16(ascii, 3);
    ascii.push_back('C');
    ascii.push_back(0xFE);
    ascii.push_back(0);
    setPbc(ascii);
    message = parseTsl5(ascii.data(), ascii.size());
    REQUIRE(message.displays.size() == 1);
    CHECK(message.displays[0].textValue == "C\xEF\xBF\xBD");

    auto const truncated = hex(kAscii.substr(0, kAscii.size() - 4));
    CHECK(parseTsl5(truncated.data(), truncated.size()).error == "pbc");
    CHECK(parseTsl5(body.data(), 4).error == "short");
}

TEST_CASE("tally colour is text, else RH, else LH; display index maps to channels")
{
    CHECK(effectiveTally(0, 0, 0) == 0);
    CHECK(effectiveTally(1, 0, 0) == 1);
    CHECK(effectiveTally(1, 2, 0) == 2);
    CHECK(effectiveTally(1, 2, 3) == 3);
    CHECK(effectiveTally(0, 0, 3) == 3);
    CHECK(channelForDisplay("", 0) == 1);
    CHECK(channelForDisplay("", 3) == 4);
    CHECK(channelForDisplay("2:4,3:1", 3) == 1);
    CHECK(channelForDisplay("2:4,3:1", 0) == 0);
}

TEST_CASE("tsl settings and play on tally")
{
    auto const defaults = loadConfig({{"NMOS_HOST_ADDRESS", "10.1.2.3"}}, {}).config;
    CHECK(defaults.tslEnable == false);
    CHECK(defaults.tslUdpPort == 8914);
    CHECK(defaults.tslTcpPort == 8915);
    CHECK(defaults.tslScreen == -1);
    CHECK(defaults.tslMap.empty());
    CHECK(defaults.channelList[0].playOnTally);
    CHECK(defaults.channelList[1].playOnTally);

    auto const set = loadConfig({{"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"TSL_ENABLE", "true"}, {"TSL_SCREEN", "0"}, {"TSL_MAP", "0:2,7:1"},
                                    {"TSL_UDP_PORT", "9914"}, {"TSL_TCP_PORT", "9915"}, {"CH2_PLAY_ON_TALLY", "false"}},
        {})
                         .config;
    CHECK(set.tslEnable);
    CHECK(set.tslScreen == 0);
    CHECK(set.tslMap == "0:2,7:1");
    CHECK(set.tslUdpPort == 9914);
    CHECK(set.tslTcpPort == 9915);
    CHECK(set.channelList[0].playOnTally);
    CHECK_FALSE(set.channelList[1].playOnTally);

    // Two channels by default: a map to channel 3 is invalid.
    for (auto const& map : {"0:3", "0:0", "-1:1", "a:1", "0-1", "0:1,,1:2"})
    {
        CHECK_THROWS_AS((void)loadConfig({{"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"TSL_MAP", map}}, {}), ConfigError);
    }
    CHECK_THROWS_AS((void)loadConfig({{"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"TSL_SCREEN", "-2"}}, {}), ConfigError);
    CHECK_THROWS_AS((void)loadConfig({{"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"TSL_UDP_PORT", "0"}}, {}), ConfigError);
    CHECK_THROWS_AS((void)loadConfig({{"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"TSL_ENABLE", "maybe"}}, {}), ConfigError);
    CHECK_THROWS_AS((void)loadConfig({{"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"CH1_PLAY_ON_TALLY", "maybe"}}, {}), ConfigError);
}

TEST_CASE("a rising red plays a Cued or a Paused channel, once")
{
    Bench bench("mxl-replay-tally-edge", 3);
    auto& engine = *bench.engine;
    engine.shotClick(1, bench.clip);
    CHECK(bench.field(1, "shot") == "cued");
    engine.pause(2);
    CHECK(bench.field(2, "shot") == "paused");

    // Display 0 (channel 1) LH red, display 1 (channel 2) red text tally: both play.
    engine.applyTally(tally({{0, kLhRed}, {1, kTextRed | kLhGreen}}));
    CHECK(bench.field(1, "shot") == "playing");
    CHECK(bench.field(1, "playing") == "true");
    CHECK(bench.field(2, "shot") == "playing");
    CHECK(bench.field(1, "tsl_lh") == "1");
    CHECK(bench.field(2, "tsl_text_tally") == "1");
    CHECK(bench.field(2, "tally") == "1");
    CHECK(bench.field(1, "tally_play_ns") != "0");
    CHECK(bench.plays(1) == "1");
    CHECK(bench.plays(2) == "1");

    // A red that stays on (the controller repeats it) does not play again.
    engine.pause(1);
    engine.applyTally(tally({{0, kLhRed}}));
    CHECK(bench.field(1, "shot") == "paused");
    CHECK(bench.plays(1) == "1");
    // Off, then red again: plays.
    engine.applyTally(tally({{0, 0}}));
    CHECK(bench.field(1, "tsl_lh") == "0");
    engine.applyTally(tally({{0, kLhRed}}));
    CHECK(bench.field(1, "shot") == "playing");
    CHECK(bench.plays(1) == "2");
    // RH red alone is not the red light.
    engine.pause(1);
    engine.applyTally(tally({{0, 0}}));
    engine.applyTally(tally({{0, kRhRed}}));
    CHECK(bench.field(1, "shot") == "paused");
    CHECK(bench.field(1, "tsl_rh") == "1");
    CHECK(bench.field(1, "tally") == "1");
    CHECK(bench.plays(1) == "2");
    CHECK(engine.metrics().render().find("mxl_replay_tsl_packets_total 6") != std::string::npos);
}

TEST_CASE("red does not play a live, playing or idle channel, nor with play on tally off")
{
    Bench bench("mxl-replay-tally-not", 3);
    auto& engine = *bench.engine;
    auto const redEdge = [&](int display) {
        engine.applyTally(tally({{display, 0}}));
        engine.applyTally(tally({{display, kLhRed}}));
    };
    // Channel 1 live, channel 2 playing, channel 3 idle off live (positioned, nothing cued).
    engine.playClip(2, bench.clip);
    engine.setPosition(3, 5 * 20000000ull);
    REQUIRE(bench.field(3, "shot") == "idle");
    REQUIRE(bench.field(3, "live") == "false");
    redEdge(0);
    redEdge(1);
    redEdge(2);
    CHECK(bench.field(1, "live") == "true");
    CHECK(bench.field(1, "shot") == "idle");
    CHECK(bench.field(3, "playing") == "false");
    CHECK(bench.plays(1) == "0");
    CHECK(bench.plays(2) == "0");
    CHECK(bench.plays(3) == "0");

    // Play on tally off: a Paused channel stays paused. The setting is kept in the state directory.
    std::string error;
    REQUIRE(engine.setPlayOnTally(2, false, error));
    engine.pause(2);
    redEdge(1);
    CHECK(bench.field(2, "shot") == "paused");
    CHECK(bench.field(2, "play_on_tally") == "false");
    CHECK(bench.plays(2) == "0");
    std::ifstream saved(bench.dir / "state" / "config.json");
    std::string const text((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"CH2_PLAY_ON_TALLY\": \"false\"") != std::string::npos);
    CHECK(engine.exportConfigJson().find("\"CH2_PLAY_ON_TALLY\":\"false\"") != std::string::npos);
    REQUIRE(engine.setPlayOnTally(2, true, error));
    redEdge(1);
    CHECK(bench.field(2, "shot") == "playing");

    // A malformed packet is counted.
    std::vector<std::uint8_t> shortPacket{1, 0, 0};
    engine.applyTally(parseTsl5(shortPacket.data(), shortPacket.size()));
    CHECK(engine.metrics().render().find("mxl_replay_tsl_errors_total 1") != std::string::npos);
}

TEST_CASE("TSL_SCREEN filters and TSL_MAP maps displays to channels")
{
    Bench bench("mxl-replay-tally-map", 3, {{"TSL_SCREEN", "3"}, {"TSL_MAP", "2:3,3:1"}});
    auto& engine = *bench.engine;
    engine.pause(1);
    engine.pause(3);
    // Screen 0 is not TSL_SCREEN: nothing changes.
    engine.applyTally(tally({{2, kLhRed}, {3, kLhRed}}, 0));
    CHECK(bench.field(3, "shot") == "paused");
    CHECK(bench.field(3, "tsl_lh") == "0");
    // Screen 3: display 2 is channel 3, display 3 channel 1; display 0 is not mapped.
    engine.applyTally(tally({{2, kLhRed}, {0, kLhRed}}, 3));
    CHECK(bench.field(3, "shot") == "playing");
    CHECK(bench.field(1, "shot") == "paused");
    engine.applyTally(tally({{3, kTextRed}}, 3));
    CHECK(bench.field(1, "shot") == "playing");
}
