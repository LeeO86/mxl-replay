#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
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

// One camera's replay buffer on disk. Frames are appended to segment files
// (`seg-<first TAI>.bin`, `segmentSeconds` each) in `directory`; RAM holds only
// an index entry per frame and a few read descriptors. Segments whose newest
// frame is older than the newest recorded frame minus `retentionNs` are deleted,
// unless a protected range (a clip) touches them. Existing segments are indexed
// again on construction, so the buffer and the clips survive a restart.
// Thread-safe: an internal lock guards the index, and no file is read or written
// while it is held, so a slow disk does not stall readers of other frames.
class FrameRing
{
public:
    FrameRing(std::string directory, std::uint64_t retentionNs, int segmentSeconds);
    ~FrameRing();
    FrameRing(FrameRing&&) noexcept;
    FrameRing& operator=(FrameRing&&) noexcept;
    FrameRing(FrameRing const&) = delete;
    FrameRing& operator=(FrameRing const&) = delete;

    // Appends the frame. False when it is older than the newest frame, replaces a
    // protected frame, or cannot be written.
    bool push(StoredFrame frame);
    void protect(std::uint64_t beginNs, std::uint64_t endNs);
    void unprotect(std::uint64_t beginNs, std::uint64_t endNs);
    // Deletes expired, unprotected segments. push() calls it on every new segment;
    // call it once after the clips of a restart are protected again.
    void enforceRetention();
    [[nodiscard]] bool isProtected(std::uint64_t taiNs) const;
    [[nodiscard]] std::optional<StoredFrame> findNearest(std::uint64_t taiNs) const;
    [[nodiscard]] std::optional<StoredFrame> findAtOrBefore(std::uint64_t taiNs) const;
    [[nodiscard]] std::optional<StoredFrame> findAfter(std::uint64_t taiNs) const;
    // The audio of the nearest frame, without reading its JPEG.
    [[nodiscard]] std::vector<float> findNearestAudio(std::uint64_t taiNs) const;
    // Waits up to `timeout` until a frame at or after `taiNs` is stored; true when one is.
    bool waitFor(std::uint64_t taiNs, std::chrono::nanoseconds timeout) const;
    [[nodiscard]] std::size_t size() const;
    // TAI of the newest frame; 0 when the ring is empty.
    [[nodiscard]] std::uint64_t newestNs() const;
    [[nodiscard]] std::size_t protectedCount() const;
    [[nodiscard]] std::uint64_t payloadBytes() const;
    // Bytes of the segments that protected ranges keep: a clip keeps whole segments.
    [[nodiscard]] std::uint64_t protectedBytes() const;
    [[nodiscard]] double protectedPercent() const;
    [[nodiscard]] std::uint64_t diskBytes() const;
    [[nodiscard]] std::size_t segmentCount() const;
    // Frames that could not be written (full disk or I/O error) since construction.
    [[nodiscard]] std::uint64_t writeFailures() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Bytes of the segment files a FrameRing would reopen in `directory`.
[[nodiscard]] std::uint64_t segmentBytes(std::string const& directory);
[[nodiscard]] double measureWriteThroughput(std::string const& directory, std::size_t bytes);
} // namespace replay
