// The state directory's config.json (1.8.2): written and read back with the JSON parser, and a state
// file that cannot be read is moved aside instead of stopping every start.
#include "app/engine.hpp"
#include "config/config.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace replay;

namespace
{
std::filesystem::path freshConfigDir(char const* name)
{
    auto const dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void writeText(std::filesystem::path const& path, std::string const& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readText(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::map<std::string, std::string> baseEnv(std::filesystem::path const& dir)
{
    return {{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
        {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()}, {"REPLAY_STORAGE_MIN_MBPS", "0"},
        {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}};
}

std::string const kTags = R"({"urn:x-example:function":["replay"],"urn:x-example:area":["studio \"1\""]})";
} // namespace

TEST_CASE("config.json round trip keeps quotes, backslashes, unicode and JSON in a string")
{
    auto const dir = freshConfigDir("mxl-replay-config-roundtrip");
    std::map<std::string, std::string> const values{
        {"NMOS_TAGS", kTags},
        {"CH1_LABEL", "say \"hi\""},
        {"CAM1_LABEL", R"(C:\cams\one \" two)"},
        {"CH2_LABEL", "Zürich – 東京 🎥"},
        {"NMOS_LABEL", "tab\there\nnew line, comma: colon {brace} [bracket]"},
        {"TSL_MAP", ""},
    };
    auto const path = dir / "config.json";
    writeText(path, configFileJson(values));
    CHECK(readConfigFile(path.string()) == values);

    // Escapes another writer may use: \u, a surrogate pair, \/ .
    writeText(path, R"({"CH1_LABEL": "\u00e9\ud83c\udfa5\/x", "CH2_LABEL":"a\\b"})");
    auto const other = readConfigFile(path.string());
    CHECK(other.at("CH1_LABEL") == "é🎥/x");
    CHECK(other.at("CH2_LABEL") == "a\\b");

    // Not a JSON object of strings.
    for (auto const* bad : {R"({"CH1_LABEL": "unterminated)", R"({"CH1_LABEL": 5})", R"({"CH1_LABEL": "a"} trailing)", R"({CH1_LABEL: "a"})"})
    {
        writeText(path, bad);
        CHECK_THROWS_AS((void)readConfigFile(path.string()), ConfigError);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("NMOS_TAGS is read by the JSON parser, escapes included")
{
    auto const loaded = loadConfig({{"NMOS_TAGS", R"({"urn:x-example:area":["studio \"1\"","caf\u00e9"]})"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {});
    CHECK(loaded.config.nmosTags.at("urn:x-example:area") == std::vector<std::string>{"studio \"1\"", "café"});
    CHECK(loadConfig({{"NMOS_TAGS", "{}"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}).config.nmosTags.empty());
    CHECK_THROWS_AS((void)loadConfig({{"NMOS_TAGS", R"({"a":"b"})"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}), ConfigError);
    CHECK_THROWS_AS((void)loadConfig({{"NMOS_TAGS", R"({"a":[1]})"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}), ConfigError);
}

TEST_CASE("a settings save with NMOS_TAGS in env starts again")
{
    // The failure of 1.8.1: the state file held NMOS_TAGS as JSON in a string, and the next start exited 78.
    auto const dir = freshConfigDir("mxl-replay-config-save");
    auto env = baseEnv(dir);
    env["NMOS_TAGS"] = kTags;
    auto const statePath = (dir / "state" / "config.json").string();
    {
        auto const loaded = loadStartupConfig(env, {}, statePath);
        Engine engine(loaded.config, loaded.flat);
        engine.openBuffer();
        std::string error;
        REQUIRE(engine.setPlayOnTally(1, false, error));
    }
    REQUIRE(readText(statePath).find("NMOS_TAGS") != std::string::npos);
    auto const again = loadStartupConfig(env, {}, statePath);
    CHECK(again.config.nmosTags.at("urn:x-example:area").at(0) == "studio \"1\"");
    CHECK_FALSE(again.config.channelList[0].playOnTally);
    CHECK(std::filesystem::exists(statePath));
    CHECK_FALSE(std::filesystem::exists(statePath + ".bad"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("a state config.json that cannot be loaded is moved aside")
{
    auto const dir = freshConfigDir("mxl-replay-config-bad");
    std::filesystem::create_directories(dir / "state");
    auto const statePath = (dir / "state" / "config.json").string();
    auto const env = baseEnv(dir);
    // The file 1.8.1 could not read back, an invalid value and an unknown key.
    for (auto const* text : {R"({"NMOS_TAGS": "{\"urn:x-example:function\":[\"replay\"]}", "CH1_PLAY_ON_TALLY": "false" )", R"({"REPLAY_JPEG_QUALITY": "999"})",
             R"({"NOT_A_KEY": "1"})"})
    {
        writeText(statePath, text);
        std::filesystem::remove(statePath + ".bad");
        auto const loaded = loadStartupConfig(env, {}, statePath);
        CHECK(loaded.config.inputs == 1);
        CHECK(loaded.config.jpegQuality == 92);
        CHECK_FALSE(std::filesystem::exists(statePath));
        CHECK(readText(statePath + ".bad") == text);
    }
    // A bad environment still exits 78 and leaves the state file where it is.
    writeText(statePath, R"({"CH1_LABEL": "kept"})");
    auto badEnv = env;
    badEnv["REPLAY_INPUTS"] = "99";
    CHECK_THROWS_AS((void)loadStartupConfig(badEnv, {}, statePath), ConfigError);
    CHECK(std::filesystem::exists(statePath));
    std::filesystem::remove_all(dir);
}

TEST_CASE("env overrides the state file; a key only in the state file stays")
{
    auto const dir = freshConfigDir("mxl-replay-config-precedence");
    std::filesystem::create_directories(dir / "state");
    auto const statePath = (dir / "state" / "config.json").string();
    writeText(statePath, configFileJson({{"NMOS_TAGS", R"({"a":["from-state"]})"}, {"NMOS_LABEL", "from-state"}}));
    auto env = baseEnv(dir);
    env["NMOS_TAGS"] = R"({"a":["from-env"]})";
    auto const loaded = loadStartupConfig(env, {}, statePath);
    CHECK(loaded.config.nmosTags.at("a").at(0) == "from-env");
    CHECK(loaded.config.nmosLabel == "from-state");
    std::filesystem::remove_all(dir);
}
