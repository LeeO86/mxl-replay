#include "ops/api.hpp"

#include "config/config.hpp"
#include "ops/httpserver.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <sstream>

namespace replay
{
namespace
{
std::string queryValue(std::string const& query, std::string const& key)
{
    auto const pos = query.find(key + "=");
    if (pos == std::string::npos)
    {
        return {};
    }
    auto const start = pos + key.size() + 1;
    auto const end = query.find('&', start);
    return query.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string jsonField(std::string const& body, std::string const& key)
{
    auto const needle = "\"" + key + "\"";
    auto const pos = body.find(needle);
    if (pos == std::string::npos)
    {
        return {};
    }
    auto const colon = body.find(':', pos + needle.size());
    if (colon == std::string::npos)
    {
        return {};
    }
    auto i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\n'))
    {
        ++i;
    }
    if (i >= body.size())
    {
        return {};
    }
    if (body[i] == '"')
    {
        auto const end = body.find('"', i + 1);
        return end == std::string::npos ? std::string{} : body.substr(i + 1, end - i - 1);
    }
    auto end = i;
    while (end < body.size() && body[end] != ',' && body[end] != '}' && body[end] != ' ' && body[end] != '\n')
    {
        ++end;
    }
    return body.substr(i, end - i);
}

bool jsonBool(std::string const& body, std::string const& key, bool fallback)
{
    auto const value = jsonField(body, key);
    if (value.empty())
    {
        return fallback;
    }
    return value == "true" || value == "1";
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
} // namespace

HttpResponse handleApi(Engine& engine, HttpRequest const& request, std::string const& indexHtml)
{
    auto const& path = request.path;
    auto const& method = request.method;
    if (path == "/livez")
    {
        return json(200, "{\"status\":\"ok\"}");
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
            return json(400, std::string("{\"error\":\"") + jsonEscape(imported.error) + "\"}");
        }
        return json(200, std::string("{\"ok\":true,\"restart_required\":") + (imported.restartRequired ? "true" : "false") + "}");
    }
    if (path == "/api/v1/nmos")
    {
        auto const& ids = engine.ids();
        return json(200, std::string("{\"node_id\":\"") + ids.node + "\",\"device_id\":\"" + ids.device + "\",\"device_label\":\"" +
                            jsonEscape(deviceLabel(engine.config())) + "\",\"host_address\":\"" + engine.config().nmosHostAddress + "\"}");
    }
    if (path == "/api/v1/clips" && method == "GET")
    {
        std::ostringstream out;
        out << '[';
        bool first = true;
        for (auto const& clip : engine.clips())
        {
            if (!first)
            {
                out << ',';
            }
            first = false;
            out << "{\"id\":\"" << clip.id << "\",\"name\":\"" << clip.name << "\",\"camera\":" << clip.camera << ",\"in_ns\":" << clip.inNs
                << ",\"out_ns\":" << clip.outNs << ",\"speed\":" << clip.speed << ",\"motion\":\"" << clip.motion << "\",\"audio\":\"" << clip.audio
                << "\",\"colour\":\"" << clip.colour << "\",\"tags\":\"" << clip.tags << "\",\"end\":\"" << endActionName(clip.end)
                << "\",\"library\":" << (clip.library ? "true" : "false") << ",\"group\":\"" << clip.groupId << "\"}";
        }
        out << ']';
        return json(200, out.str());
    }
    if (path == "/api/v1/playlists" && method == "GET")
    {
        std::ostringstream out;
        out << '[';
        bool first = true;
        for (auto const& playlist : engine.playlists())
        {
            if (!first)
            {
                out << ',';
            }
            first = false;
            out << "{\"id\":\"" << playlist.id << "\",\"name\":\"" << playlist.name << "\",\"entries\":" << playlist.entries.size() << '}';
        }
        out << ']';
        return json(200, out.str());
    }
    if (path.rfind("/api/v1/channels/", 0) == 0)
    {
        int const channel = pathIndex(path, "/api/v1/channels/");
        auto const action = suffixAfter(path, "/");
        if (action == "preview.jpg")
        {
            auto const jpeg = engine.previewJpeg(channel);
            HttpResponse response;
            response.contentType = "image/jpeg";
            response.body.assign(reinterpret_cast<char const*>(jpeg.data()), jpeg.size());
            return response;
        }
        if (method != "POST")
        {
            return json(404, "{\"error\":\"not found\"}");
        }
        if (action == "transport")
        {
            auto const command = jsonField(request.body, "command");
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
                return json(400, "{\"error\":\"unknown command\"}");
            }
        }
        else if (action == "speed")
        {
            engine.setSpeed(channel, std::stod(jsonField(request.body, "speed")));
        }
        else if (action == "position")
        {
            if (request.body.find("frames") != std::string::npos)
            {
                engine.scrubFrames(channel, std::stoi(jsonField(request.body, "frames")));
            }
            else if (request.body.find("seconds") != std::string::npos)
            {
                engine.scrubSeconds(channel, std::stod(jsonField(request.body, "seconds")));
            }
            else
            {
                engine.setPosition(channel, static_cast<std::uint64_t>(std::stoll(jsonField(request.body, "tai_ns"))));
            }
        }
        else if (action == "marks")
        {
            auto const which = jsonField(request.body, "which");
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
        }
        else if (action == "angle")
        {
            engine.setAngle(channel, std::stoi(jsonField(request.body, "camera")));
        }
        else if (action == "mode")
        {
            bool ok = false;
            if (request.body.find("motion") != std::string::npos)
            {
                engine.setMotion(channel, parseMotion(jsonField(request.body, "motion"), &ok));
            }
            if (request.body.find("audio") != std::string::npos)
            {
                engine.setAudioMode(channel, parseAudioMode(jsonField(request.body, "audio"), &ok));
            }
            if (request.body.find("timecode") != std::string::npos)
            {
                engine.setTimecodeMode(channel, jsonField(request.body, "timecode") == "output" ? TcMode::Output : TcMode::Source);
            }
        }
        else if (action == "lock")
        {
            engine.setLock(channel, jsonBool(request.body, "enable", false));
        }
        else
        {
            return json(404, "{\"error\":\"not found\"}");
        }
        return json(200, engine.statusJson());
    }
    if (path == "/api/v1/clips" && method == "POST")
    {
        int const channel = std::stoi(jsonField(request.body, "channel").empty() ? "1" : jsonField(request.body, "channel"));
        std::string error;
        auto const id = engine.createClip(channel, jsonField(request.body, "name"), jsonBool(request.body, "all_angles", false),
            jsonBool(request.body, "force", false), error);
        if (id.empty())
        {
            return json(409, std::string("{\"error\":\"") + error + "\"}");
        }
        return json(201, std::string("{\"id\":\"") + id + "\"}");
    }
    if (path.rfind("/api/v1/clips/", 0) == 0 && method == "DELETE")
    {
        auto id = path.substr(std::string("/api/v1/clips/").size());
        engine.deleteClip(id);
        return json(200, "{\"deleted\":true}");
    }
    if (path.rfind("/api/v1/shotbox/", 0) == 0 && method == "POST")
    {
        auto id = path.substr(std::string("/api/v1/shotbox/").size());
        auto const slash = id.find('/');
        if (slash != std::string::npos)
        {
            id = id.substr(0, slash);
        }
        int const channel = std::stoi(jsonField(request.body, "channel").empty() ? "1" : jsonField(request.body, "channel"));
        auto const state = engine.shotClick(channel, id);
        return json(200, std::string("{\"state\":\"") + shotStateName(state) + "\"}");
    }
    if (path == "/api/v1/playlists" && method == "POST")
    {
        std::vector<PlaylistEntry> entries;
        auto list = jsonField(request.body, "clips");
        while (!list.empty())
        {
            auto comma = list.find(',');
            auto id = list.substr(0, comma);
            if (!id.empty())
            {
                PlaylistEntry entry;
                entry.clipId = id;
                entry.end = EndAction::Next;
                entry.autoAdvance = true;
                entries.push_back(entry);
            }
            if (comma == std::string::npos)
            {
                break;
            }
            list = list.substr(comma + 1);
        }
        auto const id = engine.createPlaylist(jsonField(request.body, "name"), entries);
        return json(201, std::string("{\"id\":\"") + id + "\"}");
    }
    if (path.rfind("/api/v1/playlists/", 0) == 0 && path.find("/play") != std::string::npos && method == "POST")
    {
        auto id = path.substr(std::string("/api/v1/playlists/").size());
        id = id.substr(0, id.find('/'));
        int const channel = std::stoi(jsonField(request.body, "channel").empty() ? "1" : jsonField(request.body, "channel"));
        engine.playPlaylist(channel, id);
        return json(200, engine.statusJson());
    }
    if (path == "/api/v1/uploads" && method == "POST")
    {
        std::string error;
        auto const id = engine.upload(reinterpret_cast<std::uint8_t const*>(request.body.data()), request.body.size(), jsonField(request.body, "name"), error);
        if (id.empty())
        {
            return json(400, std::string("{\"error\":\"") + error + "\"}");
        }
        return json(201, std::string("{\"id\":\"") + id + "\"}");
    }
    if (path.rfind("/api/v1/clips/", 0) == 0 && path.find("/export") != std::string::npos && method == "POST")
    {
        auto id = path.substr(std::string("/api/v1/clips/").size());
        id = id.substr(0, id.find('/'));
        std::string error;
        auto const dir = engine.exportClip(id, error);
        if (dir.empty())
        {
            return json(400, std::string("{\"error\":\"") + error + "\"}");
        }
        return json(200, std::string("{\"path\":\"") + dir + "\"}");
    }
    if (path.rfind("/api/v1/clips/", 0) == 0 && path.find("/consolidate") != std::string::npos && method == "POST")
    {
        auto id = path.substr(std::string("/api/v1/clips/").size());
        id = id.substr(0, id.find('/'));
        std::string error;
        if (!engine.consolidate(id, error))
        {
            return json(400, std::string("{\"error\":\"") + error + "\"}");
        }
        return json(200, "{\"consolidated\":true}");
    }
    if (path == "/api/v1/control" && method == "POST")
    {
        int const channel = std::stoi(jsonField(request.body, "channel").empty() ? "1" : jsonField(request.body, "channel"));
        auto const action = jsonField(request.body, "action");
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
            engine.scrubFrames(channel, std::stoi(jsonField(request.body, "value").empty() ? "0" : jsonField(request.body, "value")));
        }
        else if (action == "speed")
        {
            engine.setSpeed(channel, std::stod(jsonField(request.body, "value").empty() ? "1" : jsonField(request.body, "value")));
        }
        return json(200, engine.statusJson());
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
    (void)queryValue;
    return json(404, "{\"error\":\"not found\"}");
}
} // namespace replay
