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
};

// Decode JPEG bitstreams on the device, run blend or DIS interpolation there,
// pack v210 there, and download only the packed grain (UI previews are made from it on request).
// Returns false when there is no device or nvJPEG rejects the bitstream; the
// caller then uses the CPU path.
// keyA and keyB name the source frames (camera and time): a frame still decoded on the
// device from the previous call is not decoded again.
bool gpuRenderFromJpeg(std::uint8_t const* jpegA, std::size_t sizeA, std::string const& keyA, std::uint8_t const* jpegB, std::size_t sizeB, std::string const& keyB,
    float phase, bool interpolate, OperatingPoint const& op, std::string const& flowKey, int width, int height, GpuPicture& out);

// One upload of a packed v210 grain, nvJPEG 4:2:2 encode, one download of the bitstream.
// Every GPU function here works on the calling thread's own stream and nvJPEG state,
// so recorder inputs and playout channels run in parallel. An MXL grain passed here is
// page-locked on first use (direct DMA).
[[nodiscard]] std::vector<std::uint8_t> gpuEncodeV210(std::uint8_t const* packed, int width, int height, int rowBytes, int quality);

// Unlock the MXL grain memory the calling thread passed to gpuEncodeV210. Call it
// before releasing that MXL reader.
void gpuReleaseHostMemory();

// One upload of 10-bit planes that are already on the host (tests, uploads, phased assembly).
[[nodiscard]] std::vector<std::uint8_t> gpuEncodeFrame10(Frame10 const& frame, int quality);

// The preview mosaic's NV12 canvas in device memory (SPECIFICATION.md §8.6): `height` luma rows, then
// height/2 rows of interleaved CbCr, `pitch` bytes apart; black. Null without a device.
[[nodiscard]] std::uint8_t* gpuCanvasCreate(int width, int height, int& pitch);
void gpuCanvasDestroy(std::uint8_t* canvas);
// Scales the picture the calling thread last moved through the GPU (gpuEncodeV210, gpuEncodeFrame10,
// gpuRenderFromJpeg) into the canvas rectangle x, y, w, h (even values), area-averaged, on the
// thread's stream. False when the thread has no such picture.
bool gpuCanvasDrawLast(std::uint8_t* canvas, int pitch, int height, int x, int y, int w, int h);
// `rows` rows of `bytes` from `src` to `dst`, each in host or device memory. Returns when copied.
bool gpuCopyRows(void* dst, std::size_t dstPitch, void const* src, std::size_t srcPitch, std::size_t bytes, std::size_t rows);
// The CUDA context this process works in (the runtime's primary context), made current on the
// calling thread: NVENC encodes the canvas there. Null without a device.
[[nodiscard]] void* gpuContext();
} // namespace replay
