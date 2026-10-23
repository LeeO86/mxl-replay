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

// Interlaced video in MXL (the shared MXL interlaced convention): one field per grain, grain 2k the
// first field of frame k. Field line i of grain `index` is frame line 2i + p, p = (index & 1) ^ bff.
[[nodiscard]] inline int fieldParity(std::uint64_t grainIndex, bool bff)
{
    return static_cast<int>((grainIndex & 1u) ^ (bff ? 1u : 0u));
}
// Weave: copies the frameHeight / 2 lines of `field` to frame lines 2i + parity.
void weaveField(std::uint8_t const* field, std::size_t rowBytes, int frameHeight, int parity, std::uint8_t* frame);
// Unweave: copies frame lines 2i + parity to the frameHeight / 2 lines of `field`.
void extractField(std::uint8_t const* frame, std::size_t rowBytes, int frameHeight, int parity, std::uint8_t* field);
} // namespace replay
