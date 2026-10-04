#include "config/config.hpp"

#include "nmos/ids.hpp"
#include "util/uuid.hpp"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace replay
{
namespace
{
std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool parseBool(std::string const& text, bool* out)
{
    auto const v = lower(text);
    if (v == "true" || v == "1" || v == "yes" || v == "on")
    {
        *out = true;
        return true;
    }
    if (v == "false" || v == "0" || v == "no" || v == "off")
    {
        *out = false;
        return true;
    }
    return false;
}

int parseInt(std::string const& key, std::string const& text, int min, int max)
{
    try
    {
        std::size_t used = 0;
        int const value = std::stoi(text, &used);
        if (used != text.size() || value < min || value > max)
        {
            throw ConfigError(key + " is outside " + std::to_string(min) + ".." + std::to_string(max));
        }
        return value;
    }
    catch (ConfigError const&)
    {
        throw;
    }
    catch (...)
    {
        throw ConfigError(key + " is not an integer");
    }
}

double parseDouble(std::string const& key, std::string const& text, double min, double max)
{
    try
    {
        std::size_t used = 0;
        double const value = std::stod(text, &used);
        if (used != text.size() || value < min || value > max)
        {
            throw ConfigError(key + " is out of range");
        }
        return value;
    }
    catch (ConfigError const&)
    {
        throw;
    }
    catch (...)
    {
        throw ConfigError(key + " is not a number");
    }
}

std::string take(std::map<std::string, std::string> const& values, std::string const& key, std::string const& fallback)
{
    auto const it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

bool isIndexedKey(std::string const& key, char const* prefix, int* index, std::string* field)
{
    std::string const head(prefix);
    if (key.rfind(head, 0) != 0)
    {
        return false;
    }
    auto const underscore = key.find('_');
    if (underscore == std::string::npos || underscore <= head.size())
    {
        return false;
    }
    try
    {
        std::size_t used = 0;
        int const n = std::stoi(key.substr(head.size(), underscore - head.size()), &used);
        if (used != underscore - head.size() || n < 1 || n > 16)
        {
            return false;
        }
        if (index != nullptr)
        {
            *index = n;
        }
        if (field != nullptr)
        {
            *field = key.substr(underscore + 1);
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool knownIndexed(std::string const& key)
{
    int index = 0;
    std::string field;
    if (isIndexedKey(key, "CAM", &index, &field))
    {
        static std::vector<std::string> const fields = {"LABEL", "COLOUR", "COLOR", "RECORD", "AUDIO", "PHASES", "BUFFER_HOURS", "HFR_FPS"};
        return std::find(fields.begin(), fields.end(), field) != fields.end();
    }
    if (isIndexedKey(key, "CH", &index, &field))
    {
        static std::vector<std::string> const fields = {"LABEL", "IDLE", "MOTION", "AUDIO", "TC", "FLOW", "ATMOS", "LOCK"};
        return std::find(fields.begin(), fields.end(), field) != fields.end();
    }
    return false;
}

std::uint64_t parseU64(std::string const& key, std::string const& text, std::uint64_t min, std::uint64_t max)
{
    try
    {
        std::size_t used = 0;
        unsigned long long const value = std::stoull(text, &used);
        if (used != text.size() || value < min || value > max)
        {
            throw ConfigError(key + " is out of range");
        }
        return static_cast<std::uint64_t>(value);
    }
    catch (ConfigError const&)
    {
        throw;
    }
    catch (...)
    {
        throw ConfigError(key + " is not an integer");
    }
}

std::map<std::string, std::vector<std::string>> parseNmosTags(std::string const& text)
{
    std::map<std::string, std::vector<std::string>> tags;
    if (text.empty() || text == "{}")
    {
        return tags;
    }
    std::size_t i = 0;
    auto skip = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t'))
        {
            ++i;
        }
    };
    auto parseString = [&]() -> std::string {
        skip();
        if (i >= text.size() || text[i] != '"')
        {
            throw ConfigError("NMOS_TAGS values must be JSON strings");
        }
        ++i;
        std::string out;
        while (i < text.size() && text[i] != '"')
        {
            if (text[i] == '\\' && i + 1 < text.size())
            {
                ++i;
            }
            out.push_back(text[i]);
            ++i;
        }
        if (i >= text.size() || text[i] != '"')
        {
            throw ConfigError("NMOS_TAGS has an unterminated string");
        }
        ++i;
        return out;
    };
    skip();
    if (i >= text.size() || text[i] != '{')
    {
        throw ConfigError("NMOS_TAGS must be a JSON object of string arrays");
    }
    ++i;
    while (true)
    {
        skip();
        if (i < text.size() && text[i] == '}')
        {
            break;
        }
        auto const key = parseString();
        skip();
        if (i >= text.size() || text[i] != ':')
        {
            throw ConfigError("NMOS_TAGS must be a JSON object of string arrays");
        }
        ++i;
        skip();
        if (i >= text.size() || text[i] != '[')
        {
            throw ConfigError("NMOS_TAGS values must be arrays of strings");
        }
        ++i;
        std::vector<std::string> values;
        while (true)
        {
            skip();
            if (i < text.size() && text[i] == ']')
            {
                ++i;
                break;
            }
            values.push_back(parseString());
            skip();
            if (i < text.size() && text[i] == ',')
            {
                ++i;
                continue;
            }
            if (i < text.size() && text[i] == ']')
            {
                ++i;
                break;
            }
            throw ConfigError("NMOS_TAGS values must be arrays of strings");
        }
        tags[key] = std::move(values);
        skip();
        if (i < text.size() && text[i] == ',')
        {
            ++i;
            continue;
        }
        skip();
        if (i < text.size() && text[i] == '}')
        {
            break;
        }
        throw ConfigError("NMOS_TAGS must be a JSON object of string arrays");
    }
    return tags;
}
} // namespace

std::vector<std::string> configKeys()
{
    return {"HOST_ID", "REPLAY_FORMAT", "REPLAY_INPUTS", "REPLAY_CHANNELS", "REPLAY_STORAGE_DIR", "REPLAY_BUFFER_HOURS", "REPLAY_JPEG_QUALITY",
        "REPLAY_PROTECT_MAX_PCT", "REPLAY_FLOW_MODULE", "REPLAY_INTERP_PRESET", "REPLAY_SLOWMO_AUDIO", "REPLAY_RAMP_FRAMES", "REPLAY_HFR_SNAP",
        "REPLAY_SEGMENT_SECONDS", "REPLAY_LIVE_DELAY_FRAMES", "REPLAY_PLAY_ON_FIRST_CLICK", "REPLAY_ALLOW_CPU_INTERP", "REPLAY_STORAGE_MIN_MBPS",
        "REPLAY_ODIRECT", "REPLAY_SYNTHETIC", "REPLAY_STATE_DIR", "MXL_DOMAIN_SCAN_PATH", "MXL_OUTPUT_DOMAIN_DIR", "MXL_OUTPUT_DOMAIN_ID",
        "MXL_HISTORY_DURATION", "MXL_CLEANUP_ON_EXIT", "SHUTDOWN_TIMEOUT_S", "NMOS_ENABLE", "NMOS_REGISTRY_ADDRESS", "NMOS_REGISTRY_PORT",
        "NMOS_QUERY_ADDRESS", "NMOS_QUERY_PORT", "NMOS_DNS_SD", "NMOS_PORT", "NMOS_SEED", "NMOS_LABEL", "NMOS_TAGS", "NMOS_HOST_ADDRESS", "WEB_ENABLE",
        "WEB_PORT", "LOG_LEVEL", "LOG_FORMAT", "REPLAY_CONFIG_FILE"};
}

std::string hostnameString()
{
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0)
    {
        return "replay";
    }
    return buf;
}

bool isIpv4Literal(std::string const& text)
{
    in_addr addr{};
    return inet_pton(AF_INET, text.c_str(), &addr) == 1;
}

std::string firstNonLoopbackIpv4()
{
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
    {
        return {};
    }
    std::string found;
    for (auto* cursor = list; cursor != nullptr; cursor = cursor->ifa_next)
    {
        if (cursor->ifa_addr == nullptr || cursor->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        auto const& in = reinterpret_cast<sockaddr_in const*>(cursor->ifa_addr)->sin_addr;
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &in, text, sizeof(text)) == nullptr)
        {
            continue;
        }
        std::string const address(text);
        if (address == "0.0.0.0" || address.rfind("127.", 0) == 0)
        {
            continue;
        }
        found = address;
        break;
    }
    freeifaddrs(list);
    return found;
}

std::string stateDirectory(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file)
{
    if (env.count("REPLAY_STATE_DIR") != 0)
    {
        return env.at("REPLAY_STATE_DIR");
    }
    if (file.count("REPLAY_STATE_DIR") != 0)
    {
        return file.at("REPLAY_STATE_DIR");
    }
    return "/config";
}

std::string nodeLabel(Config const& config)
{
    return config.nmosLabelExplicit ? config.nmosLabel : config.hostId;
}

std::string deviceLabel(Config const& config)
{
    return config.nmosLabelExplicit ? config.nmosLabel : "MXL Replay";
}

char const* idleName(IdleSource idle)
{
    switch (idle)
    {
    case IdleSource::Last:
        return "last";
    case IdleSource::Black:
        return "black";
    case IdleSource::E2e:
        return "e2e";
    }
    return "last";
}

char const* tcModeName(TcMode mode)
{
    return mode == TcMode::Output ? "output" : "source";
}

char const* flowModuleName(FlowModuleKind kind)
{
    return kind == FlowModuleKind::Ofa ? "ofa" : "dis-cuda";
}

std::map<std::string, std::string> readConfigFile(std::string const& path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw ConfigError("cannot read config file " + path);
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto const text = buffer.str();
    std::map<std::string, std::string> values;
    if (!text.empty() && text.find_first_not_of(" \t\r\n") != std::string::npos && text[text.find_first_not_of(" \t\r\n")] == '{')
    {
        std::string body = text;
        auto const start = body.find('{');
        auto const end = body.rfind('}');
        if (start == std::string::npos || end == std::string::npos || end <= start)
        {
            throw ConfigError("config file is not a JSON object");
        }
        body = body.substr(start + 1, end - start - 1);
        std::size_t i = 0;
        while (i < body.size())
        {
            while (i < body.size() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t' || body[i] == ','))
            {
                ++i;
            }
            if (i >= body.size())
            {
                break;
            }
            if (body[i] != '"')
            {
                throw ConfigError("config file has a non-string key");
            }
            auto const keyEnd = body.find('"', i + 1);
            if (keyEnd == std::string::npos)
            {
                throw ConfigError("config file has an unterminated key");
            }
            auto key = body.substr(i + 1, keyEnd - i - 1);
            i = body.find(':', keyEnd);
            if (i == std::string::npos)
            {
                throw ConfigError("config file is missing a colon");
            }
            ++i;
            while (i < body.size() && body[i] == ' ')
            {
                ++i;
            }
            if (i >= body.size() || body[i] != '"')
            {
                throw ConfigError("config values must be strings");
            }
            auto const valueEnd = body.find('"', i + 1);
            if (valueEnd == std::string::npos)
            {
                throw ConfigError("config file has an unterminated value");
            }
            values[key] = body.substr(i + 1, valueEnd - i - 1);
            i = valueEnd + 1;
        }
        return values;
    }
    std::string line;
    std::istringstream lines(text);
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        auto const eq = line.find('=');
        if (eq == std::string::npos)
        {
            throw ConfigError("config line is not KEY=value");
        }
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

LoadedConfig loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file,
    std::map<std::string, std::string> const& state)
{
    auto const known = configKeys();
    auto rejectUnknown = [&](std::map<std::string, std::string> const& source, char const* origin) {
        for (auto const& [key, value] : source)
        {
            (void)value;
            if (std::find(known.begin(), known.end(), key) == known.end() && !knownIndexed(key))
            {
                throw ConfigError(std::string("unknown configuration key ") + key + " in " + origin);
            }
        }
    };
    rejectUnknown(file, "the config file");
    rejectUnknown(state, "the state file");
    std::map<std::string, std::string> values = state;
    for (auto const& [key, value] : file)
    {
        values[key] = value;
    }
    for (auto const& [key, value] : env)
    {
        if (std::find(known.begin(), known.end(), key) != known.end() || knownIndexed(key))
        {
            values[key] = value;
        }
    }
    LoadedConfig loaded;
    Config& cfg = loaded.config;
    cfg.hostId = take(values, "HOST_ID", hostnameString());
    try
    {
        cfg.format = parseVideoFormat(take(values, "REPLAY_FORMAT", "1080p50"));
    }
    catch (std::exception const& ex)
    {
        throw ConfigError(ex.what());
    }
    cfg.inputs = parseInt("REPLAY_INPUTS", take(values, "REPLAY_INPUTS", "4"), 1, 12);
    cfg.channels = parseInt("REPLAY_CHANNELS", take(values, "REPLAY_CHANNELS", "2"), 1, 8);
    cfg.stateDir = take(values, "REPLAY_STATE_DIR", "/config");
    if (cfg.stateDir.empty())
    {
        throw ConfigError("REPLAY_STATE_DIR is empty");
    }
    cfg.storageDir = take(values, "REPLAY_STORAGE_DIR", "/data/replay");
    cfg.bufferHours = parseDouble("REPLAY_BUFFER_HOURS", take(values, "REPLAY_BUFFER_HOURS", "2"), 0.01, 48);
    cfg.jpegQuality = parseInt("REPLAY_JPEG_QUALITY", take(values, "REPLAY_JPEG_QUALITY", "92"), 1, 100);
    cfg.protectMaxPct = parseInt("REPLAY_PROTECT_MAX_PCT", take(values, "REPLAY_PROTECT_MAX_PCT", "50"), 1, 100);
    auto const flow = lower(take(values, "REPLAY_FLOW_MODULE", "dis-cuda"));
    if (flow == "ofa")
    {
        cfg.flowModule = FlowModuleKind::Ofa;
    }
    else if (flow == "dis-cuda")
    {
        cfg.flowModule = FlowModuleKind::DisCuda;
    }
    else
    {
        throw ConfigError("REPLAY_FLOW_MODULE must be dis-cuda or ofa");
    }
    bool presetOk = false;
    cfg.preset = parsePreset(lower(take(values, "REPLAY_INTERP_PRESET", "balanced")), &presetOk);
    if (!presetOk)
    {
        throw ConfigError("REPLAY_INTERP_PRESET must be fast, balanced or quality");
    }
    bool audioOk = false;
    cfg.slowmoAudio = parseAudioMode(lower(take(values, "REPLAY_SLOWMO_AUDIO", "mute")), &audioOk);
    if (!audioOk)
    {
        throw ConfigError("REPLAY_SLOWMO_AUDIO must be mute, stretch or follow");
    }
    cfg.rampFrames = parseInt("REPLAY_RAMP_FRAMES", take(values, "REPLAY_RAMP_FRAMES", "3"), 0, 60);
    cfg.hfrSnap = parseDouble("REPLAY_HFR_SNAP", take(values, "REPLAY_HFR_SNAP", "0.1"), 0, 1);
    cfg.segmentSeconds = parseInt("REPLAY_SEGMENT_SECONDS", take(values, "REPLAY_SEGMENT_SECONDS", "10"), 1, 120);
    cfg.liveDelayFrames = parseInt("REPLAY_LIVE_DELAY_FRAMES", take(values, "REPLAY_LIVE_DELAY_FRAMES", "2"), 0, 30);
    bool flag = false;
    if (!parseBool(take(values, "REPLAY_PLAY_ON_FIRST_CLICK", "false"), &flag))
    {
        throw ConfigError("REPLAY_PLAY_ON_FIRST_CLICK must be a boolean");
    }
    cfg.playOnFirstClick = flag;
    if (!parseBool(take(values, "REPLAY_ALLOW_CPU_INTERP", "false"), &flag))
    {
        throw ConfigError("REPLAY_ALLOW_CPU_INTERP must be a boolean");
    }
    cfg.allowCpuInterp = flag;
    cfg.storageMinMbps = parseDouble("REPLAY_STORAGE_MIN_MBPS", take(values, "REPLAY_STORAGE_MIN_MBPS", "100"), 0, 100000);
    if (!parseBool(take(values, "REPLAY_ODIRECT", "false"), &flag))
    {
        throw ConfigError("REPLAY_ODIRECT must be a boolean");
    }
    cfg.odirect = flag;
    if (!parseBool(take(values, "REPLAY_SYNTHETIC", "false"), &flag))
    {
        throw ConfigError("REPLAY_SYNTHETIC must be a boolean");
    }
    cfg.synthetic = flag;
    cfg.scanPath = take(values, "MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl");
    cfg.historyDurationNs = parseU64("MXL_HISTORY_DURATION", take(values, "MXL_HISTORY_DURATION", "2000000000"), 1, 86400000000000ull);
    if (!parseBool(take(values, "MXL_CLEANUP_ON_EXIT", "false"), &flag))
    {
        throw ConfigError("MXL_CLEANUP_ON_EXIT must be a boolean");
    }
    cfg.cleanupOnExit = flag;
    cfg.shutdownTimeoutS = parseInt("SHUTDOWN_TIMEOUT_S", take(values, "SHUTDOWN_TIMEOUT_S", "10"), 1, 600);
    if (!parseBool(take(values, "NMOS_ENABLE", "true"), &flag))
    {
        throw ConfigError("NMOS_ENABLE must be a boolean");
    }
    cfg.nmosEnable = flag;
    cfg.nmosRegistryAddress = take(values, "NMOS_REGISTRY_ADDRESS", "");
    cfg.nmosRegistryPort = parseInt("NMOS_REGISTRY_PORT", take(values, "NMOS_REGISTRY_PORT", "3210"), 1, 65535);
    cfg.nmosQueryAddress = take(values, "NMOS_QUERY_ADDRESS", cfg.nmosRegistryAddress);
    if (values.count("NMOS_QUERY_PORT") != 0)
    {
        cfg.nmosQueryPort = parseInt("NMOS_QUERY_PORT", values.at("NMOS_QUERY_PORT"), 1, 65535);
    }
    else
    {
        if (cfg.nmosRegistryPort >= 65535)
        {
            throw ConfigError("NMOS_QUERY_PORT defaults to NMOS_REGISTRY_PORT + 1, which does not fit");
        }
        cfg.nmosQueryPort = cfg.nmosRegistryPort + 1;
    }
    if (!parseBool(take(values, "NMOS_DNS_SD", "false"), &flag))
    {
        throw ConfigError("NMOS_DNS_SD must be a boolean");
    }
    cfg.nmosDnsSd = flag;
    cfg.nmosPort = parseInt("NMOS_PORT", take(values, "NMOS_PORT", "3302"), 1, 65535);
    cfg.nmosSeed = take(values, "NMOS_SEED", cfg.hostId + "-replay");
    cfg.nmosLabelExplicit = values.count("NMOS_LABEL") != 0;
    cfg.nmosLabel = take(values, "NMOS_LABEL", "");
    try
    {
        cfg.nmosTags = parseNmosTags(take(values, "NMOS_TAGS", ""));
    }
    catch (ConfigError const&)
    {
        throw;
    }
    if (values.count("NMOS_HOST_ADDRESS") != 0)
    {
        cfg.nmosHostAddress = values.at("NMOS_HOST_ADDRESS");
    }
    else if (values.count("HOST_ID") != 0 && isIpv4Literal(values.at("HOST_ID")))
    {
        cfg.nmosHostAddress = values.at("HOST_ID");
    }
    else
    {
        cfg.nmosHostAddress = firstNonLoopbackIpv4();
    }
    if (!isIpv4Literal(cfg.nmosHostAddress) || cfg.nmosHostAddress == "0.0.0.0" || cfg.nmosHostAddress.rfind("127.", 0) == 0)
    {
        throw ConfigError("NMOS_HOST_ADDRESS must be a non-loopback IPv4 address");
    }
    if (!parseBool(take(values, "WEB_ENABLE", "true"), &flag))
    {
        throw ConfigError("WEB_ENABLE must be a boolean");
    }
    cfg.webEnable = flag;
    cfg.webPort = parseInt("WEB_PORT", take(values, "WEB_PORT", "8150"), 0, 65535);
    cfg.logLevel = lower(take(values, "LOG_LEVEL", "info"));
    cfg.logFormat = lower(take(values, "LOG_FORMAT", "json"));
    cfg.configFile = take(values, "REPLAY_CONFIG_FILE", "");
    cfg.outputDomainId = take(values, "MXL_OUTPUT_DOMAIN_ID", "");
    if (cfg.outputDomainId.empty())
    {
        cfg.outputDomainId = makeNmosIds(cfg.nmosSeed).domain;
    }
    else if (!isUuid(cfg.outputDomainId))
    {
        throw ConfigError("MXL_OUTPUT_DOMAIN_ID is not a UUID");
    }
    cfg.outputDomainDir = take(values, "MXL_OUTPUT_DOMAIN_DIR", cfg.scanPath + "/replay-" + shortId(cfg.outputDomainId));

    static char const* colours[] = {"#3b82f6", "#ef4444", "#22c55e", "#eab308", "#a855f7", "#f97316", "#14b8a6", "#ec4899", "#84cc16", "#06b6d4", "#f43f5e", "#6366f1"};
    for (int i = 1; i <= cfg.inputs; ++i)
    {
        CameraConfig camera;
        camera.index = i;
        camera.label = take(values, "CAM" + std::to_string(i) + "_LABEL", "Cam " + std::to_string(i));
        camera.colour = take(values, "CAM" + std::to_string(i) + "_COLOUR", take(values, "CAM" + std::to_string(i) + "_COLOR", colours[(i - 1) % 12]));
        if (!parseBool(take(values, "CAM" + std::to_string(i) + "_RECORD", "true"), &camera.record))
        {
            throw ConfigError("CAM record flag must be a boolean");
        }
        if (!parseBool(take(values, "CAM" + std::to_string(i) + "_AUDIO", "true"), &camera.audio))
        {
            throw ConfigError("CAM audio flag must be a boolean");
        }
        camera.phases = parseInt("CAM_PHASES", take(values, "CAM" + std::to_string(i) + "_PHASES", "1"), 1, 4);
        camera.bufferHours = parseDouble("CAM_BUFFER_HOURS", take(values, "CAM" + std::to_string(i) + "_BUFFER_HOURS", std::to_string(cfg.bufferHours)), 0.01, 48);
        camera.nativeFps = parseInt("CAM_HFR_FPS", take(values, "CAM" + std::to_string(i) + "_HFR_FPS", "0"), 0, 480);
        cfg.cameras.push_back(camera);
    }
    for (int i = 1; i <= cfg.channels; ++i)
    {
        ChannelConfig channel;
        channel.index = i;
        channel.label = take(values, "CH" + std::to_string(i) + "_LABEL", i == 1 ? "PGM" : i == 2 ? "PVW" : "CH" + std::to_string(i));
        auto const idle = lower(take(values, "CH" + std::to_string(i) + "_IDLE", "last"));
        if (idle == "black")
        {
            channel.idle = IdleSource::Black;
        }
        else if (idle == "e2e")
        {
            channel.idle = IdleSource::E2e;
        }
        else if (idle == "last")
        {
            channel.idle = IdleSource::Last;
        }
        else
        {
            throw ConfigError("CH idle must be last, black or e2e");
        }
        bool ok = false;
        channel.motion = parseMotion(lower(take(values, "CH" + std::to_string(i) + "_MOTION", "interpolate")), &ok);
        if (!ok)
        {
            throw ConfigError("CH motion must be repeat, blend or interpolate");
        }
        channel.audio = parseAudioMode(lower(take(values, "CH" + std::to_string(i) + "_AUDIO", audioModeName(cfg.slowmoAudio))), &ok);
        if (!ok)
        {
            throw ConfigError("CH audio must be mute, stretch or follow");
        }
        auto const tc = lower(take(values, "CH" + std::to_string(i) + "_TC", "source"));
        if (tc == "output")
        {
            channel.timecode = TcMode::Output;
        }
        else if (tc == "source")
        {
            channel.timecode = TcMode::Source;
        }
        else
        {
            throw ConfigError("CH tc must be source or output");
        }
        auto const module = lower(take(values, "CH" + std::to_string(i) + "_FLOW", flowModuleName(cfg.flowModule)));
        if (module == "ofa")
        {
            channel.flow = FlowModuleKind::Ofa;
        }
        else if (module == "dis-cuda")
        {
            channel.flow = FlowModuleKind::DisCuda;
        }
        else
        {
            throw ConfigError("CH flow must be dis-cuda or ofa");
        }
        channel.atmosCamera = parseInt("CH_ATMOS", take(values, "CH" + std::to_string(i) + "_ATMOS", "0"), 0, cfg.inputs);
        if (!parseBool(take(values, "CH" + std::to_string(i) + "_LOCK", i == 2 ? "false" : "false"), &channel.lockToFirst))
        {
            throw ConfigError("CH lock must be a boolean");
        }
        cfg.channelList.push_back(channel);
    }
    loaded.flat = values;
    return loaded;
}

std::string exportKeyValue(Config const& config)
{
    std::ostringstream out;
    out << "REPLAY_FORMAT=" << config.format.token() << "\n";
    out << "REPLAY_INPUTS=" << config.inputs << "\n";
    out << "REPLAY_CHANNELS=" << config.channels << "\n";
    out << "REPLAY_STORAGE_DIR=" << config.storageDir << "\n";
    out << "REPLAY_BUFFER_HOURS=" << config.bufferHours << "\n";
    out << "REPLAY_JPEG_QUALITY=" << config.jpegQuality << "\n";
    out << "REPLAY_PROTECT_MAX_PCT=" << config.protectMaxPct << "\n";
    out << "REPLAY_FLOW_MODULE=" << flowModuleName(config.flowModule) << "\n";
    out << "REPLAY_INTERP_PRESET=" << presetName(config.preset) << "\n";
    out << "REPLAY_SLOWMO_AUDIO=" << audioModeName(config.slowmoAudio) << "\n";
    out << "MXL_DOMAIN_SCAN_PATH=" << config.scanPath << "\n";
    out << "MXL_OUTPUT_DOMAIN_DIR=" << config.outputDomainDir << "\n";
    out << "MXL_OUTPUT_DOMAIN_ID=" << config.outputDomainId << "\n";
    out << "REPLAY_STATE_DIR=" << config.stateDir << "\n";
    out << "NMOS_REGISTRY_ADDRESS=" << config.nmosRegistryAddress << "\n";
    out << "NMOS_REGISTRY_PORT=" << config.nmosRegistryPort << "\n";
    out << "NMOS_QUERY_ADDRESS=" << config.nmosQueryAddress << "\n";
    out << "NMOS_QUERY_PORT=" << config.nmosQueryPort << "\n";
    out << "NMOS_DNS_SD=" << (config.nmosDnsSd ? "true" : "false") << "\n";
    out << "NMOS_PORT=" << config.nmosPort << "\n";
    out << "NMOS_SEED=" << config.nmosSeed << "\n";
    out << "NMOS_HOST_ADDRESS=" << config.nmosHostAddress << "\n";
    if (config.nmosLabelExplicit)
    {
        out << "NMOS_LABEL=" << config.nmosLabel << "\n";
    }
    out << "MXL_HISTORY_DURATION=" << config.historyDurationNs << "\n";
    out << "MXL_CLEANUP_ON_EXIT=" << (config.cleanupOnExit ? "true" : "false") << "\n";
    out << "SHUTDOWN_TIMEOUT_S=" << config.shutdownTimeoutS << "\n";
    out << "WEB_PORT=" << config.webPort << "\n";
    for (auto const& camera : config.cameras)
    {
        out << "CAM" << camera.index << "_LABEL=" << camera.label << "\n";
        out << "CAM" << camera.index << "_COLOUR=" << camera.colour << "\n";
        out << "CAM" << camera.index << "_PHASES=" << camera.phases << "\n";
    }
    for (auto const& channel : config.channelList)
    {
        out << "CH" << channel.index << "_LABEL=" << channel.label << "\n";
        out << "CH" << channel.index << "_MOTION=" << motionName(channel.motion) << "\n";
        out << "CH" << channel.index << "_AUDIO=" << audioModeName(channel.audio) << "\n";
    }
    return out.str();
}
} // namespace replay
