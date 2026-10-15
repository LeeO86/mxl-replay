#include "ops/preview.hpp"

#include "util/logging.hpp"
#include "version.hpp"

#include <cctype>
#include <sstream>

namespace replay
{
namespace
{
std::string quote(std::string const& text)
{
    return "\"" + jsonEscape(text) + "\"";
}
} // namespace

PreviewPlan previewPlan(Config const& cfg)
{
    PreviewPlan plan;
    plan.webrtc = cfg.previewMode == "webrtc";
    plan.ownMediamtx = plan.webrtc && cfg.previewPublishUrl.empty();
    plan.path = cfg.previewPathPrefix + "/mosaic";
    plan.publishBase = cfg.previewPublishUrl.empty() ? "rtsp://127.0.0.1:" + std::to_string(cfg.mediamtxRtspPort) : cfg.previewPublishUrl;
    plan.publishUrl = plan.publishBase + "/" + plan.path;
    return plan;
}

std::string previewWhepUrl(Config const& cfg)
{
    auto const base = cfg.previewWhepUrl.empty() ? "http://" + cfg.nmosHostAddress + ":" + std::to_string(cfg.mediamtxWhepPort) : cfg.previewWhepUrl;
    return base + "/" + previewPlan(cfg).path + "/whep";
}

std::string previewHlsUrl(Config const& cfg)
{
    auto const base = cfg.previewHlsUrl.empty() ? "http://" + cfg.nmosHostAddress + ":" + std::to_string(cfg.mediamtxHlsPort) : cfg.previewHlsUrl;
    return base + "/" + previewPlan(cfg).path + "/index.m3u8";
}

MosaicLayout mosaicLayout(Config const& cfg)
{
    std::vector<std::string> channels;
    std::vector<std::string> cameras;
    for (auto const& channel : cfg.channelList)
    {
        channels.push_back(channel.label);
    }
    for (auto const& camera : cfg.cameras)
    {
        cameras.push_back(camera.label);
    }
    return mosaicLayout(channels, cameras);
}

int mosaicStep(Config const& cfg)
{
    int step = 1;
    while (static_cast<double>(cfg.format.rateNum) / (static_cast<double>(cfg.format.rateDen) * step) > 30.5)
    {
        ++step;
    }
    return step;
}

std::string renderMediamtxConfig(Config const& cfg)
{
    std::string yml;
    yml += "logLevel: warn\n";
    yml += "api: false\n";
    yml += "metrics: false\n";
    yml += "pprof: false\n";
    yml += "playback: false\n";
    yml += "rtsp: true\n";
    yml += "rtspAddress: 127.0.0.1:" + std::to_string(cfg.mediamtxRtspPort) + "\n";
    // TCP only: without UDP, MediaMTX binds no RTP/RTCP ports (8000/8001 in every instance).
    yml += "rtspTransports: [tcp]\n";
    yml += "rtmp: false\n";
    yml += "srt: false\n";
    yml += "moq: false\n";
    yml += "hls: true\n";
    yml += "hlsAddress: :" + std::to_string(cfg.mediamtxHlsPort) + "\n";
    yml += "hlsAllowOrigins: [\"*\"]\n";
    yml += "hlsVariant: lowLatency\n";
    yml += "hlsSegmentDuration: 1s\n";
    yml += "hlsPartDuration: 200ms\n";
    yml += "webrtc: true\n";
    yml += "webrtcAddress: :" + std::to_string(cfg.mediamtxWhepPort) + "\n";
    yml += "webrtcAllowOrigins: [\"*\"]\n";
    yml += "webrtcLocalUDPAddress: :" + std::to_string(cfg.mediamtxIcePort) + "\n";
    yml += "webrtcLocalTCPAddress: :" + std::to_string(cfg.mediamtxIcePort) + "\n";
    yml += cfg.nmosHostAddress.empty() ? "webrtcAdditionalHosts: []\n" : "webrtcAdditionalHosts: [\"" + cfg.nmosHostAddress + "\"]\n";
    yml += "webrtcICEServers2: []\n";
    yml += "pathDefaults:\n";
    yml += "  source: publisher\n";
    yml += "paths:\n";
    yml += "  all_others:\n";
    return yml;
}

std::string previewStatusJson(Config const& cfg, PreviewStatus const& status)
{
    auto const plan = previewPlan(cfg);
    std::ostringstream out;
    out << "{\"mode\":" << quote(cfg.previewMode);
    if (plan.webrtc)
    {
        out << ",\"publish\":" << quote(plan.ownMediamtx ? "own" : "shared") << ",\"publish_url\":" << quote(plan.publishBase)
            << ",\"path_prefix\":" << quote(cfg.previewPathPrefix) << ",\"path\":" << quote(plan.path) << ",\"state\":" << quote(status.state)
            << ",\"error\":" << quote(status.error) << ",\"encoder\":" << quote(status.encoder) << ",\"frames\":" << status.frames;
        if (plan.ownMediamtx)
        {
            out << ",\"mediamtx\":{\"running\":" << (status.mediamtxRunning ? "true" : "false") << ",\"restarts\":" << status.mediamtxRestarts << "}";
        }
        // One stream: the mosaic.
        out << ",\"streams\":[{\"path\":" << quote(plan.path) << ",\"state\":" << quote(status.state) << ",\"error\":" << quote(status.error) << "}]";
    }
    out << "}";
    return out.str();
}

std::string previewMapJson(Config const& cfg)
{
    auto const layout = mosaicLayout(cfg);
    double const fps = static_cast<double>(cfg.format.rateNum) / (static_cast<double>(cfg.format.rateDen) * mosaicStep(cfg));
    std::ostringstream out;
    out << "{\"mode\":" << quote(cfg.previewMode) << ",\"width\":" << layout.width << ",\"height\":" << layout.height << ",\"fps\":" << fps
        << ",\"path\":" << quote(previewPlan(cfg).path) << ",\"whep\":" << quote(previewWhepUrl(cfg)) << ",\"hls\":" << quote(previewHlsUrl(cfg))
        << ",\"public\":{\"whep\":" << (cfg.previewWhepUrl.empty() ? "false" : "true") << ",\"hls\":" << (cfg.previewHlsUrl.empty() ? "false" : "true")
        << "},\"tiles\":[";
    for (std::size_t i = 0; i < layout.tiles.size(); ++i)
    {
        auto const& tile = layout.tiles[i];
        out << (i > 0 ? "," : "") << "{\"id\":" << quote(tile.id) << ",\"kind\":" << quote(tile.kind) << ",\"index\":" << tile.index
            << ",\"label\":" << quote(tile.label) << ",\"x\":" << tile.x << ",\"y\":" << tile.y << ",\"w\":" << tile.w << ",\"h\":" << tile.h << "}";
    }
    out << "]}";
    return out.str();
}

std::vector<WidgetSpec> const& widgetSpecs()
{
    static std::vector<WidgetSpec> const specs{
        {"transport",
         "Replay transport",
         480,
         180,
         {{"channel", "channel", "Channel", true, false},
          {"clip", "boolean", "IN / OUT, Go IN / Go OUT and Create clip", false, false},
          {"presets", "boolean", "Speed presets", false, true}}},
        {"clip-list", "Replay clips and playlists", 400, 300, {{"channel", "channel", "Channel to load into (empty: a channel picker)", false, false}}},
        {"playlist-editor", "Replay playlist editor", 480, 320, {{"playlist", "playlist", "Playlist (empty: a playlist picker)", false, false}}},
        {"timeline", "Replay timeline", 600, 180, {{"channel", "channel", "Channel", true, false}}},
    };
    return specs;
}

WidgetSpec const* findWidget(std::string const& id)
{
    for (auto const& spec : widgetSpecs())
    {
        if (spec.id == id)
        {
            return &spec;
        }
    }
    return nullptr;
}

std::string widgetQueryError(Config const& cfg, WidgetSpec const& widget, std::function<std::string(std::string const&)> const& value)
{
    for (auto const& param : widget.params)
    {
        auto const text = value(param.name);
        if (text.empty())
        {
            if (param.required)
            {
                return param.kind == "channel" ? "channel must be a channel 1.." + std::to_string(cfg.channels) : param.name + " is required";
            }
            continue;
        }
        if (param.kind == "channel")
        {
            bool const valid = text.size() <= 2 && text.find_first_not_of("0123456789") == std::string::npos && std::stoi(text) >= 1 &&
                               std::stoi(text) <= cfg.channels;
            if (!valid)
            {
                return "channel must be a channel 1.." + std::to_string(cfg.channels);
            }
        }
        else if (param.kind == "boolean")
        {
            if (text != "true" && text != "false" && text != "1" && text != "0")
            {
                return param.name + " must be true or false";
            }
        }
        else if (param.kind == "playlist")
        {
            if (text.size() > 64 || text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos)
            {
                return "playlist must be a playlist id";
            }
        }
    }
    auto const theme = value("theme");
    if (!theme.empty() && theme != "dark" && theme != "light" && theme != "transparent")
    {
        return "theme must be dark, light or transparent";
    }
    return {};
}

std::string widgetsJson(Config const& cfg)
{
    std::ostringstream out;
    out << "[";
    for (auto const& spec : widgetSpecs())
    {
        out << (&spec == &widgetSpecs().front() ? "" : ",") << R"({"id":)" << quote(spec.id) << R"(,"title":)" << quote(spec.title)
            << R"(,"params":{"type":"object","properties":{)";
        std::string required;
        for (auto const& param : spec.params)
        {
            out << (&param == &spec.params.front() ? "" : ",") << quote(param.name) << ":{";
            if (param.kind == "channel")
            {
                out << R"("type":"integer","minimum":1,"maximum":)" << cfg.channels;
            }
            else if (param.kind == "boolean")
            {
                out << R"("type":"boolean","default":)" << (param.defaultTrue ? "true" : "false");
            }
            else
            {
                out << R"("type":"string","pattern":"^[A-Za-z0-9._-]{1,64}$")";
            }
            out << R"(,"title":)" << quote(param.title) << "}";
            if (param.required)
            {
                required += (required.empty() ? "" : ",") + quote(param.name);
            }
        }
        out << "}";
        if (!required.empty())
        {
            out << R"(,"required":[)" << required << "]";
        }
        out << R"(},"min_size":{"w":)" << spec.minW << R"(,"h":)" << spec.minH << R"(},"version":)" << quote(REPLAY_VERSION) << "}";
    }
    out << "]";
    return out.str();
}

bool listedOrigin(std::string const& sources, std::string origin)
{
    auto const normal = [](std::string text) {
        for (char& c : text)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (!text.empty() && text.back() == '/')
        {
            text.pop_back();
        }
        return text;
    };
    origin = normal(origin);
    if (origin.empty() || origin == "null")
    {
        return false;
    }
    std::istringstream tokens(sources);
    std::string token;
    while (tokens >> token)
    {
        if (token == "*" || normal(token) == origin)
        {
            return true;
        }
    }
    return false;
}
} // namespace replay
