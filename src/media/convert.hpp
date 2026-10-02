#pragma once

#include "media/format.hpp"
#include "media/frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace replay
{
struct ConvertedMedia
{
    std::vector<Frame10> frames;
    std::vector<float> audio;
    int channels = 2;
    std::string error;
};

// JPEG, or any container FFmpeg can open, scaled to the house raster.
[[nodiscard]] ConvertedMedia convertUpload(std::uint8_t const* data, std::size_t size, VideoFormat const& house);

// Intra-frame mezzanine: a JPEG-in-MOV when FFmpeg is linked, plus a WAV.
struct ExportFiles
{
    std::string movPath;
    std::string wavPath;
    std::string error;
};
} // namespace replay
