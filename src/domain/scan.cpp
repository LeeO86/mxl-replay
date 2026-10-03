#include "domain/scan.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace replay
{
namespace
{
std::string fileText(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string jsonString(std::string const& body, std::string const& key)
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
    auto const quote = body.find('"', colon + 1);
    if (quote == std::string::npos)
    {
        return {};
    }
    auto const end = body.find('"', quote + 1);
    if (end == std::string::npos)
    {
        return {};
    }
    return body.substr(quote + 1, end - quote - 1);
}

bool jsonBool(std::string const& body, std::string const& key)
{
    auto const needle = "\"" + key + "\"";
    auto const pos = body.find(needle);
    if (pos == std::string::npos)
    {
        return false;
    }
    auto const colon = body.find(':', pos);
    if (colon == std::string::npos)
    {
        return false;
    }
    auto const rest = body.substr(colon + 1);
    return rest.find("true") != std::string::npos && (rest.find("false") == std::string::npos || rest.find("true") < rest.find("false"));
}
} // namespace

std::vector<DomainInfo> scanDomains(std::string const& root)
{
    std::vector<DomainInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        return out;
    }
    for (auto const& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec || !entry.is_directory())
        {
            continue;
        }
        auto const def = entry.path() / "domain_def.json";
        if (!std::filesystem::is_regular_file(def, ec))
        {
            continue;
        }
        auto const body = fileText(def);
        DomainInfo info;
        info.path = entry.path().string();
        info.id = jsonString(body, "id");
        info.label = jsonString(body, "label");
        info.mirror = jsonBool(body, "x-mxl-fabrics-agent.mirror");
        if (!info.id.empty())
        {
            out.push_back(info);
        }
    }
    return out;
}

std::optional<DomainInfo> resolveDomain(std::string const& root, std::string const& id)
{
    for (auto const& domain : scanDomains(root))
    {
        if (domain.id == id)
        {
            return domain;
        }
    }
    return std::nullopt;
}

bool isMirrorDomain(std::string const& domainDir)
{
    auto const body = fileText(std::filesystem::path(domainDir) / "domain_def.json");
    return jsonBool(body, "x-mxl-fabrics-agent.mirror");
}

bool ensureOutputDomain(std::string const& directory, std::string const& id, std::uint64_t historyNs)
{
    if (isMirrorDomain(directory))
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
    {
        return false;
    }
    auto const def = std::filesystem::path(directory) / "domain_def.json";
    if (!std::filesystem::exists(def))
    {
        std::ofstream out(def);
        out << "{\"id\":\"" << id << "\",\"label\":\"MXL Replay\"}\n";
        std::ofstream options(std::filesystem::path(directory) / "options.json");
        options << "{\"urn:x-mxl:option:history_duration/v1.0\":" << historyNs << "}\n";
    }
    return true;
}
} // namespace replay
