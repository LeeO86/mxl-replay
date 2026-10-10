#include "media/mosaic.hpp"

#include "flow/cuda_flow.hpp"
#include "media/v210.hpp"

#include <algorithm>
#include <cstring>

namespace replay
{
MosaicLayout mosaicLayout(std::vector<std::string> const& channels, std::vector<std::string> const& cameras)
{
    int const channelRows = static_cast<int>((channels.size() + 2) / 3);
    int const cameraRows = static_cast<int>((cameras.size() + 3) / 4);
    int const nominal = channelRows * 360 + cameraRows * 270;
    double const scale = nominal > 1080 ? 1080.0 / nominal : 1.0;
    auto const even = [scale](int size) { return std::max(2, static_cast<int>(size * scale) & ~1); };
    int const channelW = even(640);
    int const channelH = even(360);
    int const cameraW = even(480);
    int const cameraH = even(270);
    MosaicLayout layout;
    auto const add = [&](char const* kind, char const* prefix, std::size_t i, std::string const& label, int x, int y, int w, int h) {
        MosaicTile tile;
        tile.kind = kind;
        tile.index = static_cast<int>(i) + 1;
        tile.id = prefix + std::to_string(tile.index);
        tile.label = label;
        tile.x = x;
        tile.y = y;
        tile.w = w;
        tile.h = h;
        layout.width = std::max(layout.width, x + w);
        layout.tiles.push_back(std::move(tile));
    };
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        add("channel", "ch", i, channels[i], static_cast<int>(i % 3) * channelW, static_cast<int>(i / 3) * channelH, channelW, channelH);
    }
    int const top = channelRows * channelH;
    for (std::size_t i = 0; i < cameras.size(); ++i)
    {
        add("camera", "cam", i, cameras[i], static_cast<int>(i % 4) * cameraW, top + static_cast<int>(i / 4) * cameraH, cameraW, cameraH);
    }
    layout.height = top + cameraRows * cameraH;
    return layout;
}

