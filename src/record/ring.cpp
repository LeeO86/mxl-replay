#include "record/ring.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <utility>

namespace replay
{
namespace
{
bool covered(std::vector<ProtectRange> const& ranges, std::uint64_t tai)
{
    for (auto const& range : ranges)
    {
        if (tai >= range.beginNs && tai <= range.endNs)
        {
            return true;
        }
    }
    return false;
}
} // namespace

FrameRing::FrameRing(std::size_t capacityFrames)
    : capacity_(std::max<std::size_t>(1, capacityFrames))
{
}

bool FrameRing::push(StoredFrame frame)
{
    auto const tai = frame.taiNs;
    if (frames_.count(tai) != 0)
    {
        if (frames_[tai].protect)
        {
            return false;
        }
        frames_[tai] = std::move(frame);
        frames_[tai].protect = isProtected(tai);
        return true;
    }
    while (order_.size() >= capacity_)
    {
        bool dropped = false;
        for (auto it = order_.begin(); it != order_.end(); ++it)
        {
            if (!isProtected(*it))
            {
                frames_.erase(*it);
                order_.erase(it);
                dropped = true;
                break;
            }
        }
        if (!dropped)
        {
            return false;
        }
    }
    frame.protect = isProtected(tai);
    order_.push_back(tai);
    frames_.emplace(tai, std::move(frame));
    return true;
}

void FrameRing::protect(std::uint64_t beginNs, std::uint64_t endNs)
{
    if (endNs < beginNs)
    {
        std::swap(beginNs, endNs);
    }
    ranges_.push_back(ProtectRange{beginNs, endNs});
    for (auto& [tai, frame] : frames_)
    {
        if (tai >= beginNs && tai <= endNs)
        {
            frame.protect = true;
        }
    }
}

void FrameRing::unprotect(std::uint64_t beginNs, std::uint64_t endNs)
{
    ranges_.erase(std::remove_if(ranges_.begin(), ranges_.end(),
                      [&](ProtectRange const& range) { return range.beginNs == beginNs && range.endNs == endNs; }),
        ranges_.end());
    for (auto& [tai, frame] : frames_)
    {
        frame.protect = isProtected(tai);
    }
}

bool FrameRing::isProtected(std::uint64_t taiNs) const
{
    return covered(ranges_, taiNs);
}

std::optional<StoredFrame> FrameRing::findNearest(std::uint64_t taiNs) const
{
    if (frames_.empty())
    {
        return std::nullopt;
    }
    auto it = frames_.lower_bound(taiNs);
    if (it == frames_.end())
    {
        return std::prev(it)->second;
    }
    if (it == frames_.begin() || it->first == taiNs)
    {
        return it->second;
    }
    auto prev = std::prev(it);
    if (taiNs - prev->first <= it->first - taiNs)
    {
        return prev->second;
    }
    return it->second;
}

std::optional<StoredFrame> FrameRing::findAtOrBefore(std::uint64_t taiNs) const
{
    if (frames_.empty())
    {
        return std::nullopt;
    }
    auto it = frames_.upper_bound(taiNs);
    if (it == frames_.begin())
    {
        return std::nullopt;
    }
    return std::prev(it)->second;
}

std::optional<StoredFrame> FrameRing::findAfter(std::uint64_t taiNs) const
{
    auto it = frames_.upper_bound(taiNs);
    if (it == frames_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::size_t FrameRing::size() const
{
    return frames_.size();
}

std::size_t FrameRing::protectedCount() const
{
    std::size_t n = 0;
    for (auto const& [tai, frame] : frames_)
    {
        (void)tai;
        if (frame.protect)
        {
            ++n;
        }
    }
    return n;
}

std::uint64_t FrameRing::payloadBytes() const
{
    std::uint64_t n = 0;
    for (auto const& [tai, frame] : frames_)
    {
        (void)tai;
        n += frame.jpeg.size();
        n += frame.audio.size() * sizeof(float);
    }
    return n;
}

std::uint64_t FrameRing::protectedBytes() const
{
    std::uint64_t n = 0;
    for (auto const& [tai, frame] : frames_)
    {
        (void)tai;
        if (frame.protect)
        {
            n += frame.jpeg.size();
            n += frame.audio.size() * sizeof(float);
        }
    }
    return n;
}

double FrameRing::protectedPercent() const
{
    auto const total = payloadBytes();
    if (total == 0)
    {
        return 0;
    }
    return 100.0 * static_cast<double>(protectedBytes()) / static_cast<double>(total);
}

SegmentWriter::SegmentWriter(std::string directory, int camera, int segmentSeconds, bool direct)
    : directory_(std::move(directory))
    , camera_(camera)
    , segmentSeconds_(segmentSeconds)
    , wantDirect_(direct)
{
    std::filesystem::create_directories(directory_);
}

SegmentWriter::~SegmentWriter()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
}

void SegmentWriter::rotate()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    auto const path = directory_ + "/cam" + std::to_string(camera_) + "_seg" + std::to_string(segment_) + ".bin";
    int flags = O_CREAT | O_WRONLY | O_TRUNC;
#ifdef O_DIRECT
    if (wantDirect_)
    {
        flags |= O_DIRECT;
    }
#endif
    fd_ = ::open(path.c_str(), flags, 0644);
    directActive_ = wantDirect_ && fd_ >= 0;
    if (fd_ < 0 && wantDirect_)
    {
        fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        directActive_ = false;
    }
    char const magic[4] = {'M', 'X', 'L', 'R'};
    if (fd_ >= 0)
    {
        std::uint32_t const version = 1;
        if (::write(fd_, magic, 4) < 0 || ::write(fd_, &version, sizeof(version)) < 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }
}

bool SegmentWriter::write(std::uint64_t taiNs, std::uint8_t const* jpeg, std::size_t jpegSize, float const* audio, std::size_t audioSamples)
{
    if (fd_ < 0 || (segmentStart_ != 0 && taiNs > segmentStart_ + static_cast<std::uint64_t>(segmentSeconds_) * 1000000000ull))
    {
        if (fd_ >= 0)
        {
            ++segment_;
        }
        segmentStart_ = taiNs;
        rotate();
    }
    if (fd_ < 0)
    {
        return false;
    }
    auto const t0 = std::chrono::steady_clock::now();
    std::uint32_t const jsize = static_cast<std::uint32_t>(jpegSize);
    std::uint32_t const asize = static_cast<std::uint32_t>(audioSamples);
    bool ok = ::write(fd_, &taiNs, sizeof(taiNs)) == static_cast<ssize_t>(sizeof(taiNs));
    ok = ok && ::write(fd_, &jsize, sizeof(jsize)) == static_cast<ssize_t>(sizeof(jsize));
    ok = ok && ::write(fd_, &asize, sizeof(asize)) == static_cast<ssize_t>(sizeof(asize));
    if (jpegSize > 0)
    {
        ok = ok && ::write(fd_, jpeg, jpegSize) == static_cast<ssize_t>(jpegSize);
    }
    if (audioSamples > 0 && audio != nullptr)
    {
        ok = ok && ::write(fd_, audio, audioSamples * sizeof(float)) == static_cast<ssize_t>(audioSamples * sizeof(float));
    }
    auto const dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double const bytes = static_cast<double>(sizeof(taiNs) + 8 + jpegSize + audioSamples * sizeof(float));
    lastBps_ = dt > 0 ? bytes / dt : 0;
    return ok;
}

double measureWriteThroughput(std::string const& directory, std::size_t bytes)
{
    std::filesystem::create_directories(directory);
    auto const path = directory + "/throughput.bin";
    int const fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
    {
        return 0;
    }
    std::vector<char> block(std::min<std::size_t>(bytes, 1 << 20), 0);
    auto const t0 = std::chrono::steady_clock::now();
    std::size_t left = bytes;
    while (left > 0)
    {
        std::size_t const n = std::min(left, block.size());
        if (::write(fd, block.data(), n) != static_cast<ssize_t>(n))
        {
            break;
        }
        left -= n;
    }
    ::fsync(fd);
    ::close(fd);
    std::filesystem::remove(path);
    auto const dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return dt > 0 ? static_cast<double>(bytes - left) / dt : 0;
}
} // namespace replay
