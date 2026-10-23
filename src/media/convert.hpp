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
    // A file with an audio stream and no video stream (1.6.0: an audio item); `audio` is 48 kHz stereo float.
    bool audioOnly = false;
    std::string error;
};

// Frame-rate conversion of an upload to the house rate: house frame n starts at n / rate and shows the
// newest source frame that started at or before it (1 ms of tolerance), so the duration stays.
class FrameRateConverter
{
public:
    FrameRateConverter(int rateNum, int rateDen)
        : num_(rateNum)
        , den_(rateDen)
    {
    }
    // A source frame starts at `nextNs` (from the file's start): `previous` fills the house frames before it.
    template <typename T>
    void emitUntil(std::int64_t nextNs, T const& previous, std::vector<T>& out)
    {
        while (startNs(emitted_) + kToleranceNs <= nextNs)
        {
            out.push_back(previous);
            ++emitted_;
        }
    }
    // The last source frame ends at `endNs`: it fills the house frames before that, and at least one.
    template <typename T>
    void finish(std::int64_t endNs, T const& last, std::vector<T>& out)
    {
        emitUntil(endNs, last, out);
        if (emitted_ == 0)
        {
            out.push_back(last);
            ++emitted_;
        }
    }
    [[nodiscard]] std::uint64_t emitted() const { return emitted_; }

private:
    static constexpr std::int64_t kToleranceNs = 1000000;
    [[nodiscard]] std::int64_t startNs(std::uint64_t n) const
    {
        return static_cast<std::int64_t>(n) * 1000000000LL * den_ / num_;
    }
    std::int64_t num_;
    std::int64_t den_;
    std::uint64_t emitted_ = 0;
};

// JPEG, or any container FFmpeg can open, scaled to the house raster and converted to the house rate.
[[nodiscard]] ConvertedMedia convertUpload(std::uint8_t const* data, std::size_t size, VideoFormat const& house);

// Intra-frame mezzanine: a JPEG-in-MOV when FFmpeg is linked, plus a WAV.
struct ExportFiles
{
    std::string movPath;
    std::string wavPath;
    std::string error;
};
} // namespace replay
