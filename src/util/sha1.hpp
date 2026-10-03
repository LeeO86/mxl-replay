#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace replay
{
std::array<std::uint8_t, 20> sha1(std::uint8_t const* data, std::size_t size);
std::array<std::uint8_t, 20> sha1(std::string_view data);
std::string base64Encode(std::uint8_t const* data, std::size_t size);
} // namespace replay
