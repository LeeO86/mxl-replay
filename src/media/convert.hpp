#pragma once

#include "media/format.hpp"
#include "media/frame.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
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
        if (next_ == 0)
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

// An upload's file on disk (`path`), or a buffer in memory (`path` empty).
struct UploadSource
{
    std::string path;
    std::uint8_t const* data = nullptr;
    std::size_t size = 0;
};

// Where a streamed upload goes (1.8.2). A callback that returns false stops the conversion.
struct UploadSink
{
    // House frame `n` (0, 1, 2 …) shows `picture`. `source` numbers the source frames: a repeated frame keeps
    // its number (encode it once). `audio`: the frame's 48 kHz stereo float samples (the house cadence; fewer
    // or none past the end of the audio).
    std::function<bool(std::uint64_t n, std::uint32_t source, Frame10 const& picture, std::vector<float> const& audio)> frame;
    // A file without video (an audio item): its 48 kHz stereo float samples, in pieces of up to 1 s.
    std::function<bool(float const* samples, std::size_t count)> audio;
};

struct UploadResult
{
    std::uint64_t frames = 0;       // house frames given to the sink
    std::uint64_t audioSamples = 0; // stereo samples of an audio item
    bool audioOnly = false;
    bool stopped = false; // a callback returned false
    std::string error;    // "scaled" for a still image of another size (not an error)
};

// JPEG, or any container FFmpeg can open, scaled to the house raster and converted to the house rate,
// one house frame at a time (1.8.2): memory does not grow with the length of the file. The audio is read
// by a second demuxer, so it does not matter how the file interleaves it with the video.
[[nodiscard]] UploadResult streamUpload(UploadSource const& source, VideoFormat const& house, UploadSink const& sink);

// streamUpload into memory: each source frame once, and the order of the house frames.
[[nodiscard]] ConvertedMedia convertUpload(std::uint8_t const* data, std::size_t size, VideoFormat const& house);

// Intra-frame mezzanine: a JPEG-in-MOV when FFmpeg is linked, plus a WAV.
struct ExportFiles
{
    std::string movPath;
    std::string wavPath;
    std::string error;
};
} // namespace replay
