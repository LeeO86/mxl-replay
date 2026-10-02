#pragma once

#include <string>

namespace replay
{
struct VideoFormat
{
    int width = 1920;
    int height = 1080;
    int rateNum = 50;
    int rateDen = 1;

    [[nodiscard]] std::string token() const;
    [[nodiscard]] bool operator==(VideoFormat const& other) const
    {
        return width == other.width && height == other.height && rateNum == other.rateNum && rateDen == other.rateDen;
    }
};

// `1080p50`, `1080p5994`, `2160p50`, `1280x720p50`, `1920x1080p30000/1001`.
VideoFormat parseVideoFormat(std::string const& token);
} // namespace replay
