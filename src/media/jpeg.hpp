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
// A small picture for the UI: the JPEG decoded at 1/2, 1/4 or 1/8 of its size with libjpeg's DCT
// scaling (the smallest of them that is still `minWidth` wide or more), into a 4:2:2 frame.
// A 1080p frame at 1/4 is 480 × 270 and costs a fraction of a full decode.
bool decodeJpegPreview(std::uint8_t const* data, std::size_t size, int minWidth, Frame10& frame);

// The same JPEG as encodeJpeg422(unpackV210(v210)), straight from packed v210 (`rowBytes`
// per line, 0 = v210RowBytes): no 16-bit frame, and the codec and its buffers are kept per
// thread. Throws like encodeJpeg422.
std::vector<std::uint8_t> encodeJpegV210(std::uint8_t const* v210, int width, int height, int rowBytes, int quality);
// The same bytes as packV210(decodeJpeg422(data)), straight into `v210` (`rowBytes` per line,
// 0 = v210RowBytes). False when the JPEG is not width × height 4:2:2.
bool decodeJpegToV210(std::uint8_t const* data, std::size_t size, int width, int height, int rowBytes, std::uint8_t* v210);

// Bytes per hour used to refuse a buffer that does not fit.
// 1920x1080 at quality 92 is 0.1875 bytes/pixel ≈ 70 GB/h at 50 fps.
[[nodiscard]] double bytesPerFrameEstimate(int width, int height, int quality);
[[nodiscard]] double bytesPerHourEstimate(int width, int height, int fpsNum, int fpsDen, int quality);
} // namespace replay
