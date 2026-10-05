#include "record/ring.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <list>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>
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

void removeFiles(std::vector<std::string> const& paths)
{
    for (auto const& path : paths)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}
} // namespace

struct FrameRing::Impl
{
    std::string directory;
    std::uint64_t retentionNs = 0;
    std::uint64_t segmentNs = 0;
    // Guards the index, segments, ranges, totals and read descriptors. Never held during a read or write.
    mutable std::mutex mutex;
    // One writer at a time; held across the write itself.
    std::mutex writeMutex;
    std::deque<Entry> index;
    std::map<std::uint32_t, Segment> segments;
    std::uint32_t nextSegment = 0;
    std::vector<ProtectRange> ranges;
    std::uint64_t payloadTotal = 0;
    std::size_t protectedFrames = 0;
    mutable std::list<std::pair<std::uint32_t, int>> readFds;
    std::uint64_t writeFailures = 0;
    std::uint64_t failedRun = 0;

    // Background I/O: a finished segment's writeback and page-cache drop, and deletions. On a
    // slow disk sync_file_range blocked the recorder for about a second at every rotation.
    std::thread janitor;
    std::mutex janitorMutex;
    std::condition_variable janitorWake;
    std::vector<int> finished;
    std::vector<std::string> doomed;
    bool stopping = false;

    void janitorLoop()
    {
        // The previous finished segment, dropped from the page cache when the next one is
        // handed over (its writeback has finished by then).
        int adviseFd = -1;
        std::unique_lock lock{janitorMutex};
        for (;;)
        {
            janitorWake.wait(lock, [&] { return stopping || !finished.empty() || !doomed.empty(); });
            if (stopping && finished.empty() && doomed.empty())
            {
                break;
            }
            auto fds = std::move(finished);
            auto paths = std::move(doomed);
            finished.clear();
            doomed.clear();
            lock.unlock();
            for (int const fd : fds)
            {
                ::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
                if (adviseFd >= 0)
                {
                    ::posix_fadvise(adviseFd, 0, 0, POSIX_FADV_DONTNEED);
                    ::close(adviseFd);
                }
                adviseFd = fd;
            }
            removeFiles(paths);
            lock.lock();
        }
        if (adviseFd >= 0)
        {
            ::close(adviseFd);
        }
    }

    void handOver(int fd, std::vector<std::string> paths)
    {
        {
            std::lock_guard lock{janitorMutex};
            if (fd >= 0)
            {
                finished.push_back(fd);
            }
            doomed.insert(doomed.end(), std::make_move_iterator(paths.begin()), std::make_move_iterator(paths.end()));
        }
        janitorWake.notify_one();
    }

