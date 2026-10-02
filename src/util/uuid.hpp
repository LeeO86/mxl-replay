#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace replay
{
inline constexpr char kUuidNamespaceUrl[] = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";

std::optional<std::string> parseUuid(std::string_view text);
bool isUuid(std::string_view text);
std::string uuidV5(std::string_view namespaceUuid, std::string_view name);
std::string shortId(std::string const& uuid);
} // namespace replay
