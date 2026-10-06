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

DomainResult ensureOutputDomain(std::string const& directory, std::string const& id, std::uint64_t historyNs)
{
    auto const def = std::filesystem::path(directory) / "domain_def.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(def, ec))
    {
        auto const body = fileText(def);
        if (jsonBool(body, "x-mxl-fabrics-agent.mirror"))
        {
            return {DomainStatus::Mirror, "output domain is a fabrics mirror"};
        }
        auto const existing = jsonString(body, "id");
        if (existing != id)
        {
            return {DomainStatus::Mismatch, "domain_def.json id " + existing + " does not match " + id};
        }
        return {DomainStatus::Ready, {}};
    }
    std::filesystem::create_directories(directory, ec);
    if (ec)
    {
        return {DomainStatus::Failed, "cannot create output domain: " + ec.message()};
    }
    {
        std::ofstream out(def);
        if (!out)
        {
            return {DomainStatus::Failed, "cannot write domain_def.json"};
        }
        // BCP-007-03 requires id, label, description and tags.
        out << "{\"id\":\"" << id << "\",\"label\":\"MXL Replay\",\"description\":\"Output domain of mxl-replay\",\"tags\":{}}\n";
    }
    auto const optionsPath = std::filesystem::path(directory) / "options.json";
    if (!std::filesystem::exists(optionsPath, ec))
    {
        std::ofstream options(optionsPath);
        if (!options)
        {
            return {DomainStatus::Failed, "cannot write options.json"};
        }
        options << "{\"urn:x-mxl:option:history_duration/v1.0\":" << historyNs << "}\n";
    }
    return {DomainStatus::Ready, {}};
}

bool removeOwnDomain(std::string const& directory, std::string const& id, std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
    {
        return true;
    }
    auto const def = std::filesystem::path(directory) / "domain_def.json";
    if (!std::filesystem::is_regular_file(def, ec))
    {
        error = "refusing to remove " + directory + " without domain_def.json";
        return false;
    }
    auto const existing = jsonString(fileText(def), "id");
    if (existing != id)
    {
        error = "refusing to remove domain " + existing;
        return false;
    }
    std::filesystem::remove_all(directory, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    return true;
}
} // namespace replay
