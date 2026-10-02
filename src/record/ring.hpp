#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace replay
{
struct StoredFrame
{
    std::uint64_t taiNs = 0;
    std::vector<std::uint8_t> jpeg;
    std::vector<float> audio;
    bool protect = false;
};

struct ProtectRange
{
    std::uint64_t beginNs = 0;
    std::uint64_t endNs = 0;
};

// In-memory ring. Protected timestamps are never overwritten. When the ring is
// full of protected frames, push() returns false and the new frame is dropped.
class FrameRing
{
public:
    explicit FrameRing(std::size_t capacityFrames);

    bool push(StoredFrame frame);
    void protect(std::uint64_t beginNs, std::uint64_t endNs);
    void unprotect(std::uint64_t beginNs, std::uint64_t endNs);
    [[nodiscard]] bool isProtected(std::uint64_t taiNs) const;
    [[nodiscard]] std::optional<StoredFrame> findNearest(std::uint64_t taiNs) const;
    [[nodiscard]] std::optional<StoredFrame> findAtOrBefore(std::uint64_t taiNs) const;
    [[nodiscard]] std::optional<StoredFrame> findAfter(std::uint64_t taiNs) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t protectedCount() const;
    [[nodiscard]] std::uint64_t payloadBytes() const;
    [[nodiscard]] std::uint64_t protectedBytes() const;
    [[nodiscard]] double protectedPercent() const;

private:
    std::size_t capacity_;
    std::map<std::uint64_t, StoredFrame> frames_;
    std::deque<std::uint64_t> order_;
    std::vector<ProtectRange> ranges_;
};

// Sequential segment writer. O_DIRECT is attempted when requested and the
// filesystem accepts it; otherwise the writer falls back to buffered IO.
class SegmentWriter
{
public:
    SegmentWriter(std::string directory, int camera, int segmentSeconds, bool direct);
    ~SegmentWriter();
    SegmentWriter(SegmentWriter const&) = delete;
    SegmentWriter& operator=(SegmentWriter const&) = delete;

    bool write(std::uint64_t taiNs, std::uint8_t const* jpeg, std::size_t jpegSize, float const* audio, std::size_t audioSamples);
    [[nodiscard]] double lastWriteBytesPerSecond() const { return lastBps_; }
    [[nodiscard]] bool directIo() const { return directActive_; }
    [[nodiscard]] std::string directory() const { return directory_; }

private:
    void rotate();
    std::string directory_;
    int camera_ = 1;
    int segmentSeconds_ = 10;
    bool wantDirect_ = false;
    bool directActive_ = false;
    int fd_ = -1;
    int segment_ = 0;
    std::uint64_t segmentStart_ = 0;
    double lastBps_ = 0;
};

[[nodiscard]] double measureWriteThroughput(std::string const& directory, std::size_t bytes);
} // namespace replay
