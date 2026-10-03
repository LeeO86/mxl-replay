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

enum class DomainStatus
{
    Ready,
    Mismatch,
    Mirror,
    Failed
};

struct DomainResult
{
    DomainStatus status = DomainStatus::Failed;
    std::string message;
};

std::vector<DomainInfo> scanDomains(std::string const& root);
std::optional<DomainInfo> resolveDomain(std::string const& root, std::string const& id);
bool isMirrorDomain(std::string const& domainDir);
DomainResult ensureOutputDomain(std::string const& directory, std::string const& id, std::uint64_t historyNs);
bool removeOwnDomain(std::string const& directory, std::string const& id, std::string& error);
} // namespace replay
