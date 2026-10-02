#pragma once

#include "media/frame.hpp"

#include <cstddef>
#include <cstdint>

namespace replay
{
[[nodiscard]] std::size_t v210RowBytes(int width);
[[nodiscard]] std::size_t v210Size(int width, int height);
void unpackV210(std::uint8_t const* src, int srcRowBytes, Frame10& dst);
void packV210(Frame10 const& src, std::uint8_t* dst, int dstRowBytes);
} // namespace replay
