#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace replay
{
struct DomainInfo
{
    std::string path;
    std::string id;
    std::string label;
    bool mirror = false;
};

std::vector<DomainInfo> scanDomains(std::string const& root);
std::optional<DomainInfo> resolveDomain(std::string const& root, std::string const& id);
bool isMirrorDomain(std::string const& domainDir);
bool ensureOutputDomain(std::string const& directory, std::string const& id, std::uint64_t historyNs);
} // namespace replay
