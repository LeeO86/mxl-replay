#pragma once

#include <string>
#include <vector>

namespace replay
{
struct VideoFormat
{
    int width = 1920;
    int height = 1080;
    int rateNum = 50;
    int rateDen = 1;
    // Interlaced, top field first (1.8.0). The rate stays the frame rate (25/1 or 30000/1001): the
    // engine records, plays and jogs whole frames; only the MXL reader and writer handle fields.
    bool interlaced = false;

    [[nodiscard]] std::string token() const;
    [[nodiscard]] bool operator==(VideoFormat const& other) const
    {
        return width == other.width && height == other.height && rateNum == other.rateNum && rateDen == other.rateDen && interlaced == other.interlaced;
    }
};

// `1080p50`, `1080p5994`, `2160p50`, `1280x720p50`, `1920x1080p30000/1001`; interlaced `1080i25`,
// `1080i2997` (also `1080i29.97`, `1080i25tff`). The number after `i` is the frame rate, as in
// mxl-test-player's PLAYER_FORMAT; only 25 and 29.97 are interlaced in MXL. `bff` is refused.
VideoFormat parseVideoFormat(std::string const& token);

// The `interlace_mode` values a video input (NMOS receiver caps, BCP-004-01) accepts in this house format:
// `progressive`, and in an interlaced house also `interlaced_tff`.
[[nodiscard]] std::vector<std::string> acceptedInterlaceModes(VideoFormat const& house);
} // namespace replay
