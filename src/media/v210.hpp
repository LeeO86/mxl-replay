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
// out = a·(1024−w) + b·w >> 10 for every 10-bit sample of two v210 pictures of `bytes` each, straight on
// the packed words: the same samples as unpackV210, blendInto and packV210 (padding stays 0), without the
// 16-bit frames. `out` may be `a` or `b`. `weight1024` is 0–1024 (blendWeight1024).
void blendV210(std::uint8_t const* a, std::uint8_t const* b, std::uint8_t* out, std::size_t bytes, std::uint32_t weight1024);

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