void v210ToNv12(std::uint8_t const* v210, int width, int height, int w, int h, std::uint8_t* nv12)
{
    // Where pixel 0..5 of a v210 group keeps its Y, and pair 0..2 its Cb and Cr: {word, shift}.
    static constexpr int kY[6][2] = {{0, 10}, {1, 0}, {1, 20}, {2, 10}, {3, 0}, {3, 20}};
    static constexpr int kCb[3][2] = {{0, 0}, {1, 10}, {2, 20}};
    static constexpr int kCr[3][2] = {{0, 20}, {2, 0}, {3, 10}};
    auto const rowBytes = v210RowBytes(width);
    std::uint8_t* chroma = nv12 + static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (int oy = 0; oy < h; ++oy)
    {
        auto const* line = v210 + static_cast<std::size_t>(oy) * static_cast<std::size_t>(height) / static_cast<std::size_t>(h) * rowBytes;
        for (int ox = 0; ox < w; ++ox)
        {
            int const x = static_cast<int>(static_cast<long long>(ox) * width / w);
            std::uint32_t words[4];
            std::memcpy(words, line + static_cast<std::size_t>(x / 6) * 16u, sizeof(words));
            auto const sample = [&](int const (&at)[2]) { return static_cast<std::uint8_t>(((words[at[0]] >> at[1]) & 0x3ffu) >> 2); };
            nv12[static_cast<std::size_t>(oy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(ox)] = sample(kY[x % 6]);
            if ((ox & 1) == 0 && (oy & 1) == 0)
            {
                auto* pair = chroma + static_cast<std::size_t>(oy / 2) * static_cast<std::size_t>(w) + static_cast<std::size_t>(ox);
                pair[0] = sample(kCb[(x % 6) / 2]);
                pair[1] = sample(kCr[(x % 6) / 2]);
            }
        }
    }
}

void frameToNv12(Frame10 const& frame, int w, int h, std::uint8_t* nv12)
{
    std::uint8_t* chroma = nv12 + static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    int const chromaWidth = frame.width / 2;
    for (int oy = 0; oy < h; ++oy)
    {
        auto const y = static_cast<std::size_t>(static_cast<long long>(oy) * frame.height / h);
        for (int ox = 0; ox < w; ++ox)
        {
            auto const x = static_cast<std::size_t>(static_cast<long long>(ox) * frame.width / w);
            nv12[static_cast<std::size_t>(oy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(ox)] =
                static_cast<std::uint8_t>(frame.y[y * static_cast<std::size_t>(frame.width) + x] >> 2);
            if ((ox & 1) == 0 && (oy & 1) == 0)
            {
                auto const c = y * static_cast<std::size_t>(chromaWidth) + x / 2;
                auto* pair = chroma + static_cast<std::size_t>(oy / 2) * static_cast<std::size_t>(w) + static_cast<std::size_t>(ox);
                pair[0] = static_cast<std::uint8_t>(frame.cb[c] >> 2);
                pair[1] = static_cast<std::uint8_t>(frame.cr[c] >> 2);
            }
        }
    }
}

PreviewMosaic::PreviewMosaic(MosaicLayout layout, bool device)
    : layout_(std::move(layout))
{
    if (device)
    {
        device_ = gpuCanvasCreate(layout_.width, layout_.height, pitch_);
    }
    if (device_ == nullptr)
    {
        // Black: Y 16, Cb and Cr 128.
        pitch_ = layout_.width;
        auto const luma = static_cast<std::size_t>(layout_.width) * static_cast<std::size_t>(layout_.height);
        host_.assign(luma + luma / 2, 128);
        std::fill(host_.begin(), host_.begin() + static_cast<std::ptrdiff_t>(luma), 16);
    }
}

PreviewMosaic::~PreviewMosaic()
{
    gpuCanvasDestroy(device_);
}

void PreviewMosaic::put(MosaicTile const& tile, std::uint8_t const* nv12)
{
    auto const* chroma = nv12 + static_cast<std::size_t>(tile.w) * static_cast<std::size_t>(tile.h);
    auto const lumaAt = static_cast<std::size_t>(tile.y) * static_cast<std::size_t>(pitch_) + static_cast<std::size_t>(tile.x);
    auto const chromaAt = static_cast<std::size_t>(layout_.height + tile.y / 2) * static_cast<std::size_t>(pitch_) + static_cast<std::size_t>(tile.x);
    auto const w = static_cast<std::size_t>(tile.w);
    if (device_ != nullptr)
    {
        gpuCopyRows(device_ + lumaAt, static_cast<std::size_t>(pitch_), nv12, w, w, static_cast<std::size_t>(tile.h));
        gpuCopyRows(device_ + chromaAt, static_cast<std::size_t>(pitch_), chroma, w, w, static_cast<std::size_t>(tile.h / 2));
        return;
    }
    std::lock_guard lock{mutex_};
    for (int row = 0; row < tile.h; ++row)
    {
        std::memcpy(host_.data() + lumaAt + static_cast<std::size_t>(row) * static_cast<std::size_t>(pitch_), nv12 + static_cast<std::size_t>(row) * w, w);
    }
    for (int row = 0; row < tile.h / 2; ++row)
    {
        std::memcpy(host_.data() + chromaAt + static_cast<std::size_t>(row) * static_cast<std::size_t>(pitch_), chroma + static_cast<std::size_t>(row) * w, w);
    }
}

void PreviewMosaic::drawV210(int tile, std::uint8_t const* v210, int width, int height)
{
    if (tile < 0 || tile >= static_cast<int>(layout_.tiles.size()) || v210 == nullptr || width < 2 || height < 1)
    {
        return;
    }
    auto const& t = layout_.tiles[static_cast<std::size_t>(tile)];
    thread_local std::vector<std::uint8_t> nv12;
    nv12.resize(static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * 3 / 2);
    v210ToNv12(v210, width, height, t.w, t.h, nv12.data());
    put(t, nv12.data());
}

void PreviewMosaic::drawFrame(int tile, Frame10 const& frame)
{
    if (tile < 0 || tile >= static_cast<int>(layout_.tiles.size()) || frame.empty())
    {
        return;
    }
    auto const& t = layout_.tiles[static_cast<std::size_t>(tile)];
    thread_local std::vector<std::uint8_t> nv12;
    nv12.resize(static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * 3 / 2);
    frameToNv12(frame, t.w, t.h, nv12.data());
    put(t, nv12.data());
}

bool PreviewMosaic::drawGpuPicture(int tile)
{
    if (device_ == nullptr || tile < 0 || tile >= static_cast<int>(layout_.tiles.size()))
    {
        return false;
    }
    auto const& t = layout_.tiles[static_cast<std::size_t>(tile)];
    return gpuCanvasDrawLast(device_, pitch_, layout_.height, t.x, t.y, t.w, t.h);
}

bool PreviewMosaic::copyTo(std::uint8_t* luma, int lumaPitch, std::uint8_t* chroma, int chromaPitch, bool deviceTarget) const
{
    auto const w = static_cast<std::size_t>(layout_.width);
    auto const h = static_cast<std::size_t>(layout_.height);
    if (device_ != nullptr)
    {
        return gpuCopyRows(luma, static_cast<std::size_t>(lumaPitch), device_, static_cast<std::size_t>(pitch_), w, h) &&
               gpuCopyRows(chroma, static_cast<std::size_t>(chromaPitch), device_ + h * static_cast<std::size_t>(pitch_), static_cast<std::size_t>(pitch_), w, h / 2);
    }
    if (deviceTarget)
    {
        return false;
    }
    std::lock_guard lock{mutex_};
    for (std::size_t row = 0; row < h; ++row)
    {
        std::memcpy(luma + row * static_cast<std::size_t>(lumaPitch), host_.data() + row * w, w);
    }
    for (std::size_t row = 0; row < h / 2; ++row)
    {
        std::memcpy(chroma + row * static_cast<std::size_t>(chromaPitch), host_.data() + (h + row) * w, w);
    }
    return true;
}
} // namespace replay
