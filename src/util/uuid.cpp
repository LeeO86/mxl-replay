#include "util/uuid.hpp"

#include "util/sha1.hpp"

#include <array>
#include <cstdio>

namespace replay
{
namespace
{
int hexVal(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

std::optional<std::array<std::uint8_t, 16>> parseBytes(std::string_view s)
{
    if (s.size() != 36)
    {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> out{};
    std::size_t byteIdx = 0;
    for (std::size_t i = 0; i < 36;)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (s[i] != '-')
            {
                return std::nullopt;
            }
            ++i;
            continue;
        }
        if (i + 1 >= s.size())
        {
            return std::nullopt;
        }
        int const hi = hexVal(s[i]);
        int const lo = hexVal(s[i + 1]);
        if (hi < 0 || lo < 0 || byteIdx >= out.size())
        {
            return std::nullopt;
        }
        out[byteIdx++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    if (byteIdx != 16)
    {
        return std::nullopt;
    }
    return out;
}

std::string formatUuid(std::array<std::uint8_t, 16> const& bytes)
{
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
        bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}
} // namespace

std::optional<std::string> parseUuid(std::string_view text)
{
    auto const bytes = parseBytes(text);
    if (!bytes)
    {
        return std::nullopt;
    }
    return formatUuid(*bytes);
}

bool isUuid(std::string_view text)
{
    return parseBytes(text).has_value();
}

std::string uuidV5(std::string_view namespaceUuid, std::string_view name)
{
    auto ns = parseBytes(namespaceUuid);
    if (!ns)
    {
        ns = parseBytes(kUuidNamespaceUrl);
    }
    std::string material(reinterpret_cast<char const*>(ns->data()), ns->size());
    material.append(name);
    auto const digest = sha1(material);
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 16; ++i)
    {
        out[static_cast<std::size_t>(i)] = digest[static_cast<std::size_t>(i)];
    }
    out[6] = static_cast<std::uint8_t>((out[6] & 0x0f) | 0x50);
    out[8] = static_cast<std::uint8_t>((out[8] & 0x3f) | 0x80);
    return formatUuid(out);
}

std::string shortId(std::string const& uuid)
{
    std::string out;
    for (char c : uuid)
    {
        if (c != '-' && out.size() < 8)
        {
            out.push_back(c);
        }
    }
    return out;
}
} // namespace replay
