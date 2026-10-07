#include "ops/api.hpp"

#include "config/config.hpp"
#include "media/timebase.hpp"
#include "ops/httpserver.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string_view>

namespace replay
{
namespace
{
// A parsed request body. A number keeps its literal: TAI nanoseconds do not fit a double.
struct Json
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text; // a string's value, or a number's literal
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    [[nodiscard]] Json const* get(std::string const& key) const
    {
        for (auto const& member : members)
        {
            if (member.first == key)
            {
                return &member.second;
            }
        }
        return nullptr;
    }
};

class JsonReader
{
public:
    explicit JsonReader(std::string const& text)
        : text_(text)
    {
    }

    bool parse(Json& out)
    {
        if (!value(out, 0))
        {
            return false;
        }
        skipSpace();
        return pos_ == text_.size();
    }

private:
    void skipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
        {
            ++pos_;
        }
    }

    bool consume(char c)
    {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c)
        {
            ++pos_;
            return true;
        }
        return false;
    }

    bool word(std::string_view literal)
    {
        if (text_.compare(pos_, literal.size(), literal) != 0)
        {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    bool value(Json& out, int depth)
    {
        skipSpace();
        if (pos_ >= text_.size() || depth > 32)
        {
            return false;
        }
        char const c = text_[pos_];
        if (c == '{')
        {
            ++pos_;
            out.kind = Json::Kind::Object;
            if (consume('}'))
            {
                return true;
            }
            do
            {
                skipSpace();
                std::string key;
                Json member;
                if (!string(key) || !consume(':') || !value(member, depth + 1))
                {
                    return false;
                }
                out.members.emplace_back(std::move(key), std::move(member));
            } while (consume(','));
            return consume('}');
        }
        if (c == '[')
        {
            ++pos_;
            out.kind = Json::Kind::Array;
            if (consume(']'))
            {
                return true;
            }
            do
            {
                Json item;
                if (!value(item, depth + 1))
                {
                    return false;
                }
                out.items.push_back(std::move(item));
            } while (consume(','));
            return consume(']');
        }
        if (c == '"')
        {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (c == 't' || c == 'f')
        {
            out.kind = Json::Kind::Bool;
            out.boolean = c == 't';
            return word(out.boolean ? "true" : "false");
        }
        if (c == 'n')
        {
            return word("null");
        }
        return number(out);
    }

    bool hex4(unsigned& code)
    {
        if (pos_ + 4 > text_.size())
        {
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i)
        {
            char const h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9')
            {
                code |= static_cast<unsigned>(h - '0');
            }
            else if (h >= 'a' && h <= 'f')
            {
                code |= static_cast<unsigned>(h - 'a' + 10);
            }
            else if (h >= 'A' && h <= 'F')
            {
                code |= static_cast<unsigned>(h - 'A' + 10);
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    static void utf8(unsigned code, std::string& out)
    {
        if (code < 0x80)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else if (code < 0x10000)
        {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    bool string(std::string& out)
    {
        if (pos_ >= text_.size() || text_[pos_] != '"')
        {
            return false;
        }
        ++pos_;
        while (pos_ < text_.size())
        {
            char const c = text_[pos_++];
            if (c == '"')
            {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20)
            {
                return false;
            }
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size())
            {
                return false;
            }
            char const escaped = text_[pos_++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                out.push_back(escaped);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
            {
                unsigned code = 0;
                if (!hex4(code))
                {
                    return false;
                }
                if (code >= 0xd800 && code < 0xdc00)
                {
                    unsigned low = 0;
                    if (!word("\\u") || !hex4(low) || low < 0xdc00 || low >= 0xe000)
                    {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(code, out);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool number(Json& out)
    {
        auto const start = pos_;
        auto const digits = [&] {
            auto const from = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                ++pos_;
            }
            return pos_ > from;
        };
        if (pos_ < text_.size() && text_[pos_] == '-')
        {
            ++pos_;
        }
        if (!digits())
        {
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == '.')
        {
            ++pos_;
            if (!digits())
            {
                return false;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
            {
                ++pos_;
            }
            if (!digits())
            {
                return false;
            }
        }
        out.kind = Json::Kind::Number;
        out.text = text_.substr(start, pos_ - start);
        return true;
    }

    std::string const& text_;
    std::size_t pos_ = 0;
};

// The request body as a JSON object; an empty body is an empty object.
bool parseBody(std::string const& text, Json& body)
{
    if (text.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        body.kind = Json::Kind::Object;
        return true;
    }
    JsonReader reader(text);
    return reader.parse(body) && body.kind == Json::Kind::Object;
}

// Numbers may also come as strings ("0.5"), as earlier clients sent them.
std::optional<double> toDouble(Json const& value)
{
    if ((value.kind != Json::Kind::Number && value.kind != Json::Kind::String) || value.text.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    double const result = std::strtod(value.text.c_str(), &end);
    if (errno != 0 || end != value.text.c_str() + value.text.size() || !std::isfinite(result))
    {
        return std::nullopt;
    }
    return result;
}

std::optional<std::int64_t> toInt(Json const& value)
{
    if ((value.kind != Json::Kind::Number && value.kind != Json::Kind::String) || value.text.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    long long const result = std::strtoll(value.text.c_str(), &end, 10);
    if (errno != 0 || end != value.text.c_str() + value.text.size())
    {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(result);
}

std::optional<std::uint64_t> toUnsigned(Json const& value)
{
    if ((value.kind != Json::Kind::Number && value.kind != Json::Kind::String) || value.text.empty() || value.text.front() == '-')
    {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long const result = std::strtoull(value.text.c_str(), &end, 10);
    if (errno != 0 || end != value.text.c_str() + value.text.size())
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(result);
}

std::optional<bool> toBool(Json const& value)
{
    if (value.kind == Json::Kind::Bool)
    {
        return value.boolean;
    }
    if (value.text == "true" || value.text == "1")
    {
        return true;
    }
    if (value.text == "false" || value.text == "0")
    {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> toString(Json const& value)
{
    if (value.kind != Json::Kind::String)
    {
        return std::nullopt;
    }
    return value.text;
}

// Reads the fields of a request body. Each read returns false when the field is absent;
// a field of the wrong type also sets error() (the first one wins).
class Fields
{
public:
    explicit Fields(Json const& body)
        : body_(body)
    {
    }

    [[nodiscard]] bool has(char const* key) const { return body_.get(key) != nullptr; }
    bool number(char const* key, double& value) { return read(key, value, toDouble, "a number"); }
    bool integer(char const* key, std::int64_t& value) { return read(key, value, toInt, "an integer"); }
    bool unsignedInt(char const* key, std::uint64_t& value) { return read(key, value, toUnsigned, "a non-negative integer"); }
    bool boolean(char const* key, bool& value) { return read(key, value, toBool, "true or false"); }
    bool string(char const* key, std::string& value) { return read(key, value, toString, "a string"); }
    void invalid(char const* key, std::string const& expected)
    {
        if (error_.empty())
        {
            error_ = std::string(key) + " must be " + expected;
        }
    }
    [[nodiscard]] std::string const& error() const { return error_; }

private:
    template <typename T, typename Convert>
    bool read(char const* key, T& value, Convert convert, char const* expected)
    {
        auto const* field = body_.get(key);
        if (field == nullptr)
        {
            return false;
        }
        auto const converted = convert(*field);
        if (!converted)
        {
            invalid(key, expected);
            return false;
        }
        value = *converted;
        return true;
    }

    Json const& body_;
    std::string error_;
};

std::string queryValue(std::string const& query, std::string const& key)
{
    std::size_t start = 0;
    while (start <= query.size())
    {
        auto const end = std::min(query.find('&', start), query.size());
        auto const item = query.substr(start, end - start);
        if (item.rfind(key + "=", 0) == 0)
        {
            // application/x-www-form-urlencoded: %XX and '+'.
            std::string value;
            for (std::size_t i = key.size() + 1; i < item.size(); ++i)
            {
                if (item[i] == '+')
                {
                    value.push_back(' ');
                }
                else if (item[i] == '%' && i + 2 < item.size() && std::isxdigit(static_cast<unsigned char>(item[i + 1])) != 0 &&
                         std::isxdigit(static_cast<unsigned char>(item[i + 2])) != 0)
                {
                    value.push_back(static_cast<char>(std::stoi(item.substr(i + 1, 2), nullptr, 16)));
                    i += 2;
                }
                else
                {
                    value.push_back(item[i]);
                }
            }
            return value;
        }
        start = end + 1;
    }
    return {};
}

int pathIndex(std::string const& path, std::string const& prefix)
{
    if (path.rfind(prefix, 0) != 0)
    {
        return 0;
    }
    try
    {
        return std::stoi(path.substr(prefix.size()));
    }
    catch (...)
    {
        return 0;
    }
}

// The path segment after `prefix` ("/api/v1/clips/" + "clip-3/export" gives "clip-3").
std::string pathId(std::string const& path, std::string const& prefix)
{
    auto id = path.substr(prefix.size());
    return id.substr(0, id.find('/'));
}

std::string suffixAfter(std::string const& path, std::string const& mark)
{
    auto const pos = path.rfind(mark);
    if (pos == std::string::npos)
    {
        return {};
    }
    return path.substr(pos + mark.size());
}

HttpResponse json(int status, std::string body)
{
    HttpResponse response;
    response.status = status;
    response.contentType = "application/json";
    response.body = std::move(body);
    return response;
}

HttpResponse error(int status, std::string const& message)
{
    return json(status, "{\"error\":\"" + jsonEscape(message) + "\"}");
}

// A required field that is absent or of the wrong type.
HttpResponse missing(Fields const& fields, std::string const& key)
{
    return error(400, fields.error().empty() ? key + " is required" : fields.error());
}

// `maxAge` seconds of browser cache: thumbnails of a clip's IN frame do not change.
HttpResponse jpeg(std::vector<std::uint8_t> const& bytes, int maxAge = 0)
{
    if (bytes.empty())
    {
        return error(404, "no picture");
    }
    HttpResponse response;
    response.contentType = "image/jpeg";
    response.body.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    response.cacheControl = maxAge > 0 ? "max-age=" + std::to_string(maxAge) : "no-store";
    return response;
}

std::string timecode(Config const& cfg, std::uint64_t ns)
{
    return formatTimecode(ns, cfg.format.rateNum, cfg.format.rateDen);
}

std::string clipJson(ClipRef const& clip, Config const& cfg)
{
    std::ostringstream out;
    out << "{\"id\":\"" << jsonEscape(clip.id) << "\",\"name\":\"" << jsonEscape(clip.name) << "\",\"camera\":" << clip.camera << ",\"in_ns\":" << clip.inNs
        << ",\"out_ns\":" << clip.outNs << ",\"in_tc\":\"" << timecode(cfg, clip.inNs) << "\",\"out_tc\":\"" << timecode(cfg, clip.outNs)
        << "\",\"speed\":" << clip.speed << ",\"motion\":\"" << jsonEscape(clip.motion) << "\",\"audio\":\"" << jsonEscape(clip.audio) << "\",\"colour\":\""
        << jsonEscape(clip.colour) << "\",\"tags\":\"" << jsonEscape(clip.tags) << "\",\"end\":\"" << endActionName(clip.end)
        << "\",\"library\":" << (clip.library ? "true" : "false") << ",\"group\":\"" << jsonEscape(clip.groupId) << "\"}";
    return out.str();
}

// `entries`: the entry list, or only their count (the list of all playlists).
std::string playlistJson(Catalog::Playlist const& playlist, std::vector<ClipRef> const& clips, bool entries)
{
    // Each entry plays its clip at the entry's speed.
    double duration = 0;
    for (auto const& entry : playlist.entries)
    {
        for (auto const& clip : clips)
        {
            if (clip.id == entry.clipId)
            {
                double const speed = std::fabs(entry.speed);
                duration += static_cast<double>(clip.outNs - clip.inNs) / (speed > 0.001 ? speed : 1.0);
            }
        }
    }
    std::ostringstream out;
    out << "{\"id\":\"" << jsonEscape(playlist.id) << "\",\"name\":\"" << jsonEscape(playlist.name) << "\",\"entries\":";
    if (!entries)
    {
        out << playlist.entries.size();
    }
    else
    {
        out << '[';
        for (std::size_t i = 0; i < playlist.entries.size(); ++i)
        {
            auto const& entry = playlist.entries[i];
            out << (i > 0 ? "," : "") << "{\"clip_id\":\"" << jsonEscape(entry.clipId) << "\",\"speed\":" << entry.speed << ",\"end\":\""
                << endActionName(entry.end) << "\",\"auto_advance\":" << (entry.autoAdvance ? "true" : "false") << '}';
        }
        out << ']';
    }
    out << ",\"duration_ns\":" << static_cast<std::uint64_t>(duration) << '}';
    return out.str();
}

// Playlist entries from `entries` (objects) or `clips` (clip ids, as an array or a comma list).
bool readEntries(Json const& body, std::vector<PlaylistEntry>& entries, std::string& problem)
{
    if (auto const* list = body.get("entries"))
    {
        if (list->kind != Json::Kind::Array)
        {
            problem = "entries must be an array";
            return false;
        }
        for (auto const& item : list->items)
        {
            Fields fields(item);
            PlaylistEntry entry;
            if (item.kind != Json::Kind::Object || !fields.string("clip_id", entry.clipId) || entry.clipId.empty())
            {
                problem = fields.error().empty() ? "each entry needs a clip_id" : fields.error();
                return false;
            }
            fields.number("speed", entry.speed);
            std::string end;
            if (fields.string("end", end))
            {
                bool ok = false;
                entry.end = parseEndAction(end, &ok);
                if (!ok)
                {
                    fields.invalid("end", "freeze, black, loop, next or return-to-live");
                }
            }
            fields.boolean("auto_advance", entry.autoAdvance);
            if (!fields.error().empty())
            {
                problem = fields.error();
                return false;
            }
            if (entry.speed < -1.0 || entry.speed > 2.0)
            {
                problem = "speed must be between -1 and 2";
                return false;
            }
            entries.push_back(entry);
        }
        return true;
    }
    auto const* clips = body.get("clips");
    if (clips == nullptr)
    {
        return true;
    }
    std::vector<std::string> ids;
    if (clips->kind == Json::Kind::Array)
    {
        for (auto const& item : clips->items)
        {
            if (item.kind != Json::Kind::String)
            {
                problem = "clips must be clip ids";
                return false;
            }
            ids.push_back(item.text);
        }
    }
    else if (clips->kind == Json::Kind::String)
    {
        std::istringstream list(clips->text);
        std::string id;
        while (std::getline(list, id, ','))
        {
            ids.push_back(id);
        }
    }
    else
    {
        problem = "clips must be an array of clip ids";
        return false;
    }
    for (auto const& id : ids)
    {
        if (!id.empty())
        {
            PlaylistEntry entry;
            entry.clipId = id;
            entry.end = EndAction::Next;
            entry.autoAdvance = true;
            entries.push_back(entry);
        }
    }
    return true;
}

// Every clip an entry names must exist.
std::string unknownClip(std::vector<PlaylistEntry> const& entries, std::vector<ClipRef> const& clips)
{
    for (auto const& entry : entries)
    {
        bool found = false;
        for (auto const& clip : clips)
        {
            found = found || clip.id == entry.clipId;
        }
        if (!found)
        {
            return entry.clipId;
        }
    }
    return {};
}

std::string nmosJson(Engine& engine)
{
    auto const& cfg = engine.config();
    auto const& ids = engine.ids();
    std::ostringstream out;
    out << "{\"node_id\":\"" << ids.node << "\",\"device_id\":\"" << ids.device << "\",\"node_label\":\"" << jsonEscape(nodeLabel(cfg))
        << "\",\"device_label\":\"" << jsonEscape(deviceLabel(cfg)) << "\",\"host_address\":\"" << jsonEscape(cfg.nmosHostAddress)
        << "\",\"nmos_port\":" << cfg.nmosPort << ",\"registry\":\""
        << (cfg.nmosRegistryAddress.empty() ? std::string{} : jsonEscape(cfg.nmosRegistryAddress) + ":" + std::to_string(cfg.nmosRegistryPort))
        << "\",\"receivers\":[";
    bool first = true;
    auto const receiver = [&](CameraConfig const& camera, int phase, bool video) {
        auto const route = engine.route(camera.index, phase, video);
        out << (first ? "" : ",") << "{\"camera\":" << camera.index << ",\"label\":\"" << jsonEscape(camera.label) << "\",\"kind\":\""
            << (video ? "video" : "audio") << "\",\"phase\":" << phase << ",\"id\":\""
            << (video ? ids.videoReceiver(camera.index, phase) : ids.audioReceiver(camera.index)) << "\",\"active\":" << (route.active ? "true" : "false")
            << ",\"state\":\"" << jsonEscape(route.state) << "\",\"sender_id\":\"" << jsonEscape(route.senderId) << "\",\"flow_id\":\""
            << jsonEscape(route.flowId) << "\",\"domain_id\":\"" << jsonEscape(route.domainId) << "\"}";
        first = false;
    };
    for (auto const& camera : cfg.cameras)
    {
        for (int phase = 1; phase <= camera.phases; ++phase)
        {
            receiver(camera, phase, true);
        }
        receiver(camera, 1, false);
    }
    out << "],\"senders\":[";
    for (std::size_t i = 0; i < cfg.channelList.size(); ++i)
    {
        auto const& channel = cfg.channelList[i];
        out << (i > 0 ? "," : "") << "{\"channel\":" << channel.index << ",\"label\":\"" << jsonEscape(channel.label) << "\",\"video_sender\":\""
            << ids.videoSender(channel.index) << "\",\"video_flow\":\"" << ids.videoFlow(channel.index, cfg.format.token()) << "\",\"audio_sender\":\""
            << ids.audioSender(channel.index) << "\",\"audio_flow\":\"" << ids.audioFlow(channel.index) << "\",\"data_sender\":\""
            << ids.dataSender(channel.index) << "\",\"data_flow\":\"" << ids.dataFlow(channel.index, cfg.format.token()) << "\"}";
    }
    out << "],\"domain_id\":\"" << jsonEscape(cfg.outputDomainId) << "\"}";
    return out.str();
}

HttpResponse channelCommand(Engine& engine, int channel, std::string const& action, Json const& body)
{
    Fields fields(body);
    if (action == "transport")
    {
        std::string command;
        fields.string("command", command);
        if (command == "play")
        {
            engine.play(channel);
        }
        else if (command == "pause")
        {
            engine.pause(channel);
        }
        else if (command == "live")
        {
            engine.live(channel);
        }
        else
        {
            return error(400, "command must be play, pause or live");
        }
    }
    else if (action == "speed")
    {
        double speed = 0;
        if (!fields.number("speed", speed))
        {
            return missing(fields, "speed");
        }
        if (speed < -1.0 || speed > 2.0)
        {
            return error(400, "speed must be between -1 and 2");
        }
        engine.setSpeed(channel, speed);
    }
    else if (action == "position")
    {
        std::int64_t frames = 0;
        double seconds = 0;
        std::uint64_t tai = 0;
        if (fields.integer("frames", frames))
        {
            engine.scrubFrames(channel, static_cast<int>(std::clamp<std::int64_t>(frames, -10000000, 10000000)));
        }
        else if (fields.number("seconds", seconds))
        {
            engine.scrubSeconds(channel, seconds);
        }
        else if (fields.unsignedInt("tai_ns", tai))
        {
            engine.setPosition(channel, tai);
        }
        else
        {
            return error(400, fields.error().empty() ? "frames, seconds or tai_ns is required" : fields.error());
        }
    }
    else if (action == "marks")
    {
        std::string which;
        fields.string("which", which);
        if (which == "in")
        {
            engine.markIn(channel);
        }
        else if (which == "out")
        {
            engine.markOut(channel);
        }
        else if (which == "goto-in")
        {
            engine.gotoIn(channel);
        }
        else if (which == "goto-out")
        {
            engine.gotoOut(channel);
        }
        else
        {
            return error(400, "which must be in, out, goto-in or goto-out");
        }
    }
    else if (action == "angle")
    {
        std::int64_t camera = 0;
        if (!fields.integer("camera", camera))
        {
            return missing(fields, "camera");
        }
        if (camera < 1 || camera > engine.config().inputs)
        {
            return error(400, "camera must be between 1 and " + std::to_string(engine.config().inputs));
        }
        engine.setAngle(channel, static_cast<int>(camera));
    }
    else if (action == "mode")
    {
        // Every given mode is checked before any is applied.
        std::string motion;
        std::string audio;
        std::string tc;
        bool const hasMotion = fields.string("motion", motion);
        bool const hasAudio = fields.string("audio", audio);
        bool const hasTc = fields.string("timecode", tc);
        bool motionOk = true;
        bool audioOk = true;
        auto const motionMode = hasMotion ? parseMotion(motion, &motionOk) : MotionMode::Repeat;
        auto const audioMode = hasAudio ? parseAudioMode(audio, &audioOk) : AudioMode::Mute;
        if (!motionOk)
        {
            fields.invalid("motion", "repeat, blend or interpolate");
        }
        if (!audioOk)
        {
            fields.invalid("audio", "mute, stretch or follow");
        }
        if (hasTc && tc != "source" && tc != "output")
        {
            fields.invalid("timecode", "source or output");
        }
        if (!fields.error().empty())
        {
            return error(400, fields.error());
        }
        if (!hasMotion && !hasAudio && !hasTc)
        {
            return error(400, "motion, audio or timecode is required");
        }
        if (hasMotion)
        {
            engine.setMotion(channel, motionMode);
        }
        if (hasAudio)
        {
            engine.setAudioMode(channel, audioMode);
        }
        if (hasTc)
        {
            engine.setTimecodeMode(channel, tc == "output" ? TcMode::Output : TcMode::Source);
        }
    }
    else if (action == "lock")
    {
        bool enable = false;
        if (!fields.boolean("enable", enable))
        {
            return missing(fields, "enable");
        }
        engine.setLock(channel, enable);
    }
    else
    {
        return error(404, "not found");
    }
    return json(200, engine.statusJson());
}

// PATCH /api/v1/clips/{id}: the given fields change, the others stay.
HttpResponse patchClip(Engine& engine, std::string const& id, Json const& body)
{
    auto clip = engine.clip(id);
    if (clip.id.empty())
    {
        return error(404, "unknown clip");
    }
    Fields fields(body);
    fields.string("name", clip.name);
    fields.unsignedInt("in_ns", clip.inNs);
    fields.unsignedInt("out_ns", clip.outNs);
    fields.number("speed", clip.speed);
    fields.string("colour", clip.colour);
    fields.string("tags", clip.tags);
    std::string text;
    bool ok = true;
    if (fields.string("motion", text))
    {
        (void)parseMotion(text, &ok);
        if (ok)
        {
            clip.motion = text;
        }
        else
        {
            fields.invalid("motion", "repeat, blend or interpolate");
        }
    }
    if (fields.string("audio", text))
    {
        (void)parseAudioMode(text, &ok);
        if (ok)
        {
            clip.audio = text;
        }
        else
        {
            fields.invalid("audio", "mute, stretch or follow");
        }
    }
    if (fields.string("end", text))
    {
        clip.end = parseEndAction(text, &ok);
        if (!ok)
        {
            fields.invalid("end", "freeze, black, loop, next or return-to-live");
        }
    }
    if (!fields.error().empty())
    {
        return error(400, fields.error());
    }
    if (clip.outNs < clip.inNs)
    {
        return error(400, "out_ns must not be before in_ns");
    }
    if (clip.speed < -1.0 || clip.speed > 2.0)
    {
        return error(400, "speed must be between -1 and 2");
    }
    if (!engine.updateClip(clip))
    {
        return error(404, "unknown clip");
    }
    return json(200, clipJson(engine.clip(id), engine.config()));
}
} // namespace

HttpResponse handleApi(Engine& engine, HttpRequest const& request, std::string const& indexHtml)
{
    auto const& path = request.path;
    auto const& method = request.method;
    if (path == "/livez")
    {
        return json(200, "{\"status\":\"ok\"}");
    }
    // The retained segments are indexed after HTTP starts; until then only /livez works.
    if (!engine.bufferReady())
    {
        return json(503, "{\"status\":\"indexing\"}");
    }
    if (path == "/readyz")
    {
        auto const& cfg = engine.config();
        if (cfg.nmosEnable && !cfg.nmosRegistryAddress.empty())
        {
            auto const query = "/x-nmos/query/v1.3/nodes/" + engine.ids().node;
            if (httpGetStatus(cfg.nmosQueryAddress, cfg.nmosQueryPort, query, 500) != 200)
            {
                return json(503, "{\"status\":\"registering\"}");
            }
        }
        return json(200, "{\"status\":\"ready\"}");
    }
    if (path == "/statusz" || path == "/api/v1/status")
    {
        return json(200, engine.statusJson());
    }
    if (path == "/metrics")
    {
        HttpResponse response;
        response.contentType = "text/plain; version=0.0.4";
        engine.updateMetrics();
        auto& metrics = engine.metrics();
        metrics.set("free_bytes", {}, static_cast<double>(engine.freeBytes()));
        metrics.set("write_bytes_per_second", {}, engine.storageBytesPerSecond());
        response.body = metrics.render();
        return response;
    }
    if (path == "/api/v1/events" && method == "GET")
    {
        HttpResponse response;
        response.websocket = true;
        return response;
    }
    if (path == "/" || path == "/index.html")
    {
        HttpResponse response;
        response.contentType = "text/html; charset=utf-8";
        response.body = indexHtml.empty()
                            ? std::string("<!doctype html><title>MXL Replay</title><h1>MXL Replay</h1><p>API at /api/v1/status</p>")
                            : indexHtml;
        return response;
    }
    if (path == "/api/v1/config" && method == "GET")
    {
        return json(200, std::string("{\"export\":") + "\"" + "see /api/v1/config/export" + "\"}");
    }
    if (path == "/api/v1/config/export" && (method.empty() || method == "GET"))
    {
        return json(200, engine.exportConfigJson());
    }
    if (path == "/api/v1/config/import" && method == "POST")
    {
        auto const imported = engine.importConfigJson(request.body);
        if (!imported.ok)
        {
            return error(400, imported.error);
        }
        return json(200, std::string("{\"ok\":true,\"restart_required\":") + (imported.restartRequired ? "true" : "false") + "}");
    }
    if (path == "/api/v1/nmos")
    {
        return json(200, nmosJson(engine));
    }
    if (path.rfind("/api/v1/cameras/", 0) == 0 && suffixAfter(path, "/") == "preview.jpg" && method == "GET")
    {
        int const camera = pathIndex(path, "/api/v1/cameras/");
        if (camera < 1 || camera > engine.config().inputs)
        {
            return error(404, "unknown camera");
        }
        return jpeg(engine.cameraPreviewJpeg(camera));
    }
    if (path.rfind("/api/v1/clips/", 0) == 0 && suffixAfter(path, "/") == "thumbnail.jpg" && method == "GET")
    {
        return jpeg(engine.clipThumbnailJpeg(pathId(path, "/api/v1/clips/")), 3600);
    }
    if (path.rfind("/api/v1/channels/", 0) == 0 && suffixAfter(path, "/") == "preview.jpg" && method == "GET")
    {
        int const channel = pathIndex(path, "/api/v1/channels/");
        if (channel < 1 || channel > engine.config().channels)
        {
            return error(404, "unknown channel");
        }
        return jpeg(engine.previewJpeg(channel));
    }
    if (path == "/api/v1/uploads" && method == "POST")
    {
        // The file is the whole body; its name comes from the query (?name=…).
        if (request.body.empty())
        {
            return error(400, "the body must be the file");
        }
        std::string problem;
        auto const id = engine.upload(reinterpret_cast<std::uint8_t const*>(request.body.data()), request.body.size(), queryValue(request.query, "name"), problem);
        if (id.empty())
        {
            return error(400, problem);
        }
        return json(201, std::string("{\"id\":\"") + jsonEscape(id) + "\"}");
    }

    // Everything below takes a JSON object (or nothing) as its body.
    Json body;
    if (!parseBody(request.body, body))
    {
        return error(400, "the body must be a JSON object");
    }
    Fields fields(body);
    auto const channelOf = [&](int& channel) {
        std::int64_t value = 1;
        fields.integer("channel", value);
        channel = static_cast<int>(std::clamp<std::int64_t>(value, 0, 1000));
        return fields.error().empty() && channel >= 1 && channel <= engine.config().channels;
    };

    if (path.rfind("/api/v1/channels/", 0) == 0)
    {
        int const channel = pathIndex(path, "/api/v1/channels/");
        if (channel < 1 || channel > engine.config().channels)
        {
            return error(404, "unknown channel");
        }
        if (method != "POST")
        {
            return error(404, "not found");
        }
        return channelCommand(engine, channel, suffixAfter(path, "/"), body);
    }
    if (path == "/api/v1/clips" && method == "GET")
    {
        std::string out = "[";
        for (auto const& clip : engine.clips())
        {
            out += (out.size() > 1 ? "," : "") + clipJson(clip, engine.config());
        }
        return json(200, out + "]");
    }
    if (path == "/api/v1/clips" && method == "POST")
    {
        int channel = 1;
        if (!channelOf(channel))
        {
            return error(fields.error().empty() ? 404 : 400, fields.error().empty() ? "unknown channel" : fields.error());
        }
        std::string name;
        std::string colour;
        std::string tags;
        bool allAngles = false;
        bool force = false;
        fields.string("name", name);
        fields.string("colour", colour);
        fields.string("tags", tags);
        fields.boolean("all_angles", allAngles);
        fields.boolean("force", force);
        if (!fields.error().empty())
        {
            return error(400, fields.error());
        }
        std::string problem;
        auto const id = engine.createClip(channel, name, allAngles, force, problem, colour, tags);
        if (id.empty())
        {
            // 409 only for the protection cap: the caller may retry with force.
            return error(problem.rfind("protected ranges", 0) == 0 ? 409 : 400, problem);
        }
        return json(201, std::string("{\"id\":\"") + jsonEscape(id) + "\"}");
    }
    if (path.rfind("/api/v1/clips/", 0) == 0)
    {
        auto const id = pathId(path, "/api/v1/clips/");
        auto const action = path.find('/', std::string("/api/v1/clips/").size()) == std::string::npos ? std::string{} : suffixAfter(path, "/");
        if (action.empty() && method == "PATCH")
        {
            return patchClip(engine, id, body);
        }
        if (action.empty() && method == "DELETE")
        {
            return engine.deleteClip(id) ? json(200, "{\"deleted\":true}") : error(404, "unknown clip");
        }
        if (action == "export" && method == "POST")
        {
            std::string problem;
            auto const dir = engine.exportClip(id, problem);
            if (dir.empty())
            {
                return error(problem == "unknown clip" ? 404 : 400, problem);
            }
            return json(200, std::string("{\"path\":\"") + jsonEscape(dir) + "\"}");
        }
        if (action == "consolidate" && method == "POST")
        {
            std::string problem;
            if (!engine.consolidate(id, problem))
            {
                return error(problem == "unknown clip" ? 404 : 400, problem);
            }
            return json(200, "{\"consolidated\":true}");
        }
        return error(404, "not found");
    }
    if (path.rfind("/api/v1/shotbox/", 0) == 0 && method == "POST")
    {
        auto const id = pathId(path, "/api/v1/shotbox/");
        int channel = 1;
        if (!channelOf(channel))
        {
            return error(fields.error().empty() ? 404 : 400, fields.error().empty() ? "unknown channel" : fields.error());
        }
        if (engine.clip(id).id.empty() && engine.playlist(id).id.empty())
        {
            return error(404, "unknown clip or playlist");
        }
        auto const state = engine.shotClick(channel, id);
        return json(200, std::string("{\"state\":\"") + shotStateName(state) + "\"}");
    }
    if (path == "/api/v1/playlists" && method == "GET")
    {
        auto const clips = engine.clips();
        std::string out = "[";
        for (auto const& playlist : engine.playlists())
        {
            out += (out.size() > 1 ? "," : "") + playlistJson(playlist, clips, false);
        }
        return json(200, out + "]");
    }
    if (path == "/api/v1/playlists" && method == "POST")
    {
        std::string name;
        fields.string("name", name);
        std::vector<PlaylistEntry> entries;
        std::string problem;
        if (!fields.error().empty() || !readEntries(body, entries, problem))
        {
            return error(400, fields.error().empty() ? problem : fields.error());
        }
        if (auto const unknown = unknownClip(entries, engine.clips()); !unknown.empty())
        {
            return error(400, "unknown clip " + unknown);
        }
        auto const id = engine.createPlaylist(name, entries);
        return json(201, std::string("{\"id\":\"") + jsonEscape(id) + "\"}");
    }
    if (path.rfind("/api/v1/playlists/", 0) == 0)
    {
        auto const id = pathId(path, "/api/v1/playlists/");
        auto const action = path.find('/', std::string("/api/v1/playlists/").size()) == std::string::npos ? std::string{} : suffixAfter(path, "/");
        auto playlist = engine.playlist(id);
        if (playlist.id.empty())
        {
            return error(404, "unknown playlist");
        }
        if (action.empty() && method == "GET")
        {
            return json(200, playlistJson(playlist, engine.clips(), true));
        }
        if (action.empty() && method == "PUT")
        {
            // The given name and entries replace the stored ones.
            fields.string("name", playlist.name);
            std::vector<PlaylistEntry> entries;
            std::string problem;
            if (!fields.error().empty() || !readEntries(body, entries, problem))
            {
                return error(400, fields.error().empty() ? problem : fields.error());
            }
            auto const clips = engine.clips();
            if (auto const unknown = unknownClip(entries, clips); !unknown.empty())
            {
                return error(400, "unknown clip " + unknown);
            }
            if (fields.has("entries") || fields.has("clips"))
            {
                playlist.entries = entries;
            }
            if (!engine.updatePlaylist(playlist))
            {
                return error(404, "unknown playlist");
            }
            return json(200, playlistJson(engine.playlist(id), clips, true));
        }
        if (action.empty() && method == "DELETE")
        {
            return engine.deletePlaylist(id) ? json(200, "{\"deleted\":true}") : error(404, "unknown playlist");
        }
        if (action == "play" && method == "POST")
        {
            int channel = 1;
            if (!channelOf(channel))
            {
                return error(fields.error().empty() ? 404 : 400, fields.error().empty() ? "unknown channel" : fields.error());
            }
            ClipRef first;
            for (auto const& entry : playlist.entries)
            {
                if (first.id.empty())
                {
                    first = engine.clip(entry.clipId);
                }
            }
            if (first.id.empty())
            {
                return error(400, "the playlist has no clip to play");
            }
            engine.playPlaylist(channel, id);
            return json(200, engine.statusJson());
        }
        return error(404, "not found");
    }
    if (path == "/api/v1/control" && method == "POST")
    {
        int channel = 1;
        if (!channelOf(channel))
        {
            return error(fields.error().empty() ? 404 : 400, fields.error().empty() ? "unknown channel" : fields.error());
        }
        std::string action;
        fields.string("action", action);
        if (action == "play")
        {
            engine.play(channel);
        }
        else if (action == "pause")
        {
            engine.pause(channel);
        }
        else if (action == "live")
        {
            engine.live(channel);
        }
        else if (action == "in")
        {
            engine.markIn(channel);
        }
        else if (action == "out")
        {
            engine.markOut(channel);
        }
        else if (action == "scrub")
        {
            std::int64_t frames = 0;
            if (!fields.integer("value", frames))
            {
                return missing(fields, "value");
            }
            engine.scrubFrames(channel, static_cast<int>(std::clamp<std::int64_t>(frames, -10000000, 10000000)));
        }
        else if (action == "speed")
        {
            double speed = 1;
            if (!fields.number("value", speed))
            {
                return missing(fields, "value");
            }
            if (speed < -1.0 || speed > 2.0)
            {
                return error(400, "value must be between -1 and 2");
            }
            engine.setSpeed(channel, speed);
        }
        else
        {
            return error(400, "action must be play, pause, live, in, out, scrub or speed");
        }
        return json(200, engine.statusJson());
    }
    return error(404, "not found");
}
} // namespace replay
