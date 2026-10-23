#pragma once

#include "media/format.hpp"
#include "media/frame.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace replay
{
struct ConvertedMedia
{
    // The distinct source frames, in order. A repeated frame is stored once (1.8.1).
    std::vector<Frame10> frames;
    // The clip at the house rate: house frame n shows frames[order[n]].
    std::vector<std::uint32_t> order;
    std::vector<float> audio;
    int channels = 2;
    // A file with an audio stream and no video stream (1.6.0: an audio item); `audio` is 48 kHz stereo float.
    bool audioOnly = false;
    std::string error;
};

// The start of each decoded source frame of an upload, in ns from the file's start, kept in decode
// order (1.8.1): a frame without a timestamp follows the previous one by that one's duration (else
// `fallbackNs`, the stream's frame period), and a start before the previous one counts as that one's.
struct SourceClock
{
    std::int64_t fallbackNs = 40000000;
    bool any = false;
    std::int64_t lastNs = 0;
    std::int64_t lastDurationNs = 0;

    // `ns`: the frame's timestamp, or a negative value when it has none. `durationNs`: 0 when unknown.
    std::int64_t start(std::int64_t ns, std::int64_t durationNs)
    {
        if (ns < 0)
        {
            ns = any ? lastNs + (lastDurationNs > 0 ? lastDurationNs : fallbackNs) : 0;
        }
        if (any)
        {
            ns = std::max(ns, lastNs);
        }
        lastDurationNs = durationNs > 0 ? durationNs : (!any || ns == lastNs ? fallbackNs : ns - lastNs);
        lastNs = ns;
        any = true;
        return ns;
    }
    // Where the last frame ends.
    [[nodiscard]] std::int64_t endNs() const { return lastNs + lastDurationNs; }
};

// Frame-rate conversion of an upload to the house rate (1.8.1): house frame n (starting at n / rate)
// shows the source frame whose start is nearest to it, the earlier one on a tie. A source frame is
// nearest up to half-way to the next one, so the decision is made when the next frame arrives. Exact:
// 25 → 50 shows each frame twice, 50 → 25 every second frame; equal rates stay 1:1 under jitter of up
// to half a period. The last frame lasts its own duration. The duration of the file stays.
class FrameRateConverter
{
public:
    FrameRateConverter(int rateNum, int rateDen)
        : num_(rateNum)
        , den_(rateDen)
    {
    }
    // Source frame `frame` starts at `startNs` (from the file's start; an earlier start counts as the previous one's).
    void add(std::uint32_t frame, std::int64_t startNs, std::vector<std::uint32_t>& order)
    {
        if (have_)
        {
            startNs = std::max(startNs, previousNs_);
            // House frames up to half-way (inclusive) to this frame show the previous one.
            while (2 * houseNs(next_) <= previousNs_ + startNs)
            {
                order.push_back(previous_);
                ++next_;
            }
        }
        have_ = true;
        previous_ = frame;
        previousNs_ = startNs;
    }
    // The last source frame ends at `endNs`: it shows on the house frames whose middle is before that,
    // and on at least one house frame of the clip.
    void finish(std::int64_t endNs, std::vector<std::uint32_t>& order)
    {
        if (!have_)
        {
            return;
        }
        while (houseNs(next_) + houseNs(next_ + 1) <= 2 * endNs)
        {
            order.push_back(previous_);
            ++next_;
        }
        if (order.empty())
        {
            order.push_back(previous_);
            ++next_;
        }
        have_ = false;
    }

private:
    [[nodiscard]] std::int64_t houseNs(std::uint64_t n) const { return static_cast<std::int64_t>(n) * 1000000000LL * den_ / num_; }
    std::int64_t num_;
    std::int64_t den_;
    std::uint64_t next_ = 0;
    bool have_ = false;
    std::uint32_t previous_ = 0;
    std::int64_t previousNs_ = 0;
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
