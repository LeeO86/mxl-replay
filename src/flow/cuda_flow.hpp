#pragma once

#include "flow/dis.hpp"
#include "media/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay
{
// True when a CUDA device is visible to this process. A missing driver returns 0
// and the process keeps running (repeat / blend / libjpeg).
[[nodiscard]] int cudaFlowDeviceCount();
[[nodiscard]] bool cudaFlowAvailable();
[[nodiscard]] bool ofaProbe();
[[nodiscard]] bool ofaAvailable();
[[nodiscard]] char const* activeFlowModuleName();

// "nvjpeg" when this binary was linked with nvJPEG and a device is visible.
// Otherwise "libjpeg-turbo" (clip playback without a GPU).
[[nodiscard]] char const* jpegRuntimeBackend();

struct GpuPicture
{
    std::vector<std::uint8_t> v210;
    std::vector<std::uint8_t> preview;
};

// Decode JPEG bitstreams on the device, run blend or DIS interpolation there,
// pack v210 there, and download only the packed grain plus a small preview.
// Returns false when there is no device or nvJPEG rejects the bitstream; the
// caller then uses the CPU path.
bool gpuRenderFromJpeg(std::uint8_t const* jpegA, std::size_t sizeA, std::uint8_t const* jpegB, std::size_t sizeB, float phase, bool interpolate,
    OperatingPoint const& op, std::string const& flowKey, int width, int height, GpuPicture& out);

// One upload of a packed v210 grain, nvJPEG 4:2:2 encode, one download of the bitstream.
[[nodiscard]] std::vector<std::uint8_t> gpuEncodeV210(std::uint8_t const* packed, int width, int height, int rowBytes, int quality);

// One upload of 10-bit planes that are already on the host (tests, uploads, phased assembly).
[[nodiscard]] std::vector<std::uint8_t> gpuEncodeFrame10(Frame10 const& frame, int quality);
} // namespace replay
