#include "record/ring.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <list>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace replay
{
namespace
{
// Segment file: "MXLR", version 1, then records {u64 TAI ns, u32 JPEG bytes,
// u32 audio samples, JPEG, float audio}. The 1.0.x writer used the same layout.
constexpr char kMagic[4] = {'M', 'X', 'L', 'R'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint64_t kFileHeaderBytes = 8;
constexpr std::uint64_t kRecordHeaderBytes = 16;
constexpr std::uint32_t kMaxJpegBytes = 64u << 20;
constexpr std::uint32_t kMaxAudioSamples = 1u << 22;
constexpr std::size_t kReadDescriptors = 8;

struct Entry
{
    std::uint64_t taiNs = 0;
    std::uint64_t offset = 0;
    std::uint32_t segment = 0;
    std::uint32_t jpegBytes = 0;
    std::uint32_t audioSamples = 0;
    bool protect = false;
};

struct Segment
{
    std::string path;
    std::uint64_t firstNs = 0;
    std::uint64_t lastNs = 0;
    std::uint64_t bytes = 0;
    int writeFd = -1;
};

std::uint64_t payload(Entry const& entry)
{
    return entry.jpegBytes + static_cast<std::uint64_t>(entry.audioSamples) * sizeof(float);
}

bool covered(std::vector<ProtectRange> const& ranges, std::uint64_t tai)
{
    return std::any_of(ranges.begin(), ranges.end(), [&](ProtectRange const& range) { return tai >= range.beginNs && tai <= range.endNs; });
}

bool overlaps(std::vector<ProtectRange> const& ranges, std::uint64_t first, std::uint64_t last)
{
    return std::any_of(ranges.begin(), ranges.end(), [&](ProtectRange const& range) { return range.beginNs <= last && range.endNs >= first; });
}

bool writeAll(int fd, void const* data, std::size_t size)
{
    auto const* p = static_cast<char const*>(data);
    while (size > 0)
    {
        auto const n = ::write(fd, p, size);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool readAll(int fd, void* data, std::size_t size, std::uint64_t offset)
{
    auto* p = static_cast<char*>(data);
    while (size > 0)
    {
        auto const n = ::pread(fd, p, size, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            return false;
        }
        p += n;
        size -= static_cast<std::size_t>(n);
        offset += static_cast<std::uint64_t>(n);
    }
    return true;
}

bool endsWith(std::string const& text, std::string const& suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isSegmentName(std::string const& name)
{
    return name.rfind("seg-", 0) == 0 && endsWith(name, ".bin");
}

// 1.0.x wrote cam<N>_seg<k>.bin, numbered from 0 on every start, and never read them.
bool isLegacyName(std::string const& name)
{
    return name.rfind("cam", 0) == 0 && name.find("_seg") != std::string::npos && endsWith(name, ".bin");
}
} // namespace

struct FrameRing::Impl
{
    std::string directory;
    std::uint64_t retentionNs = 0;
    std::uint64_t segmentNs = 0;
    std::deque<Entry> index;
    std::map<std::uint32_t, Segment> segments;
    std::uint32_t nextSegment = 0;
    std::vector<ProtectRange> ranges;
    std::uint64_t payloadTotal = 0;
    std::uint64_t protectedTotal = 0;
    std::size_t protectedFrames = 0;
    // The previous segment's writer, dropped from the page cache at the next rotation
    // (its writeback has finished by then).
    int adviseFd = -1;
    mutable std::list<std::pair<std::uint32_t, int>> readFds;
    bool writeErrorLogged = false;

    ~Impl()
    {
        for (auto& [seq, segment] : segments)
        {
            (void)seq;
            if (segment.writeFd >= 0)
            {
                ::close(segment.writeFd);
            }
        }
        if (adviseFd >= 0)
        {
            ::close(adviseFd);
        }
        for (auto const& [seq, fd] : readFds)
        {
            (void)seq;
            ::close(fd);
        }
    }

    std::deque<Entry>::iterator lower(std::uint64_t tai)
    {
        return std::lower_bound(index.begin(), index.end(), tai, [](Entry const& e, std::uint64_t v) { return e.taiNs < v; });
    }
    std::deque<Entry>::iterator upper(std::uint64_t tai)
    {
        return std::upper_bound(index.begin(), index.end(), tai, [](std::uint64_t v, Entry const& e) { return v < e.taiNs; });
    }
    std::deque<Entry>::const_iterator lower(std::uint64_t tai) const
    {
        return std::lower_bound(index.begin(), index.end(), tai, [](Entry const& e, std::uint64_t v) { return e.taiNs < v; });
    }
    std::deque<Entry>::const_iterator upper(std::uint64_t tai) const
    {
        return std::upper_bound(index.begin(), index.end(), tai, [](std::uint64_t v, Entry const& e) { return v < e.taiNs; });
    }

    Segment* current()
    {
        if (segments.empty() || segments.rbegin()->second.writeFd < 0)
        {
            return nullptr;
        }
        return &segments.rbegin()->second;
    }

    int readFd(std::uint32_t seq) const
    {
        for (auto it = readFds.begin(); it != readFds.end(); ++it)
        {
            if (it->first == seq)
            {
                readFds.splice(readFds.begin(), readFds, it);
                return it->second;
            }
        }
        auto const segment = segments.find(seq);
        if (segment == segments.end())
        {
            return -1;
        }
        int const fd = ::open(segment->second.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            return -1;
        }
        readFds.emplace_front(seq, fd);
        if (readFds.size() > kReadDescriptors)
        {
            ::close(readFds.back().second);
            readFds.pop_back();
        }
        return fd;
    }

    void dropReadFd(std::uint32_t seq)
    {
        for (auto it = readFds.begin(); it != readFds.end(); ++it)
        {
            if (it->first == seq)
            {
                ::close(it->second);
                readFds.erase(it);
                return;
            }
        }
    }

    std::optional<StoredFrame> read(Entry const& entry) const
    {
        int const fd = readFd(entry.segment);
        if (fd < 0)
        {
            return std::nullopt;
        }
        StoredFrame frame;
        frame.taiNs = entry.taiNs;
        frame.protect = entry.protect;
        frame.jpeg.resize(entry.jpegBytes);
        frame.audio.resize(entry.audioSamples);
        auto const data = entry.offset + kRecordHeaderBytes;
        if (!readAll(fd, frame.jpeg.data(), frame.jpeg.size(), data) ||
            !readAll(fd, frame.audio.data(), frame.audio.size() * sizeof(float), data + entry.jpegBytes))
        {
            return std::nullopt;
        }
        return frame;
    }

    void setProtect(Entry& entry, bool value)
    {
        if (entry.protect == value)
        {
            return;
        }
        entry.protect = value;
        if (value)
        {
            protectedTotal += payload(entry);
            ++protectedFrames;
        }
        else
        {
            protectedTotal -= payload(entry);
            --protectedFrames;
        }
    }

    void loadSegment(std::filesystem::path const& path)
    {
        int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            return;
        }
        struct stat st{};
        char header[kFileHeaderBytes];
        bool const valid = ::fstat(fd, &st) == 0 && static_cast<std::uint64_t>(st.st_size) >= kFileHeaderBytes &&
                           readAll(fd, header, sizeof(header), 0) && std::memcmp(header, kMagic, sizeof(kMagic)) == 0;
        auto const size = static_cast<std::uint64_t>(st.st_size);
        std::uint32_t const seq = nextSegment++;
        Segment segment;
        segment.path = path.string();
        segment.bytes = kFileHeaderBytes;
        bool any = false;
        std::uint64_t offset = kFileHeaderBytes;
        while (valid && offset + kRecordHeaderBytes <= size)
        {
            unsigned char record[kRecordHeaderBytes];
            if (!readAll(fd, record, sizeof(record), offset))
            {
                break;
            }
            Entry entry;
            std::memcpy(&entry.taiNs, record, 8);
            std::memcpy(&entry.jpegBytes, record + 8, 4);
            std::memcpy(&entry.audioSamples, record + 12, 4);
            auto const end = offset + kRecordHeaderBytes + payload(entry);
            // A record cut off by a crash or a full disk ends the segment.
            if (entry.jpegBytes > kMaxJpegBytes || entry.audioSamples > kMaxAudioSamples || end > size)
            {
                break;
            }
            if (index.empty() || entry.taiNs > index.back().taiNs)
            {
                entry.offset = offset;
                entry.segment = seq;
                index.push_back(entry);
                payloadTotal += payload(entry);
                if (!any)
                {
                    segment.firstNs = entry.taiNs;
                }
                segment.lastNs = entry.taiNs;
                any = true;
            }
            offset = end;
            segment.bytes = end;
        }
        ::close(fd);
        if (!any)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return;
        }
        segments.emplace(seq, std::move(segment));
    }

    void scan()
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        std::vector<std::filesystem::path> files;
        int legacy = 0;
        for (auto const& item : std::filesystem::directory_iterator(directory, ec))
        {
            auto const name = item.path().filename().string();
            if (isLegacyName(name))
            {
                std::error_code removeError;
                std::filesystem::remove(item.path(), removeError);
                ++legacy;
            }
            else if (isSegmentName(name))
            {
                files.push_back(item.path());
            }
        }
        if (legacy > 0)
        {
            logInfo("legacy_segments_removed", {{"directory", directory}, {"files", std::to_string(legacy)}});
        }
        // seg-<20-digit TAI>.bin: name order is time order.
        std::sort(files.begin(), files.end());
        for (auto const& path : files)
        {
            loadSegment(path);
        }
        if (!index.empty())
        {
            logInfo("buffer_reopened", {{"directory", directory}, {"segments", std::to_string(segments.size())}, {"frames", std::to_string(index.size())}});
        }
    }

    bool rotate(std::uint64_t tai, FrameRing& ring)
    {
        if (auto* open = current())
        {
            int const fd = open->writeFd;
            open->writeFd = -1;
            ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
            if (adviseFd >= 0)
            {
                ::posix_fadvise(adviseFd, 0, 0, POSIX_FADV_DONTNEED);
                ::close(adviseFd);
            }
            adviseFd = fd;
        }
        char name[48];
        std::snprintf(name, sizeof(name), "seg-%020llu.bin", static_cast<unsigned long long>(tai));
        auto const path = directory + "/" + name;
        int const fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            logError("segment_open_failed", {{"path", path}, {"error", std::strerror(errno)}});
            return false;
        }
        if (!writeAll(fd, kMagic, sizeof(kMagic)) || !writeAll(fd, &kVersion, sizeof(kVersion)))
        {
            ::close(fd);
            return false;
        }
        Segment segment;
        segment.path = path;
        segment.firstNs = tai;
        segment.lastNs = tai;
        segment.bytes = kFileHeaderBytes;
        segment.writeFd = fd;
        segments.emplace(nextSegment++, std::move(segment));
        ring.enforceRetention();
        return true;
    }
};

FrameRing::FrameRing(std::string directory, std::uint64_t retentionNs, int segmentSeconds)
    : impl_(std::make_unique<Impl>())
{
    impl_->directory = std::move(directory);
    impl_->retentionNs = retentionNs;
    impl_->segmentNs = static_cast<std::uint64_t>(std::max(1, segmentSeconds)) * 1000000000ull;
    impl_->scan();
}

FrameRing::~FrameRing() = default;
FrameRing::FrameRing(FrameRing&&) noexcept = default;
FrameRing& FrameRing::operator=(FrameRing&&) noexcept = default;

bool FrameRing::push(StoredFrame frame)
{
    auto& d = *impl_;
    auto const tai = frame.taiNs;
    bool replace = false;
    if (!d.index.empty() && tai <= d.index.back().taiNs)
    {
        if (tai < d.index.back().taiNs || d.index.back().protect)
        {
            return false;
        }
        replace = true;
    }
    auto* segment = d.current();
    if (segment == nullptr || tai > segment->firstNs + d.segmentNs)
    {
        if (!d.rotate(tai, *this))
        {
            return false;
        }
        segment = d.current();
    }
    Entry entry;
    entry.taiNs = tai;
    entry.offset = segment->bytes;
    entry.segment = d.segments.rbegin()->first;
    entry.jpegBytes = static_cast<std::uint32_t>(frame.jpeg.size());
    entry.audioSamples = static_cast<std::uint32_t>(frame.audio.size());
    unsigned char record[kRecordHeaderBytes];
    std::memcpy(record, &entry.taiNs, 8);
    std::memcpy(record + 8, &entry.jpegBytes, 4);
    std::memcpy(record + 12, &entry.audioSamples, 4);
    bool const ok = writeAll(segment->writeFd, record, sizeof(record)) && writeAll(segment->writeFd, frame.jpeg.data(), frame.jpeg.size()) &&
                    writeAll(segment->writeFd, frame.audio.data(), frame.audio.size() * sizeof(float));
    if (!ok)
    {
        // The segment may now end in a partial record: close it; the next frame opens a new one.
        if (!d.writeErrorLogged)
        {
            logError("segment_write_failed", {{"path", segment->path}, {"error", std::strerror(errno)}});
            d.writeErrorLogged = true;
        }
        ::close(segment->writeFd);
        segment->writeFd = -1;
        return false;
    }
    d.writeErrorLogged = false;
    segment->bytes += kRecordHeaderBytes + payload(entry);
    segment->lastNs = tai;
    if (replace)
    {
        d.payloadTotal -= payload(d.index.back());
        d.index.pop_back();
    }
    d.index.push_back(entry);
    d.payloadTotal += payload(entry);
    d.setProtect(d.index.back(), covered(d.ranges, tai));
    return true;
}

void FrameRing::protect(std::uint64_t beginNs, std::uint64_t endNs)
{
    auto& d = *impl_;
    if (endNs < beginNs)
    {
        std::swap(beginNs, endNs);
    }
    d.ranges.push_back(ProtectRange{beginNs, endNs});
    for (auto it = d.lower(beginNs); it != d.index.end() && it->taiNs <= endNs; ++it)
    {
        d.setProtect(*it, true);
    }
}

void FrameRing::unprotect(std::uint64_t beginNs, std::uint64_t endNs)
{
    auto& d = *impl_;
    d.ranges.erase(std::remove_if(d.ranges.begin(), d.ranges.end(),
                       [&](ProtectRange const& range) { return range.beginNs == beginNs && range.endNs == endNs; }),
        d.ranges.end());
    for (auto it = d.lower(std::min(beginNs, endNs)); it != d.index.end() && it->taiNs <= std::max(beginNs, endNs); ++it)
    {
        d.setProtect(*it, covered(d.ranges, it->taiNs));
    }
}

void FrameRing::enforceRetention()
{
    auto& d = *impl_;
    if (d.index.empty() || d.retentionNs == 0 || d.index.back().taiNs <= d.retentionNs)
    {
        return;
    }
    auto const cutoff = d.index.back().taiNs - d.retentionNs;
    for (auto it = d.segments.begin(); it != d.segments.end();)
    {
        auto& segment = it->second;
        if (segment.writeFd >= 0 || segment.lastNs >= cutoff || overlaps(d.ranges, segment.firstNs, segment.lastNs))
        {
            ++it;
            continue;
        }
        auto const seq = it->first;
        auto const first = d.lower(segment.firstNs);
        auto const last = d.upper(segment.lastNs);
        auto const kept = std::remove_if(first, last, [&](Entry const& entry) {
            if (entry.segment != seq)
            {
                return false;
            }
            d.payloadTotal -= payload(entry);
            return true;
        });
        d.index.erase(kept, last);
        d.dropReadFd(seq);
        std::error_code ec;
        std::filesystem::remove(segment.path, ec);
        it = d.segments.erase(it);
    }
}

bool FrameRing::isProtected(std::uint64_t taiNs) const
{
    return covered(impl_->ranges, taiNs);
}

std::optional<StoredFrame> FrameRing::findNearest(std::uint64_t taiNs) const
{
    auto const& d = *impl_;
    if (d.index.empty())
    {
        return std::nullopt;
    }
    auto it = d.lower(taiNs);
    if (it == d.index.end())
    {
        return d.read(*std::prev(it));
    }
    if (it == d.index.begin() || it->taiNs == taiNs)
    {
        return d.read(*it);
    }
    auto const prev = std::prev(it);
    if (taiNs - prev->taiNs <= it->taiNs - taiNs)
    {
        return d.read(*prev);
    }
    return d.read(*it);
}

std::optional<StoredFrame> FrameRing::findAtOrBefore(std::uint64_t taiNs) const
{
    auto const& d = *impl_;
    auto const it = d.upper(taiNs);
    if (it == d.index.begin())
    {
        return std::nullopt;
    }
    return d.read(*std::prev(it));
}

std::optional<StoredFrame> FrameRing::findAfter(std::uint64_t taiNs) const
{
    auto const& d = *impl_;
    auto const it = d.upper(taiNs);
    if (it == d.index.end())
    {
        return std::nullopt;
    }
    return d.read(*it);
}

std::size_t FrameRing::size() const
{
    return impl_->index.size();
}

std::size_t FrameRing::protectedCount() const
{
    return impl_->protectedFrames;
}

std::uint64_t FrameRing::payloadBytes() const
{
    return impl_->payloadTotal;
}

std::uint64_t FrameRing::protectedBytes() const
{
    return impl_->protectedTotal;
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

std::uint64_t FrameRing::diskBytes() const
{
    std::uint64_t total = 0;
    for (auto const& [seq, segment] : impl_->segments)
    {
        (void)seq;
        total += segment.bytes;
    }
    return total;
}

std::size_t FrameRing::segmentCount() const
{
    return impl_->segments.size();
}

std::uint64_t segmentBytes(std::string const& directory)
{
    std::uint64_t total = 0;
    std::error_code ec;
    for (auto const& item : std::filesystem::directory_iterator(directory, ec))
    {
        std::error_code sizeError;
        if (isSegmentName(item.path().filename().string()))
        {
            auto const size = item.file_size(sizeError);
            if (!sizeError)
            {
                total += size;
            }
        }
    }
    return total;
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
