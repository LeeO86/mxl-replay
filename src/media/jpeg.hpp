#pragma once

#include "media/frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace replay
{
// Storage is baseline JPEG 4:2:2, 8-bit. nvJPEG is used when a GPU is visible.
// libjpeg-turbo is the CPU fallback (clip playback with no device). nvJPEG's
// baseline encoder does not offer 10-bit or 12-bit 4:2:2.
[[nodiscard]] int jpegStorageBitDepth();
[[nodiscard]] bool jpegSupports10Bit();
[[nodiscard]] bool jpegSupports12Bit();

std::vector<std::uint8_t> encodeJpeg422(Frame10 const& frame, int quality);
bool decodeJpeg422(std::uint8_t const* data, std::size_t size, Frame10& frame);

// Bytes per hour used to refuse a buffer that does not fit.
// 1920x1080 at quality 92 is 0.1875 bytes/pixel ≈ 70 GB/h at 50 fps.
[[nodiscard]] double bytesPerFrameEstimate(int width, int height, int quality);
[[nodiscard]] double bytesPerHourEstimate(int width, int height, int fpsNum, int fpsDen, int quality);
} // namespace replay
