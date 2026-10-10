#pragma once

#include "media/frame.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace replay
{
// One picture of the preview mosaic (SPECIFICATION.md §8.6): a channel output or a camera input.
struct MosaicTile
{
    std::string id;   // "ch1", "cam2"
    std::string kind; // "channel" or "camera"
    int index = 1;
    std::string label;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct MosaicLayout
{
    int width = 0;
    int height = 0;
    // The channels first, then the cameras: tile `channel - 1`, tile `channels + camera - 1`.
    std::vector<MosaicTile> tiles;
};

// Channels at 640×360 (three a row), then cameras at 480×270 (four a row); the canvas is as wide as
// its widest row. A canvas taller than 1080 lines is scaled down to fit 1920×1080. Every edge is even
// (NV12 has one chroma sample per 2×2 pixels).
MosaicLayout mosaicLayout(std::vector<std::string> const& channels, std::vector<std::string> const& cameras);

// Point-samples a picture to a w×h NV12 tile: w×h luma bytes, then h/2 rows of w interleaved CbCr bytes.
void v210ToNv12(std::uint8_t const* v210, int width, int height, int w, int h, std::uint8_t* nv12);
void frameToNv12(Frame10 const& frame, int w, int h, std::uint8_t* nv12);

// The mosaic's NV12 canvas: in device memory when a CUDA device is visible, else in host memory.
// Each tile has one writer (its channel's playout thread, its camera's reader thread); the encoder
// copies the whole canvas. Nobody waits for anybody: a tile may show parts of two pictures for one
// encoded frame.
class PreviewMosaic
{
public:
    PreviewMosaic(MosaicLayout layout, bool device);
    ~PreviewMosaic();
    PreviewMosaic(PreviewMosaic const&) = delete;
    PreviewMosaic& operator=(PreviewMosaic const&) = delete;

    [[nodiscard]] MosaicLayout const& layout() const { return layout_; }
    [[nodiscard]] bool onDevice() const { return device_ != nullptr; }
    // A picture in host memory into tile `tile`, point-sampled on the CPU.
    void drawV210(int tile, std::uint8_t const* v210, int width, int height);
    void drawFrame(int tile, Frame10 const& frame);
    // The picture the calling thread last moved through the GPU (gpuEncodeV210, gpuRenderFromJpeg),
    // scaled on the GPU. False without a device canvas or such a picture.
    bool drawGpuPicture(int tile);
    // The whole canvas into NV12 planes: host memory, or device memory when `deviceTarget` (device canvas only).
    bool copyTo(std::uint8_t* luma, int lumaPitch, std::uint8_t* chroma, int chromaPitch, bool deviceTarget) const;

private:
    // A tile-sized NV12 picture from the host.
    void put(MosaicTile const& tile, std::uint8_t const* nv12);

    MosaicLayout layout_;
    std::uint8_t* device_ = nullptr;
    int pitch_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> host_;
};
} // namespace replay