    ~Impl()
    {
        if (janitor.joinable())
        {
            {
                std::lock_guard lock{janitorMutex};
                stopping = true;
            }
            janitorWake.notify_one();
            janitor.join();
        }
        for (auto& [seq, segment] : segments)
        {
            (void)seq;
            if (segment.writeFd >= 0)
            {
                ::close(segment.writeFd);
            }
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

    // The entry nearest to `tai` (the earlier one on a tie); end() when the index is empty.
    std::deque<Entry>::const_iterator nearest(std::uint64_t tai) const
    {
        if (index.empty())
        {
            return index.end();
        }
        auto const it = lower(tai);
        if (it == index.end())
        {
            return std::prev(it);
        }
        if (it == index.begin() || it->taiNs == tai)
        {
            return it;
        }
        auto const prev = std::prev(it);
        return tai - prev->taiNs <= it->taiNs - tai ? prev : it;
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

    // Reads `entry` (held under `lock`) and releases the lock before the reads: they use a
    // duplicate descriptor, which stays valid when the cache closes its own or the segment
    // is deleted.
    std::optional<StoredFrame> load(Entry const& entry, std::unique_lock<std::mutex>& lock, bool withJpeg) const
    {
        Entry const copy = entry;
        int const cached = readFd(copy.segment);
        int const fd = cached < 0 ? -1 : ::fcntl(cached, F_DUPFD_CLOEXEC, 0);
        lock.unlock();
        if (fd < 0)
        {
            return std::nullopt;
        }
        StoredFrame frame;
        frame.taiNs = copy.taiNs;
        frame.protect = copy.protect;
        frame.audio.resize(copy.audioSamples);
        auto const data = copy.offset + kRecordHeaderBytes;
        bool ok = readAll(fd, frame.audio.data(), frame.audio.size() * sizeof(float), data + copy.jpegBytes);
        if (ok && withJpeg)
        {
            frame.jpeg.resize(copy.jpegBytes);
            ok = readAll(fd, frame.jpeg.data(), frame.jpeg.size(), data);
        }
        ::close(fd);
        if (!ok)
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
            ++protectedFrames;
        }
        else
        {
            --protectedFrames;
        }
    }

    // Counts a frame that could not be written and logs the first one of a run.
    void failed(char const* event, std::string const& path, int error)
    {
        ++writeFailures;
        if (failedRun++ == 0)
        {
            logError(event, {{"path", path}, {"error", std::strerror(error)}});
        }
    }

    void recovered()
    {
        if (failedRun > 0)
        {
            logInfo("segment_write_recovered", {{"directory", directory}, {"dropped", std::to_string(failedRun)}});
            failedRun = 0;
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

    // Drops expired, unprotected segments from the index (lock held) and returns their
    // files, which the caller deletes after releasing the lock.
    std::vector<std::string> retain()
    {
        std::vector<std::string> expired;
        if (index.empty() || retentionNs == 0 || index.back().taiNs <= retentionNs)
        {
            return expired;
        }
        auto const cutoff = index.back().taiNs - retentionNs;
        for (auto it = segments.begin(); it != segments.end();)
        {
            auto& segment = it->second;
            if (segment.writeFd >= 0 || segment.lastNs >= cutoff || overlaps(ranges, segment.firstNs, segment.lastNs))
            {
                ++it;
                continue;
            }
            auto const seq = it->first;
            auto const first = lower(segment.firstNs);
            auto const last = upper(segment.lastNs);
            auto const kept = std::remove_if(first, last, [&](Entry const& entry) {
                if (entry.segment != seq)
                {
                    return false;
                }
                payloadTotal -= payload(entry);
                return true;
            });
            index.erase(kept, last);
            dropReadFd(seq);
            expired.push_back(segment.path);
            it = segments.erase(it);
        }
        return expired;
    }

    // Starts a new segment at `tai`. Called with writeMutex held; takes the lock only
    // to swap the segments, and hands the finished one to the janitor.
    bool rotate(std::uint64_t tai)
    {
        int previous = -1;
        {
            std::lock_guard lock{mutex};
            if (auto* open = current())
            {
                previous = open->writeFd;
                open->writeFd = -1;
            }
        }
        if (previous >= 0)
        {
            handOver(previous, {});
        }
        char name[48];
        std::snprintf(name, sizeof(name), "seg-%020llu.bin", static_cast<unsigned long long>(tai));
        auto const path = directory + "/" + name;
        int const fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            int const error = errno;
            std::lock_guard lock{mutex};
            failed("segment_open_failed", path, error);
            return false;
        }
        if (!writeAll(fd, kMagic, sizeof(kMagic)) || !writeAll(fd, &kVersion, sizeof(kVersion)))
        {
            int const error = errno;
            ::close(fd);
            // On a full disk every frame would otherwise leave an empty file behind.
            ::unlink(path.c_str());
            std::lock_guard lock{mutex};
            failed("segment_write_failed", path, error);
            return false;
        }
        Segment segment;
        segment.path = path;
        segment.firstNs = tai;
        segment.lastNs = tai;
        segment.bytes = kFileHeaderBytes;
        segment.writeFd = fd;
        std::vector<std::string> expired;
        {
            std::lock_guard lock{mutex};
            segments.emplace(nextSegment++, std::move(segment));
            expired = retain();
        }
        handOver(-1, std::move(expired));
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
    impl_->janitor = std::thread([impl = impl_.get()] { impl->janitorLoop(); });
}

FrameRing::~FrameRing() = default;
FrameRing::FrameRing(FrameRing&&) noexcept = default;
FrameRing& FrameRing::operator=(FrameRing&&) noexcept = default;

bool FrameRing::push(StoredFrame frame)
{
    auto& d = *impl_;
    std::lock_guard writer{d.writeMutex};
    auto const tai = frame.taiNs;
    bool replace = false;
    bool open = false;
    {
        std::lock_guard lock{d.mutex};
        if (!d.index.empty() && tai <= d.index.back().taiNs)
        {
            if (tai < d.index.back().taiNs || d.index.back().protect)
            {
                return false;
            }
            replace = true;
        }
        auto const* segment = d.current();
        open = segment != nullptr && tai <= segment->firstNs + d.segmentNs;
    }
    if (!open && !d.rotate(tai))
    {
        return false;
    }
    // Only this writer closes or rotates the open segment, so it stays valid unlocked.
    int fd = -1;
    std::string path;
    Entry entry;
    {
        std::lock_guard lock{d.mutex};
        auto const* segment = d.current();
        fd = segment->writeFd;
        path = segment->path;
        entry.offset = segment->bytes;
        entry.segment = d.segments.rbegin()->first;
    }
    entry.taiNs = tai;
    entry.jpegBytes = static_cast<std::uint32_t>(frame.jpeg.size());
    entry.audioSamples = static_cast<std::uint32_t>(frame.audio.size());
    unsigned char record[kRecordHeaderBytes];
    std::memcpy(record, &entry.taiNs, 8);
    std::memcpy(record + 8, &entry.jpegBytes, 4);
    std::memcpy(record + 12, &entry.audioSamples, 4);
    bool const ok = writeAll(fd, record, sizeof(record)) && writeAll(fd, frame.jpeg.data(), frame.jpeg.size()) &&
                    writeAll(fd, frame.audio.data(), frame.audio.size() * sizeof(float));
    int const error = ok ? 0 : errno;
    std::lock_guard lock{d.mutex};
    auto& segment = d.segments.at(entry.segment);
    if (!ok)
    {
        // The segment may now end in a partial record: close it; the next frame opens a new one.
        d.failed("segment_write_failed", path, error);
        ::close(fd);
        segment.writeFd = -1;
        return false;
    }
    d.recovered();
    segment.bytes += kRecordHeaderBytes + payload(entry);
    segment.lastNs = tai;
    if (replace && !d.index.empty() && d.index.back().taiNs == tai)
    {
        d.setProtect(d.index.back(), false);
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
    std::lock_guard lock{d.mutex};
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
    std::lock_guard lock{d.mutex};
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
    std::vector<std::string> expired;
    {
        std::lock_guard lock{d.mutex};
        expired = d.retain();
    }
    d.handOver(-1, std::move(expired));
}

bool FrameRing::isProtected(std::uint64_t taiNs) const
{
    std::lock_guard lock{impl_->mutex};
    return covered(impl_->ranges, taiNs);
}

std::optional<StoredFrame> FrameRing::findNearest(std::uint64_t taiNs) const
{
    auto const& d = *impl_;
    std::unique_lock lock{d.mutex};
    auto const it = d.nearest(taiNs);
    if (it == d.index.end())
    {
        return std::nullopt;
    }
    return d.load(*it, lock, true);
}

std::vector<float> FrameRing::findNearestAudio(std::uint64_t taiNs) const
{
    auto const& d = *impl_;
    std::unique_lock lock{d.mutex};
    auto const it = d.nearest(taiNs);
    if (it == d.index.end())
    {
        return {};
    }
    auto frame = d.load(*it, lock, false);
    return frame ? std::move(frame->audio) : std::vector<float>{};
}

std::optional<StoredFrame> FrameRing::findAtOrBefore(std::uint64_t taiNs) const
{
    auto const& d = *impl_;
    std::unique_lock lock{d.mutex};
    auto const it = d.upper(taiNs);
    if (it == d.index.begin())
    {
        return std::nullopt;
    }
    return d.load(*std::prev(it), lock, true);
}

std::optional<StoredFrame> FrameRing::findAfter(std::uint64_t taiNs) const
{
    auto const& d = *impl_;
    std::unique_lock lock{d.mutex};
    auto const it = d.upper(taiNs);
    if (it == d.index.end())
    {
        return std::nullopt;
    }
    return d.load(*it, lock, true);
}

std::size_t FrameRing::size() const
{
    std::lock_guard lock{impl_->mutex};
    return impl_->index.size();
}

std::uint64_t FrameRing::newestNs() const
{
    std::lock_guard lock{impl_->mutex};
    return impl_->index.empty() ? 0 : impl_->index.back().taiNs;
}

std::size_t FrameRing::protectedCount() const
{
    std::lock_guard lock{impl_->mutex};
    return impl_->protectedFrames;
}

std::uint64_t FrameRing::payloadBytes() const
{
    std::lock_guard lock{impl_->mutex};
    return impl_->payloadTotal;
}

std::uint64_t FrameRing::protectedBytes() const
{
    std::lock_guard lock{impl_->mutex};
    std::uint64_t total = 0;
    for (auto const& [seq, segment] : impl_->segments)
    {
        (void)seq;
        if (overlaps(impl_->ranges, segment.firstNs, segment.lastNs))
        {
            total += segment.bytes;
        }
    }
    return total;
}

double FrameRing::protectedPercent() const
{
    auto const total = diskBytes();
    if (total == 0)
    {
        return 0;
    }
    return 100.0 * static_cast<double>(protectedBytes()) / static_cast<double>(total);
}

std::uint64_t FrameRing::diskBytes() const
{
    std::lock_guard lock{impl_->mutex};
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
    std::lock_guard lock{impl_->mutex};
    return impl_->segments.size();
}

std::uint64_t FrameRing::writeFailures() const
{
    std::lock_guard lock{impl_->mutex};
    return impl_->writeFailures;
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
