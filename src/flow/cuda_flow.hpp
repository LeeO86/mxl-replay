#pragma once

#include "flow/dis.hpp"
#include "media/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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

// The priority of a thread's GPU stream: playout above the recording (every other thread) above flows
// computed ahead, which share the lowest with the preview copies.
enum class GpuPriority
{
    Flow,
    Record,
    Playout
};

// Creates the calling thread's GPU state (its stream at `priority`, nvJPEG) unless it has one: playout
// threads and flow workers call it first. Other threads get theirs, at Record, on their first GPU call.
bool gpuPrepareThread(GpuPriority priority);

// The optical flow of a source pair in device memory (opaque).
struct GpuFlow;

// Decodes A and B (unless the thread holds them from its last call) and computes their flow on the calling
// thread's stream; returns when it is done, null when there is no device or nvJPEG rejects a bitstream.
// `gpuSeconds` is the stream's time for the flow. The flow is freed on the stream of the thread that drops
// the last reference, after that thread's work with it is done.
[[nodiscard]] std::shared_ptr<GpuFlow const> gpuComputeFlow(std::uint8_t const* jpegA, std::size_t sizeA, std::string const& keyA, std::uint8_t const* jpegB,
    std::size_t sizeB, std::string const& keyB, OperatingPoint const& op, int width, int height, double& gpuSeconds);

// Device pictures a channel's next grains show, decoded ahead on a worker (DecodeAhead, SPECIFICATION.md §5.1;
// opaque).
struct GpuPictures;
[[nodiscard]] std::shared_ptr<GpuPictures> gpuPicturesCreate(int count);
// Decodes a JPEG into picture `index` on the calling thread's stream and returns when it is done (the worker).
// False when there is no device, nvJPEG rejects the bitstream, or it is not width × height.
bool gpuDecodePicture(GpuPictures& pictures, int index, std::uint8_t const* jpeg, std::size_t size, int width, int height);

// What gpuRenderFromJpeg asks for a frame (0: A, 1: B) it does not hold from its last call: the index of its
// picture decoded ahead, held until give(); -1 decodes it on the calling thread.
class GpuAhead
{
public:
    GpuPictures* pictures = nullptr;
    virtual int take(int frame) = 0;
    virtual void give(int index) = 0;

protected:
    ~GpuAhead() = default;
};

// Decode JPEG bitstreams on the device, blend there (interpolate with `flow` when it is given),
// pack v210 there, and download only the packed grain (UI previews are made from it on request).
// Returns false when there is no device or nvJPEG rejects the bitstream; the
// caller then uses the CPU path.
// keyA and keyB name the source frames (camera and time): a frame still decoded on the
// device from the previous call is not decoded again, and one `ahead` has is copied from there.
bool gpuRenderFromJpeg(std::uint8_t const* jpegA, std::size_t sizeA, std::string const& keyA, std::uint8_t const* jpegB, std::size_t sizeB, std::string const& keyB,
    float phase, GpuFlow const* flow, int width, int height, GpuPicture& out, GpuAhead* ahead = nullptr);

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
