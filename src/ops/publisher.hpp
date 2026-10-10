#pragma once

#include "media/mosaic.hpp"
#include "ops/preview.hpp"

#include <functional>
#include <memory>
#include <string>

namespace replay
{
// The WebRTC preview's encoder (SPECIFICATION.md §8.6). Its own thread copies the mosaic at the mosaic
// rate, encodes it once as H.264 (NVENC on the device canvas; libx264 only when NVENC cannot be opened,
// which is logged) and publishes it over RTSP/TCP to MediaMTX, which serves WHEP and HLS. Drawing
// threads never wait for it. Adapted from mxl-multiviewer 1.4.0 (src/media/publisher.cpp) and the
// FlowXer engine prototype (engine/src/engine/encoder.cpp), both by the same author.
class PreviewPublisher
{
public:
    // `fpsNum / fpsDen`: pictures per second. `url`: rtsp://host:port/path.
    PreviewPublisher(PreviewMosaic const& mosaic, std::string url, int fpsNum, int fpsDen);
    ~PreviewPublisher();
    PreviewPublisher(PreviewPublisher const&) = delete;
    PreviewPublisher& operator=(PreviewPublisher const&) = delete;

    // `observe` gets the seconds this thread spent on each published picture (copy, encode, send).
    void start(std::function<void(double)> observe);
    void stop();
    [[nodiscard]] PreviewStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace replay
