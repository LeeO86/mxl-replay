#include "app/engine.hpp"

#include "version.hpp"

#include "flow/cuda_flow.hpp"
#include "media/anc.hpp"
#include "media/audio.hpp"
#include "media/convert.hpp"
#include "media/jpeg.hpp"
#include "media/scale.hpp"
#include "media/timebase.hpp"
#include "media/v210.hpp"
#include "record/hfr.hpp"
#include "util/json.hpp"
#include "util/logging.hpp"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace replay
{
namespace
{
Yuv422 toFloat(Frame10 const& frame)
{
    Yuv422 out;
    out.width = frame.width;
    out.height = frame.height;
    out.y.resize(frame.y.size());
    out.cb.resize(frame.cb.size());
    out.cr.resize(frame.cr.size());
    for (std::size_t i = 0; i < frame.y.size(); ++i)
    {
        out.y[i] = static_cast<float>(frame.y[i]) / 1023.f;
    }
    for (std::size_t i = 0; i < frame.cb.size(); ++i)
    {
        out.cb[i] = static_cast<float>(frame.cb[i]) / 1023.f;
        out.cr[i] = static_cast<float>(frame.cr[i]) / 1023.f;
    }
    return out;
}

Frame10 fromFloat(Yuv422 const& frame)
{
    Frame10 out;
    out.allocate(frame.width, frame.height);
    for (std::size_t i = 0; i < out.y.size() && i < frame.y.size(); ++i)
    {
        out.y[i] = static_cast<std::uint16_t>(std::clamp(frame.y[i], 0.f, 1.f) * 1023.f);
    }
    for (std::size_t i = 0; i < out.cb.size() && i < frame.cb.size(); ++i)
    {
        out.cb[i] = static_cast<std::uint16_t>(std::clamp(frame.cb[i], 0.f, 1.f) * 1023.f);
        out.cr[i] = static_cast<std::uint16_t>(std::clamp(frame.cr[i], 0.f, 1.f) * 1023.f);
    }
    return out;
}

// a = a·(1−phase) + b·phase in place, in 1/1024 steps (the compiler vectorises the integer loop;
// the double version allocated a frame and ran one pixel at a time).
void blendInto(Frame10& a, Frame10 const& b, double phase)
{
    auto const w = static_cast<std::uint32_t>(std::clamp(std::lround(phase * 1024.0), 0L, 1024L));
    auto const blend = [w](std::vector<std::uint16_t>& left, std::vector<std::uint16_t> const& right) {
        std::size_t const n = std::min(left.size(), right.size());
        std::uint16_t* l = left.data();
        std::uint16_t const* r = right.data();
        for (std::size_t i = 0; i < n; ++i)
        {
            l[i] = static_cast<std::uint16_t>((l[i] * (1024u - w) + r[i] * w) >> 10);
        }
    };
    blend(a.y, b.y);
    blend(a.cb, b.cb);
    blend(a.cr, b.cr);
}

// A UI preview `previewWidth` pixels wide, point-sampled from a v210 grain (no full-size 16-bit frame).
Frame10 sampleV210(std::uint8_t const* v210, int width, int height, int previewWidth)
{
    Frame10 small;
    int const outWidth = std::max(2, std::min(previewWidth, width) & ~1);
    small.allocate(outWidth, std::max(1, height * outWidth / width));
    auto const rowBytes = v210RowBytes(width);
    for (int oy = 0; oy < small.height; ++oy)
    {
        auto const* line = v210 + static_cast<std::size_t>(oy * height / small.height) * rowBytes;
        for (int ox = 0; ox < small.width; ++ox)
        {
            int const x = (ox * width / small.width) & ~1; // even: the pixel that carries the chroma
            std::uint32_t w[4];
            std::memcpy(w, line + static_cast<std::size_t>(x / 6) * 16u, sizeof(w));
            auto const s = [&](int word, int shift) { return static_cast<std::uint16_t>((w[word] >> shift) & 0x3ffu); };
            // Pixels 0, 2, 4 of a group: Y and the Cb/Cr pair they carry.
            static constexpr int kY[3][2] = {{0, 10}, {1, 20}, {3, 0}};
            static constexpr int kCb[3][2] = {{0, 0}, {1, 10}, {2, 20}};
            static constexpr int kCr[3][2] = {{0, 20}, {2, 0}, {3, 10}};
            int const k = (x % 6) / 2;
            small.y[static_cast<std::size_t>(oy * small.width + ox)] = s(kY[k][0], kY[k][1]);
            if ((ox & 1) == 0)
            {
                small.cb[static_cast<std::size_t>(oy * (small.width / 2) + ox / 2)] = s(kCb[k][0], kCb[k][1]);
                small.cr[static_cast<std::size_t>(oy * (small.width / 2) + ox / 2)] = s(kCr[k][0], kCr[k][1]);
            }
        }
    }
    return small;
}

constexpr int kPreviewQuality = 80;

// How many grains ahead a channel's flows are computed (SPECIFICATION.md §5.3).
constexpr int kFlowAheadGrains = 8;

// Decoded ahead (SPECIFICATION.md §5.1): the stored pictures of the next 2 grains, and how long the playout
// waits for one the worker is still decoding before it decodes the picture itself.
constexpr int kDecodeAheadGrains = 2;
// Around a transition (1.8.6): the first frames of the clip after OUT, and in a mix the incoming entry's frames that
// far ahead. A mix needs two new pictures a grain; frames decoded in the grains before it, when the worker has time
// for more than one, keep it ahead.
constexpr int kDecodeAheadNext = 6;
constexpr std::chrono::milliseconds kDecodeAheadWait{5};

std::string jsonString(std::string const& text)
{
    return "\"" + jsonEscape(text) + "\"";
}

// Empty id when there is no playlist `id`.
Catalog::Playlist findPlaylist(Catalog const& catalog, std::string const& id)
{
    for (auto& playlist : catalog.playlists())
    {
        if (playlist.id == id)
        {
            return playlist;
        }
    }
    return {};
}

// The first entry of the playlist whose clip still exists; -1 when there is none.
int firstPlayable(Catalog const& catalog, Catalog::Playlist const& playlist, ClipRef& clip)
{
    for (std::size_t i = 0; i < playlist.entries.size(); ++i)
    {
        clip = catalog.clip(playlist.entries[i].clipId);
        if (!clip.id.empty())
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Seconds from a camera's newest recorded frame to `nowNs`, counted from the start at most (a
// camera that has recorded nothing since the start, or nothing at all).
double lastFrameAge(std::uint64_t newestNs, std::uint64_t startedNs, std::uint64_t nowNs)
{
    auto const last = std::max(newestNs, startedNs);
    return nowNs > last ? static_cast<double>(nowNs - last) / 1e9 : 0.0;
}
} // namespace

// A grain's lookups of the pictures its channel's worker decoded ahead (frame 0: A, 1: B): the slot of a hit
// whose stored frame is the one the grain shows, else -1 (decode it here). For gpuRenderFromJpeg and v210At.
class AheadTaker final : public GpuAhead
{
public:
    explicit AheadTaker(DecodeAhead& ahead) : ahead_(ahead)
    {
    }
    std::array<AheadFrame, 2> frames{};
    std::array<std::uint64_t, 2> stored{};
    std::array<DecodeAhead::Take, 2> taken{};
    std::array<bool, 2> asked{};
    // The v210 slots (no GPU).
    std::array<std::vector<std::uint8_t>, DecodeAhead::kSlots> const* v210 = nullptr;

    int take(int frame) override
    {
        auto& result = taken[static_cast<std::size_t>(frame)];
        result = ahead_.take(frames[static_cast<std::size_t>(frame)], kDecodeAheadWait);
        asked[static_cast<std::size_t>(frame)] = true;
        if (result.result == DecodeAhead::Result::Hit && result.storedNs != stored[static_cast<std::size_t>(frame)])
        {
            // Another stored frame than the grain's (the ring changed in between): decoded here.
            ahead_.give(result.slot);
            result.result = DecodeAhead::Result::Miss;
            result.slot = -1;
        }
        return result.slot;
    }
    void give(int index) override
    {
        ahead_.give(index);
    }

private:
    DecodeAhead& ahead_;
};

Engine::CameraRuntime::CameraRuntime(std::string directory, std::uint64_t retentionNs, int segmentSeconds)
    : ring(std::move(directory), retentionNs, segmentSeconds)
{
}

Engine::Engine(Config config, std::map<std::string, std::string> settings, std::map<std::string, std::string> state)
    : config_(std::move(config))
    , settings_(std::move(state))
    , effective_(std::move(settings))
    , ids_(makeNmosIds(config_.nmosSeed))
    , startedNs_(taiNowNs())
{
    std::error_code ec;
    std::filesystem::create_directories(config_.stateDir, ec);
    if (ec)
    {
        throw StartupError(75, "cannot create state directory " + config_.stateDir + ": " + ec.message());
    }
    std::filesystem::create_directories(config_.storageDir, ec);
    if (ec)
    {
        throw StartupError(75, "cannot create storage directory " + config_.storageDir + ": " + ec.message());
    }
    auto const catalogPath = config_.stateDir + "/index.sqlite";
    auto const legacyCatalog = config_.storageDir + "/index.sqlite";
    if (!std::filesystem::exists(catalogPath, ec) && std::filesystem::exists(legacyCatalog, ec))
    {
        std::filesystem::copy_file(legacyCatalog, catalogPath, ec);
        if (!ec)
        {
            logInfo("catalog_migrated", {{"from", legacyCatalog}, {"to", catalogPath}});
        }
    }
    catalog_.open(catalogPath);
    continueSerial();
    removeStaleCameras();
    struct statvfs st{};
    if (statvfs(config_.storageDir.c_str(), &st) != 0)
    {
        throw StartupError(75, "cannot stat storage directory " + config_.storageDir);
    }
    auto freeBytes = static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_frsize);
    for (auto const& camera : config_.cameras)
    {
        freeBytes += segmentBytes(config_.storageDir + "/cam" + std::to_string(camera.index));
    }
    double required = 0;
    cameraBudgetBytes_ = 0;
    for (auto const& camera : config_.cameras)
    {
        int const fpsNum = camera.nativeFps > 0 ? camera.nativeFps : config_.format.rateNum;
        int const fpsDen = camera.nativeFps > 0 ? 1 : config_.format.rateDen;
        required += bytesPerHourEstimate(config_.format.width, config_.format.height, fpsNum, fpsDen, config_.jpegQuality) * camera.bufferHours;
        required += 48000.0 * 2.0 * 4.0 * 3600.0 * camera.bufferHours;
    }
    cameraBudgetBytes_ = static_cast<std::uint64_t>(required);
    if (static_cast<double>(freeBytes) < required)
    {
        throw StartupError(78, "buffer does not fit on " + config_.storageDir + ": need " + std::to_string(static_cast<std::uint64_t>(required)) +
                                    " bytes, have " + std::to_string(freeBytes));
    }
    storageBps_ = measureWriteThroughput(config_.storageDir, 1 << 20);
    double const mbps = storageBps_ * 8.0 / 1e6;
    if (mbps < config_.storageMinMbps)
    {
        logWarn("storage_slow", {{"mbps", std::to_string(mbps)}, {"min", std::to_string(config_.storageMinMbps)}});
    }
    libraryDir_ = config_.storageDir + "/library";
    std::filesystem::create_directories(libraryDir_, ec);
    // Upload bodies a crash (or an OOM kill) left behind.
    std::filesystem::remove_all(uploadDirectory(), ec);
    std::filesystem::create_directories(uploadDirectory(), ec);
    loadRoutes();
    gpu_ = cudaFlowAvailable();
    auto const period = framePeriodNs(config_.format.rateNum, config_.format.rateDen);
    if (config_.odirect)
    {
        logWarn("odirect_ignored", {{"reason", "the buffer writes buffered and drops finished segments from the page cache"}});
    }
    Frame10 black;
    black.allocate(config_.format.width, config_.format.height);
    black.fill(64, 512, 512);
    blackV210_.resize(v210Size(black.width, black.height));
    packV210(black, blackV210_.data(), 0);
    for (auto const& channel : config_.channelList)
    {
        ChannelRuntime runtime;
        runtime.scheduler.framePeriodNs = period;
        runtime.scheduler.camera = 1;
        runtime.scheduler.rampFrames = config_.rampFrames;
        runtime.shot.playOnFirstClick = config_.playOnFirstClick;
        runtime.last = black;
        runtime.lastV210 = blackV210_;
        // Its flows are computed ahead on a worker whose GPU stream is below the playout's and the recording's.
        int const index = static_cast<int>(channels_.size()) + 1;
        runtime.ahead = std::make_unique<FlowAhead>([this, index](FlowAhead::Pair const& pair) { return computeFlow(pair, index); },
            [] { gpuPrepareThread(GpuPriority::Flow); });
        // Its next pictures are decoded ahead on a worker whose GPU stream has the playout's priority (set up at
        // its first decode: a channel that never plays a stored picture has no GPU state for it).
        runtime.pictures = std::make_shared<AheadPictures>();
        if (cudaFlowAvailable())
        {
            runtime.pictures->gpu = gpuPicturesCreate(DecodeAhead::kSlots);
        }
        runtime.decodeAhead = std::make_unique<DecodeAhead>(
            [this, pictures = runtime.pictures](AheadFrame const& frame, int slot, std::uint64_t& storedNs) {
                return decodeAheadFrame(*pictures, frame, slot, storedNs);
            });
        for (char const* result : {"hit", "late", "miss"})
        {
            metrics_.inc("decode_ahead_frames_total", {{"channel", std::to_string(index)}, {"result", result}}, 0);
        }
        metrics_.inc("interpolation_fallbacks_total", {{"channel", std::to_string(index)}}, 0);
        metrics_.inc("tally_plays_total", {{"channel", std::to_string(index)}}, 0);
        metrics_.inc("transitions_total", {{"channel", std::to_string(index)}, {"kind", "cut"}}, 0);
        metrics_.inc("transitions_total", {{"channel", std::to_string(index)}, {"kind", "mix"}}, 0);
        metrics_.inc("read_ahead_bytes_total", {{"channel", std::to_string(index)}}, 0);
        (void)channel;
        channels_.push_back(std::move(runtime));
    }
    if (config_.tslEnable)
    {
        metrics_.inc("tsl_packets_total", {}, 0);
        metrics_.inc("tsl_errors_total", {}, 0);
    }
    if (effective_.empty())
    {
        std::istringstream lines(exportKeyValue(config_));
        std::string line;
        while (std::getline(lines, line))
        {
            auto const eq = line.find('=');
            if (eq != std::string::npos)
            {
                effective_[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
    }
    // The rings are set once by openBuffer, before any channel plays: the loader reads them without the engine lock.
    readAhead_ = std::make_unique<ReadAhead>([this](ReadAhead::Request const& request) {
        auto const* ring = ringOf(request.camera);
        auto const bytes = ring == nullptr ? 0 : ring->prefetch(request.fromNs, request.toNs);
        metrics_.inc("read_ahead_bytes_total", {{"channel", std::to_string(request.channel)}}, static_cast<double>(bytes));
        std::lock_guard lock{readAheadMutex_};
        if (readAheadObserver_)
        {
            readAheadObserver_(request, bytes);
        }
    });
    logInfo("replay_ready", {{"storage", config_.storageDir}, {"state", config_.stateDir}, {"inputs", std::to_string(config_.inputs)},
        {"channels", std::to_string(config_.channels)}});
}

Engine::~Engine() = default;

void Engine::setReadAheadObserver(std::function<void(ReadAhead::Request const&, std::uint64_t)> observer)
{
    std::lock_guard lock{readAheadMutex_};
    readAheadObserver_ = std::move(observer);
}

void Engine::openBuffer()
{
    auto const started = std::chrono::steady_clock::now();
    // Indexing reads every record header of the retained segments: no engine lock here.
    std::vector<CameraRuntime> cameras;
    cameras.reserve(config_.cameras.size());
    for (auto const& camera : config_.cameras)
    {
        auto const retention = static_cast<std::uint64_t>(camera.bufferHours * 3600.0 * 1e9);
        cameras.emplace_back(config_.storageDir + "/cam" + std::to_string(camera.index), retention, config_.segmentSeconds);
        cameras.back().phases.resize(static_cast<std::size_t>(camera.phases));
    }
    // A retention of 1 ns keeps only the segments a clip touches (and the open one).
    auto library = std::make_unique<FrameRing>(libraryDir_ + "/frames", 1, config_.segmentSeconds);
    std::lock_guard lock{mutex_};
    cameras_ = std::move(cameras);
    library_ = std::move(library);
    // Clips keep their frames: protect them again before old segments expire. A clip
    // consolidated out of a camera buffer released its range.
    for (auto const& clip : catalog_.clips())
    {
        auto* ring = holdsRange(clip) ? ringOf(clip.camera) : nullptr;
        if (ring != nullptr)
        {
            ring->protect(clip.inNs, clip.outNs);
        }
    }
    for (auto& camera : cameras_)
    {
        camera.ring.enforceRetention();
    }
    library_->enforceRetention();
    // A channel with an idle playlist starts with it (SPECIFICATION.md §7).
    for (int channel = 1; channel <= static_cast<int>(channels_.size()); ++channel)
    {
        startIdle(channel);
    }
    bufferReady_ = true;
    auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    logInfo("buffer_ready", {{"cameras", std::to_string(cameras_.size())}, {"seconds", std::to_string(seconds)}});
}

FrameRing const* Engine::ringOf(int camera) const
{
    if (camera == kLibraryCamera)
    {
        return library_.get();
    }
    if (camera < 1 || camera > static_cast<int>(cameras_.size()))
    {
        return nullptr;
    }
    return &cameras_[static_cast<std::size_t>(camera - 1)].ring;
}

FrameRing* Engine::ringOf(int camera)
{
    return const_cast<FrameRing*>(std::as_const(*this).ringOf(camera));
}

void Engine::continueSerial()
{
    // Ids end in "-<serial>" (clip-, upload-, group-, playlist-, audio-). Continue after the catalog's:
    // starting at 1 again after a restart replaced the clips that already had those ids.
    auto const serialOf = [](std::string const& id) -> std::uint64_t {
        auto const dash = id.rfind('-');
        if (dash == std::string::npos || dash + 1 >= id.size() || id.find_first_not_of("0123456789", dash + 1) != std::string::npos)
        {
            return 0;
        }
        return std::strtoull(id.c_str() + dash + 1, nullptr, 10);
    };
    for (auto const& clip : catalog_.clips())
    {
        clipSerial_ = std::max({clipSerial_, serialOf(clip.id) + 1, serialOf(clip.groupId) + 1});
    }
    for (auto const& playlist : catalog_.playlists())
    {
        clipSerial_ = std::max(clipSerial_, serialOf(playlist.id) + 1);
    }
    // Audio items too: a new upload must not take an existing item's id (and replace its file).
    for (auto const& item : catalog_.audioItems())
    {
        clipSerial_ = std::max(clipSerial_, serialOf(item.id) + 1);
    }
}

void Engine::removeStaleCameras()
{
    std::set<int> configured;
    for (auto const& camera : config_.cameras)
    {
        configured.insert(camera.index);
    }
    std::set<int> clipped;
    for (auto const& clip : catalog_.clips())
    {
        clipped.insert(clip.camera);
    }
    std::error_code ec;
    for (auto const& item : std::filesystem::directory_iterator(config_.storageDir, ec))
    {
        auto const name = item.path().filename().string();
        std::error_code typeError;
        if (!item.is_directory(typeError) || name.size() < 4 || name.rfind("cam", 0) != 0 ||
            name.find_first_not_of("0123456789", 3) != std::string::npos)
        {
            continue;
        }
        int const camera = std::atoi(name.c_str() + 3);
        if (configured.count(camera) != 0)
        {
            continue;
        }
        auto const bytes = segmentBytes(item.path().string());
        if (clipped.count(camera) != 0)
        {
            logWarn("removed_camera_kept", {{"camera", std::to_string(camera)}, {"bytes", std::to_string(bytes)}, {"reason", "clips use it"}});
            continue;
        }
        // Only the buffer's own files go; anything else keeps the directory.
        for (auto const& file : std::filesystem::directory_iterator(item.path(), ec))
        {
            auto const fileName = file.path().filename().string();
            if (fileName.size() > 4 && fileName.compare(fileName.size() - 4, 4, ".bin") == 0 &&
                (fileName.rfind("seg-", 0) == 0 || fileName.rfind("cam", 0) == 0))
            {
                std::error_code removeError;
                std::filesystem::remove(file.path(), removeError);
            }
        }
        std::error_code dirError;
        std::filesystem::remove(item.path(), dirError);
        logInfo("removed_camera_deleted", {{"camera", std::to_string(camera)}, {"bytes", std::to_string(bytes)}});
    }
}

double Engine::hfrFactor(int camera) const
{
    if (camera < 1 || camera > static_cast<int>(config_.cameras.size()))
    {
        return 1;
    }
    auto const& cfg = config_.cameras[static_cast<std::size_t>(camera - 1)];
    double const house = static_cast<double>(config_.format.rateNum) / static_cast<double>(config_.format.rateDen);
    if (cfg.nativeFps > 0)
    {
        return static_cast<double>(cfg.nativeFps) / house;
    }
    return static_cast<double>(std::max(1, cfg.phases));
}

std::uint64_t Engine::sourcePeriod(int camera) const
{
    double const factor = std::max(1.0, hfrFactor(camera));
    return static_cast<std::uint64_t>(static_cast<double>(framePeriodNs(config_.format.rateNum, config_.format.rateDen)) / factor);
}

void Engine::storeFrame(CameraRuntime& camera, std::uint64_t taiNs, Frame10 const& frame, std::vector<float> const& audio, std::vector<StoredFrame>& pending)
{
    StoredFrame stored;
    stored.taiNs = taiNs;
    stored.jpeg = gpuEncodeFrame10(frame, config_.jpegQuality);
    if (stored.jpeg.empty())
    {
        stored.jpeg = encodeJpeg422(frame, config_.jpegQuality);
    }
    stored.audio = audio;
    pending.push_back(std::move(stored));
}

void Engine::pushFrames(CameraRuntime& camera, std::vector<StoredFrame> frames)
{
    // The ring has its own lock: a slow disk stalls this camera only, not the channels.
    std::uint64_t recorded = 0;
    std::uint64_t bytes = 0;
    for (auto& frame : frames)
    {
        auto const size = frame.jpeg.size() + frame.audio.size() * sizeof(float);
        if (camera.ring.push(std::move(frame)))
        {
            ++recorded;
            bytes += size;
        }
    }
    std::lock_guard lock{mutex_};
    camera.recorded += recorded;
    camera.dropped += frames.size() - recorded;
    writtenBytes_ += bytes;
}

void Engine::flushHouse(CameraRuntime& camera, CameraConfig const& cfg, std::vector<StoredFrame>& pending)
{
    if (!camera.houseOpen)
    {
        return;
    }
    std::vector<PhaseGrain> grains;
    for (int phase = 0; phase < cfg.phases; ++phase)
    {
        PhaseGrain grain;
        grain.phase = phase;
        grain.present = camera.phases[static_cast<std::size_t>(phase)].present;
        grain.grainTaiNs = camera.phases[static_cast<std::size_t>(phase)].taiNs;
        grain.payload = phase;
        grains.push_back(grain);
    }
    auto const house = framePeriodNs(config_.format.rateNum, config_.format.rateDen);
    auto const interleaved = interleavePhases(grains, cfg.phases, static_cast<std::uint64_t>(house));
    camera.phaseMissing += static_cast<std::uint64_t>(interleaved.missing);
    for (auto const& item : interleaved.frames)
    {
        int const phase = std::clamp(item.phase, 0, cfg.phases - 1);
        auto const& slot = camera.phases[static_cast<std::size_t>(phase)];
        if (!slot.present && !item.repeated)
        {
            continue;
        }
        Frame10 const& picture = slot.present ? slot.frame : camera.phases[static_cast<std::size_t>(phase)].frame;
        if (picture.empty() && slot.present == false)
        {
            continue;
        }
        std::vector<float> audio;
        if (phase == 0)
        {
            audio = camera.audio;
        }
        storeFrame(camera, item.timeNs, picture.empty() ? slot.frame : picture, audio, pending);
    }
    for (auto& slot : camera.phases)
    {
        slot.present = false;
    }
    camera.audio.clear();
    camera.houseOpen = false;
}

void Engine::ingestV210(int camera, int phase, std::uint64_t taiNs, std::uint8_t const* packed, std::size_t bytes)
{
    if (packed == nullptr || bytes < v210Size(config_.format.width, config_.format.height))
    {
        return;
    }
    auto const rowBytes = static_cast<int>(v210RowBytes(config_.format.width));
    auto jpeg = gpuEncodeV210(packed, config_.format.width, config_.format.height, rowBytes, config_.jpegQuality);
    // The camera's preview tile (phase 1): from the grain on the GPU, or sampled on the CPU below.
    bool const drawTile = mosaic_ != nullptr && phase <= 1 && camera >= 1 && mosaicDue(taiNs);
    if (drawTile && !jpeg.empty())
    {
        mosaic_->drawGpuPicture(config_.channels + camera - 1);
    }
    if (jpeg.empty())
    {
        // CPU: a camera with one phase encodes here, without the engine lock, like the GPU path
        // (under the lock every camera waited for the others' encodes). HFR phases are
        // interleaved under the lock.
        bool single = false;
        {
            std::lock_guard lock{mutex_};
            single = camera >= 1 && camera <= static_cast<int>(cameras_.size()) && config_.cameras[static_cast<std::size_t>(camera - 1)].phases <= 1;
        }
        if (!single)
        {
            Frame10 frame;
            frame.allocate(config_.format.width, config_.format.height);
            unpackV210(packed, rowBytes, frame);
            ingestVideo(camera, phase, taiNs, std::move(frame));
            return;
        }
        jpeg = encodeJpegV210(packed, config_.format.width, config_.format.height, rowBytes, config_.jpegQuality);
        if (drawTile)
        {
            mosaic_->drawV210(config_.channels + camera - 1, packed, config_.format.width, config_.format.height);
        }
    }
    std::vector<StoredFrame> pending(1);
    pending[0].taiNs = taiNs;
    pending[0].jpeg = std::move(jpeg);
    CameraRuntime* runtime = nullptr;
    {
        std::lock_guard lock{mutex_};
        if (camera < 1 || camera > static_cast<int>(cameras_.size()) || !config_.cameras[static_cast<std::size_t>(camera - 1)].record)
        {
            return;
        }
        runtime = &cameras_[static_cast<std::size_t>(camera - 1)];
        // The camera's audio arrives with phase 1 (SPECIFICATION.md §4.2).
        if (phase <= 1)
        {
            pending[0].audio = std::move(runtime->audio);
            runtime->audio.clear();
        }
    }
    pushFrames(*runtime, std::move(pending));
}

void Engine::countDropped(int camera, std::uint64_t grains)
{
    std::lock_guard lock{mutex_};
    if (camera >= 1 && camera <= static_cast<int>(cameras_.size()))
    {
        cameras_[static_cast<std::size_t>(camera - 1)].dropped += grains;
    }
}

void Engine::ingestVideo(int camera, int phase, std::uint64_t taiNs, Frame10 frame)
{
    if (mosaic_ != nullptr && phase <= 1 && camera >= 1 && mosaicDue(taiNs))
    {
        mosaic_->drawFrame(config_.channels + camera - 1, frame);
    }
    std::vector<StoredFrame> pending;
    CameraRuntime* target = nullptr;
    {
        std::lock_guard lock{mutex_};
        if (camera < 1 || camera > static_cast<int>(cameras_.size()))
        {
            return;
        }
        auto& runtime = cameras_[static_cast<std::size_t>(camera - 1)];
        auto const& cfg = config_.cameras[static_cast<std::size_t>(camera - 1)];
        if (!cfg.record)
        {
            return;
        }
        target = &runtime;
        if (frame.width != config_.format.width || frame.height != config_.format.height)
        {
            Frame10 scaled;
            scaled.allocate(config_.format.width, config_.format.height);
            scaleFrame(frame, scaled, ScaleFilter::Bicubic);
            frame = std::move(scaled);
            runtime.scaled = true;
        }
        if (cfg.phases <= 1)
        {
            storeFrame(runtime, taiNs, frame, runtime.audio, pending);
            runtime.audio.clear();
        }
        else
        {
            auto const houseIndex = timestampToIndex(config_.format.rateNum, config_.format.rateDen, taiNs);
            if (runtime.houseOpen && houseIndex != runtime.openHouse)
            {
                flushHouse(runtime, cfg, pending);
            }
            runtime.houseOpen = true;
            runtime.openHouse = houseIndex;
            if (phase >= 1 && phase <= cfg.phases)
            {
                auto& slot = runtime.phases[static_cast<std::size_t>(phase - 1)];
                slot.present = true;
                slot.frame = std::move(frame);
                slot.taiNs = taiNs;
            }
            bool full = true;
            for (auto const& slot : runtime.phases)
            {
                full = full && slot.present;
            }
            if (full)
            {
                flushHouse(runtime, cfg, pending);
            }
        }
    }
    if (!pending.empty())
    {
        pushFrames(*target, std::move(pending));
    }
}

void Engine::ingestAudio(int camera, std::uint64_t taiNs, std::vector<float> audio, int channels)
{
    (void)taiNs;
    std::lock_guard lock{mutex_};
    if (camera < 1 || camera > static_cast<int>(cameras_.size()))
    {
        return;
    }
    auto& runtime = cameras_[static_cast<std::size_t>(camera - 1)];
    runtime.audioChannels = channels;
    runtime.audio = std::move(audio);
}

Frame10 Engine::frameAt(int camera, std::uint64_t taiNs, bool* found) const
{
    Frame10 frame;
    if (found != nullptr)
    {
        *found = false;
    }
    auto const* ring = ringOf(camera);
    if (ring == nullptr)
    {
        return frame;
    }
    auto const stored = ring->findNearest(taiNs);
    if (!stored)
    {
        return frame;
    }
    if (!decodeJpeg422(stored->jpeg.data(), stored->jpeg.size(), frame))
    {
        return frame;
    }
    if (found != nullptr)
    {
        *found = true;
    }
    return frame;
}

bool Engine::v210At(int camera, std::uint64_t taiNs, std::vector<std::uint8_t>& v210, bool* found, DecodedPicture& last, bool* repeated,
    AheadTaker* ahead) const
{
    *found = false;
    auto const* ring = ringOf(camera);
    if (ring == nullptr)
    {
        return false;
    }
    auto stored = ring->findNearest(taiNs);
    if (!stored)
    {
        return false;
    }
    *found = true;
    *repeated = !last.v210.empty() && stored->jpeg == last.jpeg;
    if (*repeated)
    {
        v210 = last.v210; // the previous grain's picture (1.8.3)
        return true;
    }
    v210.resize(v210Size(config_.format.width, config_.format.height));
    bool ok = false;
    if (ahead != nullptr && ahead->v210 != nullptr)
    {
        ahead->frames[0] = {camera, taiNs};
        ahead->stored[0] = stored->taiNs;
        if (int const slot = ahead->take(0); slot >= 0)
        {
            auto const& picture = (*ahead->v210)[static_cast<std::size_t>(slot)];
            ok = picture.size() == v210.size();
            if (ok)
            {
                std::memcpy(v210.data(), picture.data(), v210.size());
            }
            ahead->give(slot);
        }
    }
    if (!ok)
    {
        ok = decodeJpegToV210(stored->jpeg.data(), stored->jpeg.size(), config_.format.width, config_.format.height, 0, v210.data());
    }
    last.jpeg = ok ? std::move(stored->jpeg) : std::vector<std::uint8_t>{};
    last.key.clear();
    last.v210 = ok ? v210 : std::vector<std::uint8_t>{};
    return ok;
}

RenderedFrame Engine::render(int channel, std::uint64_t outputTaiNs)
{
    std::unique_lock lock{mutex_};
    RenderedFrame rendered;
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return rendered;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto const& cfg = config_.channelList[static_cast<std::size_t>(channel - 1)];
    auto const period = runtime.scheduler.framePeriodNs;
    if (cfg.lockToFirst && channel > 1)
    {
        runtime.scheduler.positionNs = channels_[0].scheduler.positionNs;
    }
    if (runtime.liveMode)
    {
        runtime.scheduler.positionNs = static_cast<std::int64_t>(outputTaiNs) - static_cast<std::int64_t>(config_.liveDelayFrames) * period;
        runtime.scheduler.speed = 1;
    }
    else if (runtime.scheduler.playing)
    {
        runtime.scheduler.step();
        // The outgoing entry of a transition plays on to its OUT and holds there.
        if (runtime.mix.active)
        {
            runtime.mix.scheduler.step();
            runtime.mix.scheduler.positionNs = std::min(runtime.mix.scheduler.positionNs, static_cast<std::int64_t>(runtime.mix.outNs));
        }
        maybeStartTransition(channel);
    }
    if (!runtime.clipId.empty() && runtime.hasOut && runtime.scheduler.positionNs >= static_cast<std::int64_t>(runtime.outNs) && runtime.scheduler.playing)
    {
        finishClip(channel);
    }
    readAhead(channel);
    int const camera = runtime.scheduler.camera;
    auto const position = static_cast<std::uint64_t>(std::max<std::int64_t>(0, runtime.scheduler.positionNs));
    // Live: the recorder stores frame M-2 just as grain M starts. Wait briefly for it instead of
    // showing the frame before (and skipping it on the next grain). A camera that is not
    // recording close to live is not waited for.
    if (auto const* ring = ringOf(camera); runtime.liveMode && ring != nullptr)
    {
        auto const newest = ring->newestNs();
        if (newest < position && newest + 3 * static_cast<std::uint64_t>(period) >= position)
        {
            lock.unlock();
            ring->waitFor(position, std::chrono::nanoseconds(period / 2));
            lock.lock();
        }
    }
    MotionMode motion = cfg.motion;
    if (motion == MotionMode::Interpolate && !gpuInterpolate())
    {
        motion = MotionMode::Blend;
    }
    auto const pick = pickSource(static_cast<std::int64_t>(position), 0, period, hfrFactor(camera), config_.hfrSnap, motion);
    auto const periodSrc = sourcePeriod(camera);
    std::uint64_t const taiA = static_cast<std::uint64_t>(pick.frameA) * periodSrc;
    std::uint64_t const taiB = static_cast<std::uint64_t>(pick.frameB) * periodSrc;
    planFlows(channel);
    planDecodes(channel, pick, motion);
    bool const wantFlow = motion == MotionMode::Interpolate && pick.kind == SourcePick::Kind::Interpolate;
    // A grain that interpolates: with its flow (a hit), or blended because the flow is not ready yet (a
    // fallback, logged at most every 10 s). Lock held.
    auto const countFlow = [&](bool ready) {
        Labels const labels{{"channel", std::to_string(channel)}};
        if (ready)
        {
            metrics_.inc("flow_cache_hits_total", labels);
            return;
        }
        metrics_.inc("interpolation_fallbacks_total", labels);
        ++runtime.fallbackLog.unlogged;
        if (auto const count = runtime.fallbackLog.toLog(taiNowNs()); count != 0)
        {
            logWarn("interpolation_fallback", {{"channel", std::to_string(channel)}, {"fallbacks", std::to_string(count)},
                                                  {"flow_ms", std::to_string(std::lround(runtime.ahead->flowSeconds() * 1000))}});
        }
    };
    bool exact = pick.kind == SourcePick::Kind::Exact;
    bool renderedOnGpu = false;
    // The output grain is this thread's last GPU picture (the preview tile is drawn from it there).
    bool devicePicture = false;
    // A video mix (SPECIFICATION.md §7, 1.6.0): the outgoing entry's and the incoming entry's nearest frames,
    // blended at the mix weight on the GPU (the blend path without a flow) or on the CPU.
    bool mixed = false;
    Frame10 mixPicture;
    if (runtime.mix.active && runtime.mix.done < runtime.mix.videoFrames && !runtime.black)
    {
        int const outCamera = runtime.mix.scheduler.camera;
        auto const* ringOut = ringOf(outCamera);
        auto const* ringIn = ringOf(camera);
        auto const nearest = [&](std::int64_t positionNs, int cam) {
            auto const p = pickSource(std::max<std::int64_t>(0, positionNs), 0, period, hfrFactor(cam), config_.hfrSnap, MotionMode::Repeat);
            return static_cast<std::uint64_t>(p.frameA) * sourcePeriod(cam);
        };
        auto const taiOut = nearest(runtime.mix.scheduler.positionNs, outCamera);
        auto const taiIn = nearest(runtime.scheduler.positionNs, camera);
        double const weight = mixWeight(runtime.mix.done, runtime.mix.videoFrames);
        bool const gpu = cudaFlowAvailable();
        if (ringOut != nullptr && ringIn != nullptr)
        {
            lock.unlock();
            auto const storedOut = ringOut->findNearest(taiOut);
            auto const storedIn = ringIn->findNearest(taiIn);
            GpuPicture gpuPicture;
            if (storedOut && storedIn && !storedOut->jpeg.empty() && !storedIn->jpeg.empty())
            {
                // Both entries' frames from the decode worker when it has them (1.8.6).
                AheadTaker taker(*runtime.decodeAhead);
                taker.pictures = runtime.pictures->gpu.get();
                taker.frames = {AheadFrame{outCamera, taiOut}, AheadFrame{camera, taiIn}};
                taker.stored = {storedOut->taiNs, storedIn->taiNs};
                bool const onGpu = gpu &&
                                   gpuRenderFromJpeg(storedOut->jpeg.data(), storedOut->jpeg.size(), std::to_string(outCamera) + ":" + std::to_string(storedOut->taiNs),
                                       storedIn->jpeg.data(), storedIn->jpeg.size(), std::to_string(camera) + ":" + std::to_string(storedIn->taiNs),
                                       static_cast<float>(weight), nullptr, config_.format.width, config_.format.height, gpuPicture,
                                       taker.pictures != nullptr ? &taker : nullptr);
                countDecodeAhead(channel, taker);
                if (onGpu)
                {
                    rendered.v210 = std::move(gpuPicture.v210);
                    renderedOnGpu = true;
                    devicePicture = true;
                    mixed = true;
                }
                else
                {
                    Frame10 frameIn;
                    if (decodeJpeg422(storedOut->jpeg.data(), storedOut->jpeg.size(), mixPicture) &&
                        decodeJpeg422(storedIn->jpeg.data(), storedIn->jpeg.size(), frameIn) && mixPicture.width == frameIn.width &&
                        mixPicture.height == frameIn.height)
                    {
                        blendInto(mixPicture, frameIn, weight);
                        mixed = true;
                    }
                }
            }
            lock.lock();
            if (renderedOnGpu)
            {
                runtime.lastV210 = rendered.v210;
            }
        }
    }
    if (auto const* ring = ringOf(camera); !mixed && !runtime.black && cudaFlowAvailable() && ring != nullptr)
    {
        // Read, decode, interpolate and download without the engine lock, so channels and
        // the recorder do not wait for each other's disk or GPU work.
        bool const wantB = pick.kind != SourcePick::Kind::Exact && pick.kind != SourcePick::Kind::Repeat;
        auto& ahead = *runtime.ahead;
        lock.unlock();
        auto storedA = ring->findNearest(taiA);
        auto const storedB = wantB ? ring->findNearest(taiB) : std::nullopt;
        bool const second = storedB && storedA && storedB->taiNs != storedA->taiNs;
        GpuPicture gpuPicture;
        // Held until the picture is done.
        auto const flow = wantFlow && second ? ahead.find(flowKey(camera, taiA, taiB)).gpu : nullptr;
        bool onGpu = false;
        if (storedA && !storedA->jpeg.empty())
        {
            // The previous grain's picture keeps its key, so the decoder does not decode it again (1.8.3).
            auto& last = runtime.decoded;
            bool const repeated = !last.key.empty() && storedA->jpeg == last.jpeg;
            auto const keyA = repeated ? last.key : std::to_string(camera) + ":" + std::to_string(storedA->taiNs);
            auto const keyB = second ? std::to_string(camera) + ":" + std::to_string(storedB->taiNs) : std::string{};
            // A frame the GPU does not hold from the last grain comes from the decode worker when it has it.
            AheadTaker taker(*runtime.decodeAhead);
            taker.pictures = runtime.pictures->gpu.get();
            taker.frames = {AheadFrame{camera, taiA}, AheadFrame{camera, taiB}};
            taker.stored = {storedA->taiNs, second ? storedB->taiNs : 0};
            onGpu = gpuRenderFromJpeg(storedA->jpeg.data(), storedA->jpeg.size(), keyA, second ? storedB->jpeg.data() : nullptr, second ? storedB->jpeg.size() : 0,
                keyB, static_cast<float>(pick.phase), flow.get(), config_.format.width, config_.format.height, gpuPicture,
                taker.pictures != nullptr ? &taker : nullptr);
            countDecodeAhead(channel, taker);
            if (onGpu && repeated)
            {
                metrics_.inc("playout_repeated_pictures_total", {{"channel", std::to_string(channel)}});
            }
            else if (onGpu)
            {
                last.jpeg = std::move(storedA->jpeg);
                last.key = keyA;
                last.v210.clear();
            }
        }
        lock.lock();
        if (onGpu)
        {
            rendered.v210 = std::move(gpuPicture.v210);
            runtime.lastV210 = rendered.v210;
            renderedOnGpu = true;
            devicePicture = true;
            if (wantFlow)
            {
                countFlow(!second || flow != nullptr);
            }
        }
    }
    bool foundA = false;
    bool foundB = false;
    Frame10 frameA;
    Frame10 frameB;
    Frame10 picture;
    // CPU: an exact frame is decoded straight into the output grain; only blends and
    // interpolation need the 16-bit frames, and only they need frame B.
    bool directV210 = false;
    if (renderedOnGpu || mixed)
    {
        foundA = true;
    }
    else
    {
        bool const needB = pick.kind != SourcePick::Kind::Exact && pick.kind != SourcePick::Kind::Repeat && std::fabs(pick.phase) >= 1e-6;
        bool const black = runtime.black;
        lock.unlock();
        bool decoded = false;
        bool repeated = false;
        if (!needB && !black)
        {
            AheadTaker taker(*runtime.decodeAhead);
            taker.v210 = runtime.pictures->gpu == nullptr ? &runtime.pictures->v210 : nullptr;
            decoded = v210At(camera, taiA, rendered.v210, &foundA, runtime.decoded, &repeated, &taker);
            countDecodeAhead(channel, taker);
            directV210 = decoded;
        }
        // A black channel shows no frame; a frame not in the house format takes the 16-bit path.
        if (!decoded && !black && (needB || foundA))
        {
            frameA = frameAt(camera, taiA, &foundA);
            if (needB)
            {
                frameB = frameAt(camera, taiB, &foundB);
            }
        }
        lock.lock();
        if (repeated)
        {
            metrics_.inc("playout_repeated_pictures_total", {{"channel", std::to_string(channel)}});
        }
    }
    // Black is one cached grain: filling, packing and thumbnailing a black frame for every
    // grain made idle channels cost about as much as playing ones.
    bool blackFrame = false;
    if (runtime.black || (!foundA && !foundB))
    {
        if (!runtime.black && cfg.idle == IdleSource::Last && !runtime.lastV210.empty())
        {
            rendered.v210 = runtime.lastV210;
            renderedOnGpu = true;
        }
        else if (!runtime.black && cfg.idle == IdleSource::Last && !runtime.last.empty())
        {
            picture = runtime.last;
        }
        else if (!runtime.black && cfg.idle == IdleSource::E2e)
        {
            lock.unlock();
            picture = frameAt(camera, outputTaiNs, &foundA);
            lock.lock();
            blackFrame = !foundA;
        }
        else
        {
            blackFrame = true;
        }
        rendered.black = !renderedOnGpu && (runtime.black || (!foundA && !foundB));
    }
    else if (mixed)
    {
        exact = false;
        if (!renderedOnGpu)
        {
            picture = std::move(mixPicture);
        }
    }
    else if (renderedOnGpu || directV210)
    {
        exact = pick.kind == SourcePick::Kind::Exact || pick.kind == SourcePick::Kind::Repeat || directV210;
    }
    else if (!foundB || pick.kind == SourcePick::Kind::Exact || pick.kind == SourcePick::Kind::Repeat || std::fabs(pick.phase) < 1e-6)
    {
        picture = foundA ? std::move(frameA) : std::move(frameB);
        exact = true;
    }
    else if (pick.kind == SourcePick::Kind::Blend || motion == MotionMode::Blend)
    {
        // The frames are local copies: blend without the engine lock (the recorders wait for it).
        lock.unlock();
        blendInto(frameA, frameB, pick.phase);
        lock.lock();
        picture = std::move(frameA);
    }
    else
    {
        // Interpolation on the CPU (REPLAY_ALLOW_CPU_INTERP without a GPU) with the flow computed ahead; blended
        // while it is not ready.
        auto const flow = runtime.ahead->find(flowKey(camera, taiA, taiB)).cpu;
        auto const op = operatingPoint(config_.preset);
        lock.unlock();
        if (flow)
        {
            picture = fromFloat(interpolateFrames(toFloat(frameA), toFloat(frameB), flow->forward, flow->backward, static_cast<float>(pick.phase), op));
        }
        else
        {
            blendInto(frameA, frameB, pick.phase);
            picture = std::move(frameA);
        }
        lock.lock();
        countFlow(flow != nullptr);
    }
    if (blackFrame)
    {
        rendered.v210 = blackV210_;
        runtime.lastV210 = blackV210_;
    }
    else if (directV210)
    {
        runtime.lastV210 = rendered.v210;
    }
    else if (!renderedOnGpu)
    {
        rendered.v210.resize(v210Size(picture.width, picture.height));
        packV210(picture, rendered.v210.data(), 0);
        runtime.lastV210 = rendered.v210;
        runtime.last = std::move(picture);
    }
    rendered.positionNs = position;
    rendered.speed = runtime.scheduler.speed;
    rendered.motion = mixed ? "mix" : motionName(exact && motion == MotionMode::Interpolate ? MotionMode::Repeat : motion);
    rendered.exact = exact;
    rendered.camera = camera;
    auto const tcTai = cfg.timecode == TcMode::Source ? position : outputTaiNs;
    auto const tc = timecodeFromTai(tcTai, config_.format.rateNum, config_.format.rateDen, false);
    rendered.timecode = tc.format();
    runtime.timecode = rendered.timecode;
    AncPacket packet;
    packet.tc = tc;
    rendered.anc = encodeAncGrain(packet);
    if (config_.format.interlaced)
    {
        // One grain per field at the field rate, F bits 0b10 and 0b11, the same timecode in both.
        packet.interlaced = true;
        packet.thirtyFrame = config_.format.rateNum == 30000 && config_.format.rateDen == 1001;
        rendered.anc = encodeAncGrain(packet);
        packet.tc.field = 1;
        packet.line = 572;
        rendered.ancSecond = encodeAncGrain(packet);
    }

    int const samples = audioSamplesForFrame(timestampToIndex(config_.format.rateNum, config_.format.rateDen, outputTaiNs), config_.format.rateNum,
        config_.format.rateDen, 48000);
    int const audioCamera = cfg.atmosCamera > 0 ? cfg.atmosCamera : camera;
    double const speed = std::fabs(runtime.scheduler.speed);
    // A channel that stands still (paused, cued, at a clip's end, black, or at speed 0) is silent: its position
    // does not move, and its frame's samples repeated every grain are a buzz. Slow motion in `mute` is silent too.
    bool const still = !runtime.liveMode && (!runtime.scheduler.playing || runtime.black || speed <= 0.01);
    // A playlist entry whose own audio is off is silent too (its aux track plays, 1.6.0).
    bool const silent = still || (!runtime.liveMode && speed < 0.999 && cfg.audio == AudioMode::Mute) || (!runtime.liveMode && !runtime.ownAudio);
    std::vector<float> pcm(static_cast<std::size_t>(samples) * 2, 0.f);
    if (auto const* ring = ringOf(audioCamera))
    {
        // Audio is stored with the frames on house times (phase 1 of an HFR camera).
        auto const house = static_cast<std::uint64_t>(std::max<std::int64_t>(1, period));
        auto at = (position + house / 2) / house * house;
        // A channel that just stopped fades out over the audio after the grain it played last (no splice, no click),
        // not over its frame again.
        if (still && runtime.fadeFramesLeft < 2 && runtime.audioNs != 0)
        {
            at = runtime.audioNs + house;
        }
        runtime.audioNs = at;
        lock.unlock();
        auto audio = ring->findNearestAudio(at);
        lock.lock();
        if (!audio.empty())
        {
            audio.resize(pcm.size(), 0.f);
            pcm = std::move(audio);
        }
    }
    if (!silent && !runtime.liveMode && speed < 0.999)
    {
        if (cfg.audio == AudioMode::Stretch)
        {
            pcm = timeStretch(pcm, 2, speed).samples;
            pcm.resize(static_cast<std::size_t>(samples) * 2, 0.f);
        }
        else if (cfg.audio == AudioMode::Follow)
        {
            pcm = resampleLinear(pcm, 2, speed);
            pcm.resize(static_cast<std::size_t>(samples) * 2, 0.f);
        }
    }
    // Two-grain fades: out into silence, in when the channel plays (or goes live) again.
    if (silent)
    {
        if (runtime.fadeFramesLeft < 2)
        {
            applyMuteFade(pcm, runtime.fadeFramesLeft, 2);
            ++runtime.fadeFramesLeft;
        }
        else
        {
            std::fill(pcm.begin(), pcm.end(), 0.f);
        }
    }
    else if (runtime.fadeFramesLeft > 0)
    {
        applyFadeIn(pcm, 2 - runtime.fadeFramesLeft, 2);
        --runtime.fadeFramesLeft;
    }
    // The audio crossfade of a transition (equal power): the outgoing entry's audio at its own position, faded out
    // under the incoming's. A channel that stops ends it with a one-grain fade.
    bool outgoingAudible = false;
    if (runtime.mix.active && runtime.mix.audioSamples < runtime.mix.audioTotal)
    {
        auto& mix = runtime.mix;
        double const outSpeed = std::fabs(mix.scheduler.speed);
        bool const outMuted = !mix.ownAudio || (outSpeed < 0.999 && mix.audio == AudioMode::Mute);
        std::vector<float> outPcm(pcm.size(), 0.f);
        int const outAudioCamera = cfg.atmosCamera > 0 ? cfg.atmosCamera : mix.scheduler.camera;
        if (auto const* ring = ringOf(outAudioCamera); ring != nullptr && !outMuted)
        {
            auto const house = static_cast<std::uint64_t>(std::max<std::int64_t>(1, period));
            auto const outPosition = static_cast<std::uint64_t>(std::max<std::int64_t>(0, mix.scheduler.positionNs));
            auto at = (outPosition + house / 2) / house * house;
            // Held at its OUT: the audio after it, not the same frame again.
            if (mix.audioNs != 0 && at <= mix.audioNs)
            {
                at = mix.audioNs + house;
            }
            mix.audioNs = at;
            lock.unlock();
            auto audio = ring->findNearestAudio(at);
            lock.lock();
            if (!audio.empty())
            {
                audio.resize(outPcm.size(), 0.f);
                outPcm = std::move(audio);
            }
            if (outSpeed < 0.999 && mix.audio == AudioMode::Stretch)
            {
                outPcm = timeStretch(outPcm, 2, outSpeed).samples;
                outPcm.resize(pcm.size(), 0.f);
            }
            else if (outSpeed < 0.999 && mix.audio == AudioMode::Follow)
            {
                outPcm = resampleLinear(outPcm, 2, outSpeed);
                outPcm.resize(pcm.size(), 0.f);
            }
        }
        if (still)
        {
            applyMuteFade(outPcm, 0, 1);
        }
        std::size_t const frames = pcm.size() / 2;
        for (std::size_t i = 0; i < frames; ++i)
        {
            auto const gains = still ? CrossfadeGains{1.f, 1.f} : crossfadeGains(mix.audioSamples + static_cast<std::int64_t>(i), mix.audioTotal);
            pcm[2 * i] = pcm[2 * i] * gains.in + outPcm[2 * i] * gains.out;
            pcm[2 * i + 1] = pcm[2 * i + 1] * gains.in + outPcm[2 * i + 1] * gains.out;
        }
        mix.audioSamples = still ? mix.audioTotal : mix.audioSamples + static_cast<std::int64_t>(frames);
        outgoingAudible = !outMuted;
    }
    mixAux(channel, lock, pcm, still, !silent || outgoingAudible, outputTaiNs);
    if (runtime.mix.active && runtime.scheduler.playing)
    {
        ++runtime.mix.done;
        if (runtime.mix.done >= std::max(runtime.mix.videoFrames, runtime.mix.audioFrames) && runtime.mix.audioSamples >= runtime.mix.audioTotal)
        {
            runtime.mix.active = false;
        }
    }
    rendered.audio = std::move(pcm);
    metrics_.set("channel_speed", {{"channel", std::to_string(channel)}}, runtime.scheduler.speed);
    metrics_.set("channel_state", {{"channel", std::to_string(channel)}}, runtime.liveMode ? 0 : runtime.scheduler.playing ? 1 : 2);
    if (mosaic_ != nullptr && mosaicDue(outputTaiNs) && rendered.v210.size() >= v210Size(config_.format.width, config_.format.height))
    {
        // The channel's preview tile, without the engine lock (the tile is this channel's alone).
        lock.unlock();
        if (!devicePicture || !mosaic_->drawGpuPicture(channel - 1))
        {
            mosaic_->drawV210(channel - 1, rendered.v210.data(), config_.format.width, config_.format.height);
        }
    }
    return rendered;
}

void Engine::planFlows()
{
    std::lock_guard lock{mutex_};
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        planFlows(static_cast<int>(i) + 1);
    }
}

void Engine::planFlows(int channel)
{
    // None while the channel is live, black or does not interpolate, and none whose frame B is not recorded
    // yet (close to live). A locked channel moves with channel 1.
    auto const& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto const& cfg = config_.channelList[static_cast<std::size_t>(channel - 1)];
    int const camera = runtime.scheduler.camera;
    auto const* ring = ringOf(camera);
    std::vector<FlowAhead::Pair> plan;
    if (cfg.motion == MotionMode::Interpolate && gpuInterpolate() && !runtime.liveMode && !runtime.black && ring != nullptr)
    {
        auto const periodSrc = sourcePeriod(camera);
        auto const newest = ring->newestNs();
        auto const now = FlowAhead::Clock::now();
        auto const& moving = cfg.lockToFirst && channel > 1 ? channels_[0].scheduler : runtime.scheduler;
        for (auto const& pair : pairsAhead(moving, hfrFactor(camera), config_.hfrSnap, kFlowAheadGrains))
        {
            auto const a = static_cast<std::uint64_t>(pair.frameA) * periodSrc;
            auto const b = static_cast<std::uint64_t>(pair.frameB) * periodSrc;
            if (b <= newest)
            {
                plan.push_back({flowKey(camera, a, b), camera, a, b, now + std::chrono::nanoseconds(pair.last * moving.framePeriodNs)});
            }
        }
    }
    runtime.ahead->plan(std::move(plan));
}

AheadFlow Engine::computeFlow(FlowAhead::Pair const& pair, int channel)
{
    // On the channel's worker, without the engine lock: the rings have their own.
    AheadFlow flow;
    auto const op = operatingPoint(config_.preset);
    auto const* ring = ringOf(pair.camera);
    if (ring == nullptr)
    {
        return flow;
    }
    if (cudaFlowAvailable())
    {
        auto const a = ring->findNearest(pair.taiA);
        auto const b = ring->findNearest(pair.taiB);
        if (!a || !b || a->taiNs == b->taiNs)
        {
            return flow;
        }
        double gpuSeconds = 0;
        flow.gpu = gpuComputeFlow(a->jpeg.data(), a->jpeg.size(), std::to_string(pair.camera) + ":" + std::to_string(a->taiNs), b->jpeg.data(), b->jpeg.size(),
            std::to_string(pair.camera) + ":" + std::to_string(b->taiNs), op, config_.format.width, config_.format.height, gpuSeconds);
        if (flow.gpu)
        {
            metrics_.observe("frame_gpu_seconds", {{"channel", std::to_string(channel)}, {"stage", "flow"}}, gpuSeconds);
        }
        return flow;
    }
    bool foundA = false;
    bool foundB = false;
    auto const a = frameAt(pair.camera, pair.taiA, &foundA);
    auto const b = frameAt(pair.camera, pair.taiB, &foundB);
    if (foundA && foundB)
    {
        flow.cpu = std::make_shared<FlowPair const>(computeFlowPair(grayFromLuma(toFloat(a)), grayFromLuma(toFloat(b)), op));
    }
    return flow;
}

void Engine::planDecodes(int channel, SourcePick const& current, MotionMode motion)
{
    // None while black, and none not recorded yet (close to live: those are decoded on the output thread). A
    // locked channel moves at channel 1's speed. Lock held, no reads, no allocation.
    auto const& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto const& cfg = config_.channelList[static_cast<std::size_t>(channel - 1)];
    // The GPU decodes both frames of a grain between two, and both entries of a mix; the CPU path decodes only
    // single frames ahead (its mix and blend decode 16-bit frames).
    bool const gpu = runtime.pictures->gpu != nullptr;
    auto const source = [&](Scheduler const& scheduler, SourcePick const& pick, MotionMode mode) {
        AheadSource ahead;
        ahead.scheduler = scheduler;
        ahead.current = pick;
        ahead.camera = scheduler.camera;
        ahead.periodNs = sourcePeriod(scheduler.camera);
        ahead.hfrFactor = hfrFactor(scheduler.camera);
        ahead.hfrSnap = config_.hfrSnap;
        ahead.motion = mode;
        ahead.withB = gpu;
        return ahead;
    };
    std::array<AheadSource, 2> sources{};
    int count = 0;
    std::array<AheadFrame, kDecodeAheadNext> next{};
    int nextCount = 0;
    if (runtime.black)
    {
    }
    else if (runtime.mix.active && runtime.mix.done < runtime.mix.videoFrames)
    {
        // A mix (1.8.6): the incoming and the outgoing entry's nearest frames (render's mix), the outgoing one
        // holding at its OUT.
        if (gpu)
        {
            auto const nearest = [&](Scheduler const& scheduler) {
                return pickSource(std::max<std::int64_t>(0, scheduler.positionNs), 0, scheduler.framePeriodNs, hfrFactor(scheduler.camera), config_.hfrSnap,
                    MotionMode::Repeat);
            };
            auto incoming = source(runtime.scheduler, nearest(runtime.scheduler), MotionMode::Repeat);
            incoming.grains = kDecodeAheadNext;
            sources[static_cast<std::size_t>(count++)] = incoming;
            auto outgoing = source(runtime.mix.scheduler, nearest(runtime.mix.scheduler), MotionMode::Repeat);
            outgoing.untilNs = runtime.mix.outNs;
            sources[static_cast<std::size_t>(count++)] = outgoing;
        }
    }
    else
    {
        Scheduler moving = cfg.lockToFirst && channel > 1 ? channels_[0].scheduler : runtime.scheduler;
        moving.positionNs = runtime.scheduler.positionNs;
        moving.camera = runtime.scheduler.camera;
        if (runtime.liveMode)
        {
            moving.playing = true;
            moving.speed = 1;
            moving.targetSpeed = 1;
            moving.rampLeft = 0;
        }
        auto main = source(moving, current, motion);
        // Before OUT (1.8.6): the next clip's first kDecodeAheadNext frames, from twice that many grains before the
        // longest transition starts (a mix shows the incoming IN on its first grain). The frames after OUT are not
        // shown.
        double const speed = runtime.scheduler.speed;
        auto const position = runtime.scheduler.positionNs;
        auto const transition = runtime.playlistId.empty() ? 0 : runtime.planMaxTransition;
        auto const window = static_cast<double>(transition + 2 * kDecodeAheadNext) * speed * static_cast<double>(runtime.scheduler.framePeriodNs);
        if (runtime.nextStart.valid && !runtime.clipId.empty() && runtime.hasOut && runtime.scheduler.playing && speed >= kMinClipSpeed &&
            position < static_cast<std::int64_t>(runtime.outNs) && static_cast<double>(static_cast<std::int64_t>(runtime.outNs) - position) <= window)
        {
            main.untilNs = runtime.outNs;
            // The next clip from its IN at its speed, as cueClip and the steps after it put it.
            Scheduler start;
            start.framePeriodNs = runtime.scheduler.framePeriodNs;
            start.camera = runtime.nextStart.camera;
            start.positionNs = static_cast<std::int64_t>(runtime.nextStart.inNs);
            start.setTargetSpeed(runtime.nextStart.speed, 0);
            auto const periodNext = sourcePeriod(start.camera);
            for (int grain = 0; grain < 2 * kDecodeAheadNext && nextCount < kDecodeAheadNext; ++grain)
            {
                auto const pick = pickSource(std::max<std::int64_t>(0, start.positionNs), 0, start.framePeriodNs, hfrFactor(start.camera), config_.hfrSnap,
                    MotionMode::Repeat);
                AheadFrame const frame{start.camera, static_cast<std::uint64_t>(std::max<std::int64_t>(0, pick.frameA)) * periodNext};
                if (nextCount == 0 || !(next[static_cast<std::size_t>(nextCount - 1)] == frame))
                {
                    next[static_cast<std::size_t>(nextCount++)] = frame;
                }
                start.playing = true;
                start.step();
            }
        }
        sources[static_cast<std::size_t>(count++)] = main;
    }
    auto plan = planAhead(sources.data(), count, next.data(), nextCount, kDecodeAheadGrains);
    // Only recorded frames.
    int kept = 0;
    for (int i = 0; i < plan.count; ++i)
    {
        auto const& frame = plan.frames[static_cast<std::size_t>(i)];
        auto const* ring = ringOf(frame.camera);
        if (ring != nullptr && frame.taiNs <= ring->newestNs())
        {
            plan.frames[static_cast<std::size_t>(kept++)] = frame;
        }
    }
    runtime.decodeAhead->plan(plan.frames.data(), kept, plan.keep.data(), plan.keepCount);
}

bool Engine::decodeAheadFrame(AheadPictures& pictures, AheadFrame const& frame, int slot, std::uint64_t& storedNs) const
{
    // On the channel's decode worker, without the engine lock: the rings have their own.
    auto const* ring = ringOf(frame.camera);
    if (ring == nullptr)
    {
        return false;
    }
    auto const stored = ring->findNearest(frame.taiNs);
    if (!stored || stored->jpeg.empty())
    {
        return false;
    }
    storedNs = stored->taiNs;
    // The same picture as the frame before it (a repeated source frame of an upload, a still camera): the
    // playout shows it without decoding (1.8.3), so it is not decoded here either.
    if (stored->jpeg == pictures.lastJpeg)
    {
        return false;
    }
    pictures.lastJpeg.assign(stored->jpeg.begin(), stored->jpeg.end());
    int const width = config_.format.width;
    int const height = config_.format.height;
    if (pictures.gpu)
    {
        gpuPrepareThread(GpuPriority::Playout);
        return gpuDecodePicture(*pictures.gpu, slot, stored->jpeg.data(), stored->jpeg.size(), width, height);
    }
    auto& v210 = pictures.v210[static_cast<std::size_t>(slot)];
    v210.resize(v210Size(width, height));
    return decodeJpegToV210(stored->jpeg.data(), stored->jpeg.size(), width, height, 0, v210.data());
}

void Engine::countDecodeAhead(int channel, AheadTaker const& taker)
{
    for (std::size_t i = 0; i < taker.asked.size(); ++i)
    {
        if (!taker.asked[i])
        {
            continue;
        }
        auto const& taken = taker.taken[i];
        auto const result = taken.result == DecodeAhead::Result::Hit ? "hit" : taken.result == DecodeAhead::Result::Late ? "late" : "miss";
        metrics_.inc("decode_ahead_frames_total", {{"channel", std::to_string(channel)}, {"result", result}});
        if (taken.planned)
        {
            metrics_.observe("decode_ahead_wait_seconds", {{"channel", std::to_string(channel)}}, std::chrono::duration<double>(taken.waited).count());
        }
    }
}

Engine::ClipEnd Engine::planClipEnd(ChannelRuntime const& runtime, Catalog::Playlist const& list) const
{
    ClipEnd plan;
    // The idle playlist plays every entry on and starts again after the last (SPECIFICATION.md §7).
    if (runtime.idle && !list.id.empty())
    {
        std::vector<bool> present;
        for (auto const& item : list.entries)
        {
            present.push_back(!catalog_.clip(item.clipId).id.empty());
        }
        int const next = idleNext(present, runtime.playlistIndex);
        if (next >= 0)
        {
            plan.action = EndAction::Next;
            plan.idleNext = true;
            plan.next = next;
            plan.nextClip = catalog_.clip(list.entries[static_cast<std::size_t>(next)].clipId);
            plan.autoAdvance = true;
            return plan;
        }
    }
    // A playlist entry has its own end action and auto-advance.
    PlaylistEntry const* entry = nullptr;
    if (runtime.playlistIndex >= 0 && runtime.playlistIndex < static_cast<int>(list.entries.size()))
    {
        entry = &list.entries[static_cast<std::size_t>(runtime.playlistIndex)];
    }
    auto action = entry != nullptr ? entry->end : EndAction::Freeze;
    if (entry == nullptr)
    {
        auto const clip = catalog_.clip(runtime.clipId);
        action = clip.id.empty() ? runtime.shot.end : clip.end;
    }
    // A looped playlist plays over and over: its last playable entry's freeze or black wraps to the
    // first entry and plays on. An entry's own loop and return-to-live still win (SPECIFICATION §7).
    bool wraps = false;
    if (entry != nullptr && list.loop && (action == EndAction::Freeze || action == EndAction::Black))
    {
        int later = playlistNext(list.entries, runtime.playlistIndex);
        while (later >= 0 && catalog_.clip(list.entries[static_cast<std::size_t>(later)].clipId).id.empty())
        {
            later = playlistNext(list.entries, later);
        }
        if (later < 0)
        {
            action = EndAction::Next;
            wraps = true;
        }
    }
    plan.action = action;
    if (action == EndAction::Next && entry != nullptr)
    {
        // The next entry whose clip still exists; auto-advance plays it, otherwise it is cued. A looped
        // playlist starts again at its first.
        plan.autoAdvance = entry->autoAdvance || wraps;
        int next = playlistNext(list.entries, runtime.playlistIndex);
        ClipRef nextClip;
        while (next >= 0 && (nextClip = catalog_.clip(list.entries[static_cast<std::size_t>(next)].clipId)).id.empty())
        {
            next = playlistNext(list.entries, next);
        }
        if (next < 0 && list.loop)
        {
            next = firstPlayable(catalog_, list, nextClip);
        }
        if (next >= 0)
        {
            plan.next = next;
            plan.nextClip = nextClip;
        }
    }
    return plan;
}

void Engine::finishClip(int channel)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto const list = runtime.playlistId.empty() ? Catalog::Playlist{} : findPlaylist(catalog_, runtime.playlistId);
    auto const plan = planClipEnd(runtime, list);
    if (plan.idleNext)
    {
        runtime.playlistIndex = plan.next;
        cueClip(channel, plan.nextClip, &list.entries[static_cast<std::size_t>(plan.next)]);
        runtime.idle = true;
        runtime.scheduler.playing = true;
        runtime.shot.state = ShotState::Playing;
        return;
    }
    runtime.shot.end = plan.action;
    runtime.shot.finished();
    // The clip or the playlist ends here: the idle playlist plays, else the end action (freeze or black) stays.
    auto const ended = [&](bool black) {
        if (startIdle(channel))
        {
            return;
        }
        runtime.scheduler.playing = false;
        if (black)
        {
            runtime.black = true;
        }
        else
        {
            runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.outNs);
        }
    };
    if (plan.action == EndAction::Loop)
    {
        runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.inNs);
        runtime.scheduler.playing = true;
        runtime.shot.state = ShotState::Playing;
    }
    else if (plan.action == EndAction::Black)
    {
        ended(true);
    }
    else if (plan.action == EndAction::ReturnToLive)
    {
        goLive(runtime);
    }
    else if (plan.action == EndAction::Next && plan.next >= 0)
    {
        runtime.playlistIndex = plan.next;
        cueClip(channel, plan.nextClip, &list.entries[static_cast<std::size_t>(plan.next)]);
        runtime.scheduler.playing = plan.autoAdvance;
        runtime.shot.state = plan.autoAdvance ? ShotState::Playing : ShotState::Cued;
    }
    else
    {
        ended(false);
    }
}

void Engine::readAhead(int channel)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    // Live and black read nothing old: the newest frames are still in the page cache.
    if (runtime.liveMode || runtime.black)
    {
        runtime.readAheadCamera = -1;
        runtime.readAheadNext.clear();
        runtime.nextStart.valid = false;
        return;
    }
    auto const post = [&](int camera, std::uint64_t from, std::uint64_t to) { readAhead_->request({channel, camera, from, to}); };
    double const speed = runtime.scheduler.speed;
    auto const span = static_cast<std::uint64_t>(static_cast<double>(kReadAheadNs) * std::max(1.0, std::fabs(speed)));
    auto const position = static_cast<std::uint64_t>(std::max<std::int64_t>(0, runtime.scheduler.positionNs));
    int const camera = runtime.scheduler.camera;
    // The frames ahead of the position (behind it backwards), read again when half of them are played.
    bool const back = speed < 0;
    std::uint64_t const from = back ? position - std::min(position, span) : position;
    std::uint64_t const to = back ? position : position + span;
    if (camera != runtime.readAheadCamera || position < runtime.readAheadFrom || position > runtime.readAheadTo)
    {
        post(camera, from, to);
        runtime.readAheadCamera = camera;
        runtime.readAheadFrom = from;
        runtime.readAheadTo = to;
    }
    else if (!back && to > runtime.readAheadTo + span / 2)
    {
        post(camera, runtime.readAheadTo, to);
        runtime.readAheadTo = to;
    }
    else if (back && from + span / 2 < runtime.readAheadFrom)
    {
        post(camera, from, runtime.readAheadFrom);
        runtime.readAheadFrom = from;
    }
    // The first frames of the clip after OUT (the next entry, the idle playlist's, a loop's IN), once OUT is near:
    // kReadAheadLeadNs before the playlist's longest transition starts.
    if (runtime.clipId.empty() || !runtime.hasOut || !runtime.scheduler.playing || speed < kMinClipSpeed || position >= runtime.outNs)
    {
        return;
    }
    auto const transition = runtime.playlistId.empty() ? 0 : currentPlaylist(channel).id.empty() ? 0 : runtime.planMaxTransition;
    auto const lead = static_cast<double>(kReadAheadLeadNs) + static_cast<double>(transition) * static_cast<double>(runtime.scheduler.framePeriodNs);
    if (static_cast<double>(runtime.outNs - position) / speed > lead)
    {
        return;
    }
    auto const& idleId = config_.channelList[static_cast<std::size_t>(channel - 1)].idlePlaylist;
    auto const key = runtime.clipId + "#" + runtime.playlistId + "#" + std::to_string(runtime.playlistIndex) + "#" + idleId + "#" +
                     std::to_string(catalogVersion_) + "#" + std::to_string(runtime.outNs);
    if (key == runtime.readAheadNext)
    {
        return;
    }
    runtime.readAheadNext = key;
    runtime.nextStart.valid = false;
    auto const& list = currentPlaylist(channel);
    auto const plan = planClipEnd(runtime, list);
    // The clip after OUT: its first second read, its first frames decoded ahead at its speed (planDecodes).
    auto const next = [&](int nextCamera, std::uint64_t inNs, double nextSpeed) {
        post(nextCamera, inNs, inNs + kReadAheadNs);
        runtime.nextStart.valid = true;
        runtime.nextStart.camera = nextCamera;
        runtime.nextStart.inNs = inNs;
        runtime.nextStart.speed = nextSpeed;
    };
    if (plan.idleNext || (plan.action == EndAction::Next && plan.next >= 0))
    {
        next(plan.nextClip.camera, plan.nextClip.inNs,
            plan.next >= 0 && plan.next < static_cast<int>(list.entries.size()) ? list.entries[static_cast<std::size_t>(plan.next)].speed : 1.0);
    }
    else if (plan.action == EndAction::Loop)
    {
        next(camera, runtime.inNs, runtime.scheduler.targetSpeed);
    }
    else if (plan.action != EndAction::ReturnToLive && !idleId.empty())
    {
        ClipRef first;
        auto const idle = findPlaylist(catalog_, idleId);
        if (int const index = firstPlayable(catalog_, idle, first); index >= 0)
        {
            next(first.camera, first.inNs, idle.entries[static_cast<std::size_t>(index)].speed);
        }
    }
}

bool Engine::startIdle(int channel)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto const& id = config_.channelList[static_cast<std::size_t>(channel - 1)].idlePlaylist;
    if (id.empty())
    {
        return false;
    }
    auto const list = findPlaylist(catalog_, id);
    ClipRef clip;
    int const first = firstPlayable(catalog_, list, clip);
    if (first < 0)
    {
        return false;
    }
    runtime.playlistId = list.id;
    runtime.playlistIndex = first;
    cueClip(channel, clip, &list.entries[static_cast<std::size_t>(first)]);
    runtime.auxRestart = true;
    runtime.idle = true;
    runtime.scheduler.playing = true;
    runtime.shot.clipId = list.id;
    runtime.shot.state = ShotState::Playing;
    logInfo("idle_playlist", {{"channel", std::to_string(channel)}, {"playlist", list.id}});
    return true;
}

void Engine::stop(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    if (runtime.idle)
    {
        // The idle playlist is what a stopped channel plays: a paused one plays on.
        runtime.black = false;
        runtime.scheduler.playing = true;
        runtime.shot.state = ShotState::Playing;
        return;
    }
    if (startIdle(channel) || runtime.liveMode)
    {
        return;
    }
    // A transition in progress ends with the stop (no blend held on the output).
    runtime.mix.active = false;
    runtime.scheduler.playing = false;
    runtime.shot.state = ShotState::Ended;
}

bool Engine::setIdlePlaylist(int channel, std::string const& id, std::string& error)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        error = "unknown channel";
        return false;
    }
    if (!id.empty() && findPlaylist(catalog_, id).id.empty())
    {
        error = "unknown playlist";
        return false;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    config_.channelList[static_cast<std::size_t>(channel - 1)].idlePlaylist = id;
    auto const key = "CH" + std::to_string(channel) + "_IDLE_PLAYLIST";
    if (id.empty())
    {
        settings_.erase(key);
        effective_.erase(key);
        // The playlist plays on as an ordinary one.
        runtime.idle = false;
    }
    else
    {
        settings_[key] = id;
        effective_[key] = id;
        // It takes over only an idle channel: one that plays its idle playlist, or one stopped or
        // ended (frozen or black). A live, scrubbed, playing, cued or paused channel arms it: it
        // starts at the next clip end, stop or playlist end.
        bool const idle = runtime.idle || (!runtime.liveMode && !runtime.scheduler.playing && runtime.shot.state == ShotState::Ended);
        if (idle && !(runtime.idle && runtime.playlistId == id))
        {
            startIdle(channel);
        }
    }
    return saveSettings(error);
}

void Engine::play(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.liveMode = false;
    runtime.black = false;
    runtime.scheduler.playing = true;
    runtime.shot.state = ShotState::Playing;
}

void Engine::pause(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.liveMode = false;
    runtime.scheduler.playing = false;
    runtime.shot.state = ShotState::Paused;
}

void Engine::live(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    goLive(runtime);
    runtime.shot.state = ShotState::Idle;
}

void Engine::goLive(ChannelRuntime& runtime)
{
    runtime.liveMode = true;
    runtime.black = false;
    runtime.idle = false;
    runtime.mix.active = false;
    runtime.scheduler.playing = false;
    if (runtime.scheduler.camera == kLibraryCamera)
    {
        runtime.scheduler.camera = 1;
    }
    runtime.clipId.clear();
    runtime.playlistId.clear();
    runtime.playlistIndex = -1;
}

bool Engine::protectionFull() const
{
    std::uint64_t budget = 0;
    std::uint64_t protectedBytes = 0;
    for (std::size_t i = 0; i < cameras_.size(); ++i)
    {
        double const fps = static_cast<double>(config_.format.rateNum) / static_cast<double>(config_.format.rateDen) * hfrFactor(static_cast<int>(i + 1));
        budget += static_cast<std::uint64_t>(bytesPerFrameEstimate(config_.format.width, config_.format.height, config_.jpegQuality) * fps * 3600.0 *
                                              config_.cameras[i].bufferHours);
        protectedBytes += cameras_[i].ring.protectedBytes();
    }
    return budget > 0 && (100.0 * static_cast<double>(protectedBytes) / static_cast<double>(budget)) >= config_.protectMaxPct;
}

void Engine::setSpeed(int channel, double speed)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    channels_[static_cast<std::size_t>(channel - 1)].scheduler.setTargetSpeed(speed, config_.rampFrames);
}

void Engine::scrubFrames(int channel, int frames)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.liveMode = false;
    runtime.scheduler.playing = false;
    runtime.scheduler.scrubFrames(frames);
}

void Engine::scrubSeconds(int channel, double seconds)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.liveMode = false;
    runtime.scheduler.playing = false;
    runtime.scheduler.scrubSeconds(seconds);
}

void Engine::setPosition(int channel, std::uint64_t taiNs)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.liveMode = false;
    runtime.scheduler.positionNs = static_cast<std::int64_t>(taiNs);
}

void Engine::markIn(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.hasIn = true;
    runtime.inNs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, runtime.scheduler.positionNs));
}

void Engine::markOut(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.hasOut = true;
    runtime.outNs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, runtime.scheduler.positionNs));
}

void Engine::gotoIn(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    if (runtime.hasIn)
    {
        runtime.liveMode = false;
        runtime.scheduler.playing = false;
        runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.inNs);
    }
}

void Engine::gotoOut(int channel)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    if (runtime.hasOut)
    {
        runtime.liveMode = false;
        runtime.scheduler.playing = false;
        runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.outNs);
    }
}

void Engine::setAngle(int channel, int camera)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    if (camera < 1 || camera > static_cast<int>(cameras_.size()))
    {
        return;
    }
    channels_[static_cast<std::size_t>(channel - 1)].scheduler.camera = camera;
}

void Engine::setMotion(int channel, MotionMode mode)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(config_.channelList.size()))
    {
        return;
    }
    config_.channelList[static_cast<std::size_t>(channel - 1)].motion = mode;
}

void Engine::setAudioMode(int channel, AudioMode mode)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(config_.channelList.size()))
    {
        return;
    }
    config_.channelList[static_cast<std::size_t>(channel - 1)].audio = mode;
}

void Engine::setTimecodeMode(int channel, TcMode mode)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(config_.channelList.size()))
    {
        return;
    }
    config_.channelList[static_cast<std::size_t>(channel - 1)].timecode = mode;
}

void Engine::setLock(int channel, bool enabled)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(config_.channelList.size()))
    {
        return;
    }
    config_.channelList[static_cast<std::size_t>(channel - 1)].lockToFirst = enabled;
}

bool Engine::setPlayOnTally(int channel, bool enabled, std::string& error)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(config_.channelList.size()))
    {
        error = "unknown channel";
        return false;
    }
    config_.channelList[static_cast<std::size_t>(channel - 1)].playOnTally = enabled;
    settings_["CH" + std::to_string(channel) + "_PLAY_ON_TALLY"] = enabled ? "true" : "false";
    effective_["CH" + std::to_string(channel) + "_PLAY_ON_TALLY"] = enabled ? "true" : "false";
    return saveSettings(error);
}

bool Engine::saveSettings(std::string& error) const
{
    return writeConfigFile(config_.stateDir + "/config.json", settings_, error);
}

void Engine::applyTally(TslMessage const& message)
{
    metrics_.inc("tsl_packets_total");
    if (!message.error.empty())
    {
        metrics_.inc("tsl_errors_total");
        return;
    }
    bool played = false;
    {
        std::lock_guard lock{mutex_};
        for (auto const& display : message.displays)
        {
            if (config_.tslScreen >= 0 && display.screen != config_.tslScreen)
            {
                continue;
            }
            int const channel = channelForDisplay(config_.tslMap, display.index);
            if (channel < 1 || channel > static_cast<int>(channels_.size()))
            {
                continue;
            }
            auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
            auto const& cfg = config_.channelList[static_cast<std::size_t>(channel - 1)];
            bool const wasRed = runtime.tslLh == 1 || runtime.tslText == 1;
            runtime.tslLh = display.lh;
            runtime.tslRh = display.rh;
            runtime.tslText = display.text;
            bool const red = display.lh == 1 || display.text == 1;
            auto const shot = runtime.shot.state;
            // Only the rising edge: a red that stays on (the controller repeats it) never plays again.
            if (!red || wasRed || !cfg.playOnTally || runtime.liveMode || (shot != ShotState::Cued && shot != ShotState::Paused))
            {
                continue;
            }
            play(channel);
            runtime.tallyPlayNs = taiNowNs();
            metrics_.inc("tally_plays_total", {{"channel", std::to_string(channel)}});
            logInfo("tally_play", {{"channel", std::to_string(channel)}, {"label", cfg.label}, {"display", std::to_string(display.index)},
                                      {"shot", shotStateName(shot)}, {"clip", runtime.shot.clipId}});
            played = true;
        }
    }
    // As after an API command: the flows of the next grains start now.
    if (played && bufferReady_)
    {
        planFlows();
    }
}

std::string Engine::createClip(int channel, std::string const& name, bool allAngles, bool force, std::string& error, std::string const& colour,
    std::string const& tags)
{
    std::lock_guard lock{mutex_};
    error.clear();
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        error = "unknown channel";
        return {};
    }
    auto const& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    if (!runtime.hasIn || !runtime.hasOut || runtime.outNs <= runtime.inNs)
    {
        error = "mark IN and OUT first";
        return {};
    }
    if (!force && protectionFull())
    {
        error = "protected ranges exceed " + std::to_string(config_.protectMaxPct) + "% of storage; export or delete clips, or pass force";
        return {};
    }
    auto const group = "group-" + std::to_string(clipSerial_);
    std::string first;
    int const from = allAngles ? 1 : runtime.scheduler.camera;
    int const to = allAngles ? static_cast<int>(cameras_.size()) : runtime.scheduler.camera;
    for (int camera = from; camera <= to; ++camera)
    {
        // Null for kLibraryCamera: a clip marked while an upload plays.
        auto const* cameraCfg = camera >= 1 && camera <= static_cast<int>(config_.cameras.size()) ? &config_.cameras[static_cast<std::size_t>(camera - 1)] : nullptr;
        ClipRef clip;
        clip.id = "clip-" + std::to_string(clipSerial_++);
        clip.name = name.empty() ? clip.id : name;
        if (allAngles && cameraCfg != nullptr)
        {
            clip.name += " " + cameraCfg->label;
        }
        clip.camera = camera;
        clip.inNs = runtime.inNs;
        clip.outNs = runtime.outNs;
        // The speed the operator set (a ramp only runs while playing). A clip plays forward:
        // from a paused (0) or reverse setting it plays at 100 %.
        double const speed = runtime.scheduler.targetSpeed;
        clip.speed = speed >= kMinClipSpeed && speed <= kMaxClipSpeed ? speed : 1.0;
        clip.motion = motionName(config_.channelList[static_cast<std::size_t>(channel - 1)].motion);
        clip.audio = audioModeName(config_.channelList[static_cast<std::size_t>(channel - 1)].audio);
        if (!colour.empty())
        {
            clip.colour = colour;
        }
        else if (cameraCfg != nullptr)
        {
            clip.colour = cameraCfg->colour;
        }
        clip.tags = tags;
        clip.end = EndAction::Freeze;
        clip.groupId = allAngles ? group : "";
        catalog_.upsertClip(clip);
        ++catalogVersion_;
        if (auto* ring = ringOf(camera))
        {
            ring->protect(clip.inNs, clip.outNs);
        }
        if (first.empty())
        {
            first = clip.id;
        }
    }
    return first;
}

ClipEdit Engine::updateClip(ClipRef clip, bool force)
{
    std::lock_guard lock{mutex_};
    auto const existing = clip.id.empty() ? ClipRef{} : catalog_.clip(clip.id);
    if (existing.id.empty())
    {
        return ClipEdit::Unknown;
    }
    if (clip.name.empty())
    {
        clip.name = existing.name;
    }
    bool const moved = existing.camera != clip.camera || existing.inNs != clip.inNs || existing.outNs != clip.outNs;
    if (moved)
    {
        // A new IN or OUT needs a recorded frame within one frame period and lands on it (an
        // index lookup, no read): no range outside the buffer that retention could never free.
        auto const* ring = ringOf(clip.camera);
        auto const period = sourcePeriod(clip.camera);
        auto const snap = [&](std::uint64_t& ns) {
            auto const nearest = ring != nullptr ? ring->nearestNs(ns) : 0;
            if (nearest == 0 || (nearest > ns ? nearest - ns : ns - nearest) > period)
            {
                return false;
            }
            ns = nearest;
            return true;
        };
        if ((clip.inNs != existing.inNs && !snap(clip.inNs)) || (clip.outNs != existing.outNs && !snap(clip.outNs)))
        {
            return ClipEdit::NoFrame;
        }
        if (clip.outNs < clip.inNs)
        {
            return ClipEdit::OutBeforeIn;
        }
        bool const grows = existing.camera != clip.camera || clip.inNs < existing.inNs || clip.outNs > existing.outNs;
        if (grows && holdsRange(existing) && !force && protectionFull())
        {
            return ClipEdit::OverCap;
        }
    }
    catalog_.upsertClip(clip);
    ++catalogVersion_;
    if (moved && holdsRange(existing))
    {
        // The new range first: frames both ranges share stay protected throughout.
        if (auto* ring = ringOf(clip.camera))
        {
            ring->protect(clip.inNs, clip.outNs);
        }
        if (auto* ring = ringOf(existing.camera))
        {
            ring->unprotect(existing.inNs, existing.outNs);
        }
    }
    return ClipEdit::Ok;
}

bool Engine::deleteClip(std::string const& id)
{
    FrameRing* ring = nullptr;
    {
        std::lock_guard lock{mutex_};
        auto clip = catalog_.clip(id);
        if (clip.id.empty())
        {
            return false;
        }
        catalog_.deleteClip(id);
        ++catalogVersion_;
        // A consolidated clip released its range already; releasing it again could remove
        // another clip's identical range.
        ring = holdsRange(clip) ? ringOf(clip.camera) : nullptr;
        if (ring != nullptr)
        {
            ring->unprotect(clip.inNs, clip.outNs);
        }
    }
    // Camera rings drop expired segments when they rotate; the library ring only here.
    if (ring != nullptr)
    {
        ring->enforceRetention();
    }
    return true;
}

std::vector<ClipRef> Engine::clips() const
{
    std::lock_guard lock{mutex_};
    return catalog_.clips();
}

ClipRef Engine::clip(std::string const& id) const
{
    std::lock_guard lock{mutex_};
    return catalog_.clip(id);
}

void Engine::cueClip(int channel, ClipRef const& clip, PlaylistEntry const* entry)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.liveMode = false;
    runtime.black = false;
    runtime.idle = false;
    runtime.mix.active = false;
    runtime.ownAudio = entry == nullptr || entry->ownAudio;
    runtime.clipId = clip.id;
    runtime.hasIn = true;
    runtime.hasOut = true;
    runtime.inNs = clip.inNs;
    runtime.outNs = clip.outNs;
    runtime.scheduler.camera = clip.camera;
    runtime.scheduler.positionNs = static_cast<std::int64_t>(clip.inNs);
    runtime.scheduler.setTargetSpeed(entry != nullptr ? entry->speed : clip.speed, 0);
    runtime.scheduler.playing = false;
    runtime.shot.end = entry != nullptr ? entry->end : clip.end;
    // The cued frame, and the first one a play steps to (a cue that plays at once shows that one), are decoded on
    // the worker from now, not on the next grain's output thread (1.8.6); the next grain's plan keeps them.
    std::array<AheadFrame, 2> cued{};
    int count = 0;
    auto const periodSrc = sourcePeriod(clip.camera);
    auto const* ring = ringOf(clip.camera);
    for (auto const positionNs : {static_cast<std::int64_t>(clip.inNs),
             static_cast<std::int64_t>(clip.inNs) + static_cast<std::int64_t>(std::llround(runtime.scheduler.targetSpeed * static_cast<double>(runtime.scheduler.framePeriodNs)))})
    {
        auto const pick = pickSource(std::max<std::int64_t>(0, positionNs), 0, runtime.scheduler.framePeriodNs, hfrFactor(clip.camera), config_.hfrSnap,
            MotionMode::Repeat);
        AheadFrame const frame{clip.camera, static_cast<std::uint64_t>(std::max<std::int64_t>(0, pick.frameA)) * periodSrc};
        if (ring != nullptr && frame.taiNs <= ring->newestNs() && (count == 0 || !(cued[0] == frame)))
        {
            cued[static_cast<std::size_t>(count++)] = frame;
        }
    }
    runtime.decodeAhead->plan(cued.data(), count);
    bool ok = false;
    config_.channelList[static_cast<std::size_t>(channel - 1)].motion = parseMotion(clip.motion, &ok);
    config_.channelList[static_cast<std::size_t>(channel - 1)].audio = parseAudioMode(clip.audio, &ok);
}

void Engine::playClip(int channel, std::string const& clipId)
{
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto clip = catalog_.clip(clipId);
    if (clip.id.empty())
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.playlistId.clear();
    runtime.playlistIndex = -1;
    cueClip(channel, clip, nullptr);
    runtime.scheduler.playing = true;
    runtime.shot.clipId = clip.id;
    runtime.shot.state = ShotState::Playing;
}

ShotState Engine::shotClick(int channel, std::string const& id)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return ShotState::Idle;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    // A clip, or a playlist that starts with its first clip that still exists.
    auto clip = catalog_.clip(id);
    auto const list = clip.id.empty() ? findPlaylist(catalog_, id) : Catalog::Playlist{};
    int const first = clip.id.empty() ? firstPlayable(catalog_, list, clip) : -1;
    if (clip.id.empty())
    {
        return runtime.shot.state;
    }
    if (runtime.shot.clipId != id)
    {
        runtime.shot.state = ShotState::Idle;
        runtime.shot.clipId = id;
    }
    runtime.shot.playOnFirstClick = config_.playOnFirstClick;
    auto const before = runtime.shot.state;
    auto const state = runtime.shot.click();
    if (state == ShotState::Paused)
    {
        runtime.scheduler.playing = false;
    }
    else if (state == ShotState::Playing && (before == ShotState::Paused || before == ShotState::Cued))
    {
        // Resume where it stands (the cue put it on IN).
        runtime.liveMode = false;
        runtime.black = false;
        runtime.scheduler.playing = true;
    }
    else
    {
        // Cue, or play from the start (play on first click, or after the clip ended).
        runtime.playlistId = first >= 0 ? id : std::string{};
        runtime.playlistIndex = first;
        cueClip(channel, clip, first >= 0 ? &list.entries[static_cast<std::size_t>(first)] : nullptr);
        runtime.auxRestart = true;
        runtime.scheduler.playing = state == ShotState::Playing;
    }
    return runtime.shot.state;
}

std::string Engine::createPlaylist(std::string const& name, std::vector<PlaylistEntry> const& entries, bool loop, AuxTrack const& aux)
{
    std::lock_guard lock{mutex_};
    Catalog::Playlist playlist;
    playlist.id = "playlist-" + std::to_string(clipSerial_++);
    playlist.name = name.empty() ? playlist.id : name;
    playlist.entries = entries;
    playlist.loop = loop;
    playlist.aux = aux;
    catalog_.upsertPlaylist(playlist);
    ++catalogVersion_;
    return playlist.id;
}

bool Engine::updatePlaylist(Catalog::Playlist const& playlist)
{
    std::lock_guard lock{mutex_};
    if (findPlaylist(catalog_, playlist.id).id.empty())
    {
        return false;
    }
    auto stored = playlist;
    if (stored.name.empty())
    {
        stored.name = stored.id;
    }
    catalog_.upsertPlaylist(stored);
    ++catalogVersion_;
    // A channel playing the playlist stays on its entry: the same clip nearest its old place,
    // else the same place (or the last entry).
    int const size = static_cast<int>(stored.entries.size());
    for (auto& runtime : channels_)
    {
        if (runtime.playlistId != stored.id)
        {
            continue;
        }
        int best = -1;
        for (int i = 0; i < size; ++i)
        {
            if (stored.entries[static_cast<std::size_t>(i)].clipId == runtime.clipId &&
                (best < 0 || std::abs(i - runtime.playlistIndex) < std::abs(best - runtime.playlistIndex)))
            {
                best = i;
            }
        }
        runtime.playlistIndex = best >= 0 ? best : std::min(runtime.playlistIndex, size - 1);
        if (runtime.playlistIndex < 0)
        {
            runtime.playlistId.clear();
        }
    }
    return true;
}

bool Engine::deletePlaylist(std::string const& id)
{
    std::lock_guard lock{mutex_};
    if (findPlaylist(catalog_, id).id.empty())
    {
        return false;
    }
    catalog_.deletePlaylist(id);
    ++catalogVersion_;
    // A channel whose idle playlist it was has none any more.
    bool changed = false;
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        auto& cfg = config_.channelList[i];
        if (cfg.idlePlaylist == id)
        {
            cfg.idlePlaylist.clear();
            settings_.erase("CH" + std::to_string(i + 1) + "_IDLE_PLAYLIST");
            effective_.erase("CH" + std::to_string(i + 1) + "_IDLE_PLAYLIST");
            changed = true;
        }
        if (channels_[i].playlistId == id)
        {
            channels_[i].idle = false;
        }
    }
    std::string problem;
    if (changed && !saveSettings(problem))
    {
        logWarn("settings_not_saved", {{"error", problem}});
    }
    return true;
}

std::vector<Catalog::Playlist> Engine::playlists() const
{
    std::lock_guard lock{mutex_};
    return catalog_.playlists();
}

Catalog::Playlist Engine::playlist(std::string const& id) const
{
    std::lock_guard lock{mutex_};
    return findPlaylist(catalog_, id);
}

void Engine::playPlaylist(int channel, std::string const& id, bool play)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return;
    }
    auto const list = findPlaylist(catalog_, id);
    ClipRef clip;
    int const first = firstPlayable(catalog_, list, clip);
    if (first < 0)
    {
        return;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    runtime.playlistId = id;
    runtime.playlistIndex = first;
    cueClip(channel, clip, &list.entries[static_cast<std::size_t>(first)]);
    runtime.auxRestart = true;
    runtime.scheduler.playing = play;
    runtime.shot.clipId = id;
    runtime.shot.state = play ? ShotState::Playing : ShotState::Cued;
}

std::string Engine::upload(std::uint8_t const* data, std::size_t size, std::string const& name, std::string& error)
{
    return storeUpload(UploadSource{{}, data, size}, name, error);
}

std::string Engine::uploadFile(std::string const& path, std::string const& name, std::string& error)
{
    return storeUpload(UploadSource{path, nullptr, 0}, name, error);
}

std::string Engine::storeUpload(UploadSource const& source, std::string const& name, std::string& error)
{
    // One upload at a time: its frames go to the library ring while the file is read (1.8.2), on the
    // frame grid after the ring's newest frame (a camera ring only appends, so frames older than its
    // live recording would be refused).
    std::lock_guard serial{uploadMutex_};
    auto const period = static_cast<std::uint64_t>(framePeriodNs(config_.format.rateNum, config_.format.rateDen));
    auto const origin = (library_->newestNs() / period + 1) * period;
    // Protected from the first frame on (the library keeps only segments a clip touches); the end is
    // known when the file is read.
    constexpr std::uint64_t kOpenEnd = std::numeric_limits<std::uint64_t>::max();
    library_->protect(origin, kOpenEnd);
    std::string audioId;
    std::string audioPath;
    std::ofstream audioFile;
    std::string problem;
    std::vector<std::uint8_t> jpeg;
    std::uint32_t encoded = 0;
    bool haveJpeg = false;
    UploadSink sink;
    sink.frame = [&](std::uint64_t n, std::uint32_t sourceFrame, Frame10 const& picture, std::vector<float> const& audio) {
        if (uploadsStopped_.load())
        {
            problem = "shutting down";
            return false;
        }
        // House frame n shows source frame `sourceFrame`; a repeated source frame is encoded once (1.8.1).
        if (!haveJpeg || sourceFrame != encoded)
        {
            jpeg = gpuEncodeFrame10(picture, config_.jpegQuality);
            if (jpeg.empty())
            {
                jpeg = encodeJpeg422(picture, config_.jpegQuality);
            }
            encoded = sourceFrame;
            haveJpeg = true;
        }
        StoredFrame frame;
        frame.taiNs = origin + n * period;
        frame.jpeg = jpeg;
        frame.audio = audio;
        if (!library_->push(std::move(frame)))
        {
            problem = "upload could not be stored (frame " + std::to_string(n) + ")";
            return false;
        }
        return true;
    };
    sink.audio = [&](float const* samples, std::size_t count) {
        if (uploadsStopped_.load())
        {
            problem = "shutting down";
            return false;
        }
        // An audio item (1.6.0): 48 kHz stereo float, written to a file of the library and read per grain.
        if (audioId.empty())
        {
            {
                std::lock_guard lock{mutex_};
                audioId = std::string(kAudioItemPrefix) + std::to_string(clipSerial_++);
            }
            std::error_code ec;
            std::filesystem::create_directories(libraryDir_ + "/audio", ec);
            audioPath = audioItemPath(audioId);
            audioFile.open(audioPath + ".tmp", std::ios::binary | std::ios::trunc);
        }
        audioFile.write(reinterpret_cast<char const*>(samples), static_cast<std::streamsize>(count * sizeof(float)));
        if (!audioFile)
        {
            problem = "audio could not be stored";
            return false;
        }
        return true;
    };
    auto const result = streamUpload(source, config_.format, sink);
    std::error_code ec;
    if (result.audioOnly)
    {
        library_->unprotect(origin, kOpenEnd);
        audioFile.close();
        if (result.stopped || !result.error.empty() || result.audioSamples == 0 || !audioFile)
        {
            if (!audioPath.empty())
            {
                std::filesystem::remove(audioPath + ".tmp", ec);
            }
            error = !problem.empty() ? problem : !result.error.empty() ? result.error : "audio could not be stored";
            return {};
        }
        std::filesystem::rename(audioPath + ".tmp", audioPath, ec);
        if (ec)
        {
            error = "audio could not be stored";
            return {};
        }
        Catalog::AudioItem item;
        item.id = audioId;
        item.name = name.empty() ? audioId : name;
        item.samples = static_cast<std::int64_t>(result.audioSamples);
        item.createdNs = taiNowNs();
        std::lock_guard lock{mutex_};
        catalog_.upsertAudioItem(item);
        ++catalogVersion_;
        error.clear();
        return audioId;
    }
    if (result.stopped || result.frames == 0 || (!result.error.empty() && result.error != "scaled"))
    {
        // The frames already stored are removed with their segments (not kept until the next upload).
        library_->unprotect(origin, kOpenEnd);
        library_->discardFrom(origin);
        error = !problem.empty() ? problem : !result.error.empty() ? result.error : "upload failed";
        return {};
    }
    error.clear();
    ClipRef clip;
    clip.camera = kLibraryCamera;
    clip.inNs = origin;
    clip.outNs = origin + (result.frames - 1) * period;
    library_->protect(clip.inNs, clip.outNs);
    library_->unprotect(origin, kOpenEnd);
    std::lock_guard lock{mutex_};
    clip.id = "upload-" + std::to_string(clipSerial_++);
    clip.name = name.empty() ? clip.id : name;
    clip.speed = 1;
    clip.motion = "interpolate";
    clip.audio = "mute";
    clip.library = true;
    clip.end = EndAction::Freeze;
    catalog_.upsertClip(clip);
    ++catalogVersion_;
    return clip.id;
}

bool Engine::auxSourceExists(std::string const& source) const
{
    std::lock_guard lock{mutex_};
    if (source.empty())
    {
        return true;
    }
    return source.rfind(kAudioItemPrefix, 0) == 0 ? !catalog_.audioItem(source).id.empty() : !catalog_.clip(source).id.empty();
}

std::vector<Catalog::AudioItem> Engine::audioItems() const
{
    std::lock_guard lock{mutex_};
    return catalog_.audioItems();
}

Catalog::AudioItem Engine::audioItem(std::string const& id) const
{
    std::lock_guard lock{mutex_};
    return catalog_.audioItem(id);
}

bool Engine::renameAudioItem(std::string const& id, std::string const& name)
{
    std::lock_guard lock{mutex_};
    auto item = catalog_.audioItem(id);
    if (item.id.empty())
    {
        return false;
    }
    item.name = name;
    catalog_.upsertAudioItem(item);
    ++catalogVersion_;
    return true;
}

bool Engine::deleteAudioItem(std::string const& id)
{
    std::lock_guard lock{mutex_};
    if (catalog_.audioItem(id).id.empty())
    {
        return false;
    }
    catalog_.deleteAudioItem(id);
    // The playlists that used it lose their aux track.
    for (auto playlist : catalog_.playlists())
    {
        if (playlist.aux.source == id)
        {
            playlist.aux.source.clear();
            catalog_.upsertPlaylist(playlist);
        }
    }
    ++catalogVersion_;
    std::error_code ec;
    std::filesystem::remove(audioItemPath(id), ec);
    return true;
}

std::string Engine::exportClip(std::string const& id, std::string& error)
{
    ClipRef clip;
    std::uint64_t period = 0;
    FrameRing const* ring = nullptr;
    {
        std::lock_guard lock{mutex_};
        clip = catalog_.clip(id);
        if (clip.id.empty())
        {
            error = "unknown clip";
            return {};
        }
        period = sourcePeriod(clip.camera);
        ring = ringOf(clip.camera);
    }
    if (ring == nullptr)
    {
        error = "the clip's camera is not configured";
        return {};
    }
    // Reads, decoding and file writes run without the engine lock (the ring has its own).
    auto const dir = libraryDir_ + "/" + id;
    std::filesystem::create_directories(dir);
    std::vector<float> audio;
    int index = 0;
    for (std::uint64_t tai = clip.inNs; tai <= clip.outNs; tai += period)
    {
        auto const stored = ring->findNearest(tai);
        Frame10 frame;
        if (!stored || !decodeJpeg422(stored->jpeg.data(), stored->jpeg.size(), frame))
        {
            continue;
        }
        auto const jpeg = encodeJpeg422(frame, config_.jpegQuality);
        std::ofstream out(dir + "/frame_" + std::to_string(index) + ".jpg", std::ios::binary);
        out.write(reinterpret_cast<char const*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        audio.insert(audio.end(), stored->audio.begin(), stored->audio.end());
        ++index;
    }
    auto const wav = writeWav(audio, 2, 48000);
    std::ofstream wavOut(dir + "/audio.wav", std::ios::binary);
    wavOut.write(reinterpret_cast<char const*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    return dir;
}

bool Engine::consolidate(std::string const& id, std::string& error)
{
    auto const dir = exportClip(id, error);
    if (dir.empty())
    {
        return false;
    }
    std::lock_guard lock{mutex_};
    auto clip = catalog_.clip(id);
    // Released once: a second consolidate would remove another clip's identical range.
    if (clip.id.empty() || clip.library)
    {
        return true;
    }
    clip.library = true;
    catalog_.upsertClip(clip);
    ++catalogVersion_;
    if (clip.camera >= 1 && clip.camera <= static_cast<int>(cameras_.size()))
    {
        cameras_[static_cast<std::size_t>(clip.camera - 1)].ring.unprotect(clip.inNs, clip.outNs);
    }
    return true;
}

std::string Engine::routesPath() const
{
    return config_.stateDir + "/routes.json";
}

void Engine::loadRoutes()
{
    std::ifstream in(routesPath());
    if (!in)
    {
        return;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto const body = buffer.str();
    std::size_t cursor = 0;
    while (true)
    {
        auto const start = body.find('{', cursor);
        if (start == std::string::npos)
        {
            break;
        }
        auto const end = body.find('}', start);
        if (end == std::string::npos)
        {
            break;
        }
        auto const item = body.substr(start, end - start + 1);
        cursor = end + 1;
        auto field = [&](char const* key) {
            auto const needle = std::string("\"") + key + "\"";
            auto const pos = item.find(needle);
            if (pos == std::string::npos)
            {
                return std::string{};
            }
            auto const colon = item.find(':', pos + needle.size());
            if (colon == std::string::npos)
            {
                return std::string{};
            }
            auto i = colon + 1;
            while (i < item.size() && item[i] == ' ')
            {
                ++i;
            }
            if (i < item.size() && item[i] == '"')
            {
                auto const stop = item.find('"', i + 1);
                return stop == std::string::npos ? std::string{} : item.substr(i + 1, stop - i - 1);
            }
            auto stop = i;
            while (stop < item.size() && item[stop] != ',' && item[stop] != '}')
            {
                ++stop;
            }
            return item.substr(i, stop - i);
        };
        int const camera = std::atoi(field("camera").c_str());
        int const phase = std::atoi(field("phase").c_str());
        bool const video = field("video") != "false";
        Route route;
        route.active = field("active") == "true";
        route.domainId = field("domain_id");
        route.flowId = field("flow_id");
        route.senderId = field("sender_id");
        route.state = field("state").empty() ? (route.active ? "running" : "waiting") : field("state");
        if (camera > 0 && phase > 0)
        {
            routes_[std::to_string(camera) + (video ? "v" : "a") + std::to_string(phase)] = route;
        }
    }
}

void Engine::saveRoutes() const
{
    std::ofstream out(routesPath());
    if (!out)
    {
        return;
    }
    out << '[';
    bool first = true;
    for (auto const& [key, route] : routes_)
    {
        if (key.size() < 3)
        {
            continue;
        }
        bool const video = key.find('v') != std::string::npos;
        auto const mark = key.find(video ? 'v' : 'a');
        int const camera = std::atoi(key.substr(0, mark).c_str());
        int const phase = std::atoi(key.substr(mark + 1).c_str());
        if (!first)
        {
            out << ',';
        }
        first = false;
        out << "{\"camera\":" << camera << ",\"phase\":" << phase << ",\"video\":" << (video ? "true" : "false") << ",\"active\":"
            << (route.active ? "true" : "false") << ",\"domain_id\":\"" << jsonEscape(route.domainId) << "\",\"flow_id\":\"" << jsonEscape(route.flowId)
            << "\",\"sender_id\":\"" << jsonEscape(route.senderId) << "\",\"state\":\"" << jsonEscape(route.state) << "\"}";
    }
    out << "]\n";
}

void Engine::setRoute(int camera, int phase, bool video, Route route, bool persist)
{
    std::lock_guard lock{mutex_};
    routes_[std::to_string(camera) + (video ? "v" : "a") + std::to_string(phase)] = std::move(route);
    if (persist)
    {
        saveRoutes();
    }
}

bool Engine::setRouteState(int camera, int phase, bool video, Route const& expected, std::string const& state)
{
    std::lock_guard lock{mutex_};
    auto const it = routes_.find(std::to_string(camera) + (video ? "v" : "a") + std::to_string(phase));
    if (it == routes_.end() || !it->second.active || it->second.domainId != expected.domainId || it->second.flowId != expected.flowId ||
        it->second.senderId != expected.senderId)
    {
        return false;
    }
    it->second.state = state;
    return true;
}


std::string Engine::exportConfigJson() const
{
    std::lock_guard lock{mutex_};
    std::ostringstream out;
    out << "{\"version\":1,\"secrets\":false,\"settings\":{";
    bool first = true;
    for (auto const& [key, value] : effective_)
    {
        if (!first)
        {
            out << ',';
        }
        first = false;
        out << '"' << jsonEscape(key) << "\":\"" << jsonEscape(value) << '"';
    }
    out << "},\"clips\":[";
    first = true;
    for (auto const& clip : catalog_.clips())
    {
        if (!first)
        {
            out << ',';
        }
        first = false;
        out << "{\"id\":\"" << jsonEscape(clip.id) << "\",\"name\":\"" << jsonEscape(clip.name) << "\",\"camera\":" << clip.camera << ",\"in_ns\":" << clip.inNs
            << ",\"out_ns\":" << clip.outNs << ",\"speed\":" << clip.speed << ",\"motion\":\"" << jsonEscape(clip.motion) << "\",\"audio\":\""
            << jsonEscape(clip.audio) << "\",\"colour\":\"" << jsonEscape(clip.colour) << "\",\"tags\":\"" << jsonEscape(clip.tags) << "\",\"end\":\""
            << endActionName(clip.end) << "\",\"library\":" << (clip.library ? "true" : "false") << ",\"group\":\"" << jsonEscape(clip.groupId) << "\"}";
    }
    out << "],\"playlists\":[";
    first = true;
    for (auto const& playlist : catalog_.playlists())
    {
        if (!first)
        {
            out << ',';
        }
        first = false;
        out << "{\"id\":\"" << jsonEscape(playlist.id) << "\",\"name\":\"" << jsonEscape(playlist.name) << "\",\"loop\":" << (playlist.loop ? "true" : "false")
            << ",\"entries\":[";
        bool entryFirst = true;
        for (auto const& entry : playlist.entries)
        {
            if (!entryFirst)
            {
                out << ',';
            }
            entryFirst = false;
            out << "{\"clip_id\":\"" << jsonEscape(entry.clipId) << "\",\"speed\":" << entry.speed << ",\"end\":\"" << endActionName(entry.end)
                << "\",\"auto_advance\":" << (entry.autoAdvance ? "true" : "false") << ',' << entryTransitionJson(entry) << '}';
        }
        out << "],\"aux\":" << auxJson(playlist.aux) << '}';
    }
    out << "]}";
    return out.str();
}

Engine::ImportResult Engine::importConfigJson(std::string const& body)
{
    ImportResult result;
    Json doc;
    if (!parseJson(body, doc) || doc.kind != Json::Kind::Object)
    {
        result.error = "the document must be a JSON object";
        return result;
    }
    try
    {
        // An exported document, or a flat object of settings.
        auto const* settings = doc.get("settings");
        bool const flat = settings == nullptr && doc.get("clips") == nullptr && doc.get("playlists") == nullptr;
        // `merged`: the effective settings after the import (checked); `state`: what the UI set, saved.
        auto merged = effective_;
        auto state = settings_;
        if (settings != nullptr || flat)
        {
            auto const& object = settings != nullptr ? *settings : doc;
            bool const strings = object.kind == Json::Kind::Object &&
                                 std::all_of(object.members.begin(), object.members.end(), [](auto const& member) { return member.second.kind == Json::Kind::String; });
            if (!strings)
            {
                result.error = "settings must be an object of strings";
                return result;
            }
            for (auto const& [key, value] : object.members)
            {
                merged[key] = value.text;
                state[key] = value.text;
            }
        }
        auto const loaded = loadConfig({}, {}, merged);
        static char const* restartKeys[] = {"REPLAY_FORMAT", "REPLAY_INPUTS", "REPLAY_CHANNELS", "REPLAY_STORAGE_DIR", "REPLAY_STATE_DIR", "WEB_PORT",
            "NMOS_PORT", "NMOS_SEED", "NMOS_LABEL", "NMOS_HOST_ADDRESS", "MXL_OUTPUT_DOMAIN_DIR", "MXL_OUTPUT_DOMAIN_ID", "NMOS_REGISTRY_ADDRESS",
            "NMOS_REGISTRY_PORT", "NMOS_QUERY_ADDRESS", "NMOS_QUERY_PORT", "REPLAY_PREVIEW_MODE", "PREVIEW_PUBLISH_URL", "PREVIEW_PATH_PREFIX",
            "PREVIEW_WHEP_URL", "PREVIEW_HLS_URL", "MEDIAMTX_RTSP_PORT", "MEDIAMTX_WHEP_PORT", "MEDIAMTX_HLS_PORT", "MEDIAMTX_ICE_UDP_PORT",
            "WIDGET_FRAME_ANCESTORS", "TSL_ENABLE", "TSL_UDP_PORT", "TSL_TCP_PORT"};
        for (auto const* key : restartKeys)
        {
            auto const before = effective_.count(key) ? effective_.at(key) : std::string{};
            auto const after = merged.count(key) ? merged.at(key) : std::string{};
            if (before != after)
            {
                result.restartRequired = true;
            }
        }
        std::lock_guard lock{mutex_};
        effective_ = merged;
        settings_ = state;
        if (loaded.config.inputs == config_.inputs)
        {
            for (std::size_t i = 0; i < config_.cameras.size(); ++i)
            {
                config_.cameras[i].label = loaded.config.cameras[i].label;
                config_.cameras[i].colour = loaded.config.cameras[i].colour;
            }
        }
        if (loaded.config.channels == config_.channels)
        {
            for (std::size_t i = 0; i < config_.channelList.size(); ++i)
            {
                config_.channelList[i].label = loaded.config.channelList[i].label;
                config_.channelList[i].motion = loaded.config.channelList[i].motion;
                config_.channelList[i].audio = loaded.config.channelList[i].audio;
                config_.channelList[i].idle = loaded.config.channelList[i].idle;
                config_.channelList[i].timecode = loaded.config.channelList[i].timecode;
                config_.channelList[i].playOnTally = loaded.config.channelList[i].playOnTally;
                config_.channelList[i].idlePlaylist = loaded.config.channelList[i].idlePlaylist;
            }
        }
        config_.tslScreen = loaded.config.tslScreen;
        config_.tslMap = loaded.config.tslMap;
        if (!saveSettings(result.error))
        {
            return result;
        }
        auto const text = [](Json const& item, char const* key) {
            auto const* field = item.get(key);
            return field != nullptr ? jsonText(*field).value_or(std::string{}) : std::string{};
        };
        auto const* clips = doc.get("clips");
        for (auto const& item : clips != nullptr ? clips->items : std::vector<Json>{})
        {
            ClipRef clip;
            clip.id = text(item, "id");
            if (clip.id.empty())
            {
                continue;
            }
            clip.name = text(item, "name");
            auto const* field = item.get("camera");
            clip.camera = static_cast<int>(field != nullptr ? jsonInt(*field).value_or(clip.camera) : clip.camera);
            field = item.get("in_ns");
            clip.inNs = field != nullptr ? jsonUnsigned(*field).value_or(0) : 0;
            field = item.get("out_ns");
            clip.outNs = field != nullptr ? jsonUnsigned(*field).value_or(clip.inNs) : clip.inNs;
            field = item.get("speed");
            clip.speed = field != nullptr ? jsonDouble(*field).value_or(clip.speed) : clip.speed;
            clip.motion = text(item, "motion");
            clip.audio = text(item, "audio");
            clip.colour = text(item, "colour");
            clip.tags = text(item, "tags");
            bool endOk = false;
            clip.end = parseEndAction(text(item, "end"), &endOk);
            if (!endOk)
            {
                clip.end = EndAction::Freeze;
            }
            field = item.get("library");
            clip.library = field != nullptr && jsonBool(*field).value_or(false);
            clip.groupId = text(item, "group");
            // The imported range is protected now, not at the next start; a clip it replaces
            // releases its own (after: frames both ranges share stay protected).
            auto const previous = catalog_.clip(clip.id);
            catalog_.upsertClip(clip);
            ++catalogVersion_;
            if (auto* ring = holdsRange(clip) ? ringOf(clip.camera) : nullptr)
            {
                ring->protect(clip.inNs, clip.outNs);
            }
            if (auto* ring = !previous.id.empty() && holdsRange(previous) ? ringOf(previous.camera) : nullptr)
            {
                ring->unprotect(previous.inNs, previous.outNs);
            }
        }
        auto const* playlists = doc.get("playlists");
        for (auto const& item : playlists != nullptr ? playlists->items : std::vector<Json>{})
        {
            Catalog::Playlist playlist;
            playlist.id = text(item, "id");
            playlist.name = text(item, "name");
            if (playlist.id.empty())
            {
                continue;
            }
            auto const* loop = item.get("loop");
            playlist.loop = loop != nullptr && jsonBool(*loop).value_or(false);
            auto const* entries = item.get("entries");
            for (auto const& entryItem : entries != nullptr ? entries->items : std::vector<Json>{})
            {
                PlaylistEntry entry;
                entry.clipId = text(entryItem, "clip_id");
                auto const* field = entryItem.get("speed");
                entry.speed = field != nullptr ? jsonDouble(*field).value_or(entry.speed) : entry.speed;
                bool endOk = false;
                entry.end = parseEndAction(text(entryItem, "end"), &endOk);
                if (!endOk)
                {
                    entry.end = EndAction::Next;
                }
                field = entryItem.get("auto_advance");
                entry.autoAdvance = field == nullptr || jsonBool(*field).value_or(true);
                // Transition fields that do not read keep their defaults (as the other entry fields).
                std::string ignored;
                auto copy = entry;
                if (readEntryTransition(entryItem, copy, ignored))
                {
                    entry = copy;
                }
                playlist.entries.push_back(entry);
            }
            if (auto const* aux = item.get("aux"))
            {
                std::string ignored;
                auto copy = playlist.aux;
                if (readAux(*aux, copy, ignored))
                {
                    playlist.aux = copy;
                }
            }
            catalog_.upsertPlaylist(playlist);
            ++catalogVersion_;
        }
        continueSerial();
        result.ok = true;
        return result;
    }
    catch (ConfigError const& ex)
    {
        result.error = ex.what();
        return result;
    }
    catch (std::exception const& ex)
    {
        result.error = ex.what();
        return result;
    }
}

Route Engine::route(int camera, int phase, bool video) const
{
    std::lock_guard lock{mutex_};
    auto const it = routes_.find(std::to_string(camera) + (video ? "v" : "a") + std::to_string(phase));
    if (it == routes_.end())
    {
        return {};
    }
    return it->second;
}

std::vector<std::uint8_t> Engine::recentPreview(std::string const& key, std::chrono::milliseconds maxAge) const
{
    std::lock_guard lock{previewMutex_};
    auto const it = previews_.find(key);
    if (it == previews_.end() || (maxAge.count() > 0 && std::chrono::steady_clock::now() - it->second.made > maxAge))
    {
        return {};
    }
    return it->second.jpeg;
}

void Engine::keepPreview(std::string const& key, std::vector<std::uint8_t> const& jpeg) const
{
    std::lock_guard lock{previewMutex_};
    // Thumbnails of old IN points pile up: start over now and then.
    if (previews_.size() > 512)
    {
        previews_.clear();
    }
    previews_[key] = Preview{std::chrono::steady_clock::now(), jpeg};
}

std::vector<std::uint8_t> Engine::previewJpeg(int channel) const
{
    auto const key = "channel:" + std::to_string(channel);
    if (auto cached = recentPreview(key, std::chrono::milliseconds(150)); !cached.empty())
    {
        return cached;
    }
    Frame10 small;
    {
        // Sampling 640 pixels across is cheap; the encode runs without the lock.
        std::lock_guard lock{mutex_};
        if (channel < 1 || channel > static_cast<int>(channels_.size()))
        {
            return {};
        }
        auto const& v210 = channels_[static_cast<std::size_t>(channel - 1)].lastV210;
        if (v210.size() < v210Size(config_.format.width, config_.format.height))
        {
            return {};
        }
        small = sampleV210(v210.data(), config_.format.width, config_.format.height, 640);
    }
    auto jpeg = encodeJpeg422(small, kPreviewQuality);
    keepPreview(key, jpeg);
    return jpeg;
}

std::vector<std::uint8_t> Engine::storedPreview(int camera, std::uint64_t taiNs) const
{
    FrameRing const* ring = nullptr;
    {
        std::lock_guard lock{mutex_};
        ring = ringOf(camera);
    }
    // The ring has its own lock; the read and the decode run without the engine lock. The
    // decode is DCT-scaled (a quarter of 1080p), not a full-size decode.
    auto const stored = ring != nullptr ? ring->findNearest(taiNs) : std::nullopt;
    Frame10 small;
    if (!stored || stored->jpeg.empty() || !decodeJpegPreview(stored->jpeg.data(), stored->jpeg.size(), 384, small))
    {
        return {};
    }
    return encodeJpeg422(small, kPreviewQuality);
}

std::vector<std::uint8_t> Engine::cameraPreviewJpeg(int camera) const
{
    if (camera < 1)
    {
        return {};
    }
    auto const key = "camera:" + std::to_string(camera);
    if (auto cached = recentPreview(key, std::chrono::seconds(1)); !cached.empty())
    {
        return cached;
    }
    std::uint64_t newest = 0;
    {
        std::lock_guard lock{mutex_};
        auto const* ring = ringOf(camera);
        newest = ring != nullptr ? ring->newestNs() : 0;
    }
    auto jpeg = newest == 0 ? std::vector<std::uint8_t>{} : storedPreview(camera, newest);
    if (!jpeg.empty())
    {
        keepPreview(key, jpeg);
    }
    return jpeg;
}

std::vector<std::uint8_t> Engine::clipThumbnailJpeg(std::string const& id) const
{
    std::uint64_t frame = 0;
    int camera = 0;
    {
        // The IN frame itself, or one within a frame period; not a distant one when IN is gone.
        std::lock_guard lock{mutex_};
        auto const clip = catalog_.clip(id);
        auto const* ring = clip.id.empty() ? nullptr : ringOf(clip.camera);
        auto const nearest = ring != nullptr ? ring->nearestNs(clip.inNs) : 0;
        if (nearest == 0 || (nearest > clip.inNs ? nearest - clip.inNs : clip.inNs - nearest) > sourcePeriod(clip.camera))
        {
            return {};
        }
        frame = nearest;
        camera = clip.camera;
    }
    auto const key = "clip:" + id + ":" + std::to_string(camera) + ":" + std::to_string(frame);
    if (auto cached = recentPreview(key, std::chrono::milliseconds(0)); !cached.empty())
    {
        return cached;
    }
    auto jpeg = storedPreview(camera, frame);
    if (!jpeg.empty())
    {
        keepPreview(key, jpeg);
    }
    return jpeg;
}

void Engine::setPreviewMosaic(PreviewMosaic* mosaic)
{
    mosaic_ = mosaic;
    mosaicStep_ = static_cast<std::uint64_t>(mosaicStep(config_));
}

void Engine::setPreviewStatus(std::function<PreviewStatus()> status)
{
    previewStatus_ = std::move(status);
}

bool Engine::mosaicDue(std::uint64_t taiNs) const
{
    return timestampToIndex(config_.format.rateNum, config_.format.rateDen, taiNs) % mosaicStep_ == 0;
}

bool Engine::gpuInterpolate() const
{
    return gpu_ || config_.allowCpuInterp;
}

void Engine::setGpuPresent(bool present)
{
    gpu_ = present || cudaFlowAvailable();
}

void Engine::updateMetrics(std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock{mutex_};
    if (auto const elapsed = std::chrono::duration<double>(now - rateAt_).count(); elapsed >= 1)
    {
        writeBps_ = static_cast<double>(writtenBytes_ - rateBytes_) / elapsed;
        rateBytes_ = writtenBytes_;
        rateAt_ = now;
    }
    metrics_.set("write_bytes_per_second", {}, writeBps_);
    auto const tai = taiNowNs();
    for (std::size_t i = 0; i < cameras_.size(); ++i)
    {
        auto const& camera = cameras_[i];
        Labels const labels{{"camera", std::to_string(config_.cameras[i].index)}};
        metrics_.set("record_frames_total", labels, static_cast<double>(camera.recorded));
        metrics_.set("record_last_frame_age_seconds", labels, lastFrameAge(camera.ring.newestNs(), startedNs_, tai));
        metrics_.set("record_dropped_total", labels, static_cast<double>(camera.dropped));
        metrics_.set("phase_missing_total", labels, static_cast<double>(camera.phaseMissing));
        // Frames lost to a full disk or an I/O error (also part of record_dropped_total).
        metrics_.set("storage_write_failed_total", labels, static_cast<double>(camera.ring.writeFailures()));
        metrics_.set("protected_bytes", labels, static_cast<double>(camera.ring.protectedBytes()));
        metrics_.set("disk_bytes", labels, static_cast<double>(camera.ring.diskBytes()));
    }
    // The preview mode, and in WebRTC mode where and how the mosaic is published.
    auto const plan = previewPlan(config_);
    metrics_.set("preview_mode", {{"mode", "jpeg"}}, plan.webrtc ? 0 : 1);
    metrics_.set("preview_mode", {{"mode", "webrtc"}}, plan.webrtc ? 1 : 0);
    if (plan.webrtc)
    {
        auto const preview = previewStatus_ ? previewStatus_() : PreviewStatus{};
        metrics_.set("preview_publish_mode", {{"mode", "own"}}, plan.ownMediamtx ? 1 : 0);
        metrics_.set("preview_publish_mode", {{"mode", "shared"}}, plan.ownMediamtx ? 0 : 1);
        for (char const* state : {"connecting", "publishing", "error"})
        {
            metrics_.set("preview_publish_state", {{"stream", plan.path}, {"state", state}}, preview.state == state ? 1 : 0);
        }
        for (char const* encoder : {"nvenc", "x264"})
        {
            metrics_.set("preview_encoder", {{"encoder", encoder}}, preview.encoder == encoder ? 1 : 0);
        }
        metrics_.set("preview_frames_total", {}, static_cast<double>(preview.frames));
        if (plan.ownMediamtx)
        {
            metrics_.set("preview_mediamtx_restarts_total", {}, static_cast<double>(preview.mediamtxRestarts));
        }
    }
}

std::pair<int, std::string> Engine::admitUpload(std::uint64_t bytes) const
{
    auto const cap = static_cast<std::uint64_t>(config_.uploadMaxMb) * 1024 * 1024;
    if (cap > 0 && bytes > cap)
    {
        return {413, "the upload is larger than " + std::to_string(config_.uploadMaxMb) + " MB (REPLAY_UPLOAD_MAX_MB)"};
    }
    // The body, its JPEG frames (estimated), what the camera buffers still grow into, and a reserve:
    // an upload must not take the disk the recording needs.
    std::uint64_t cameraBytes = 0;
    for (auto const& camera : config_.cameras)
    {
        cameraBytes += segmentBytes(config_.storageDir + "/cam" + std::to_string(camera.index));
    }
    auto const cameraGrowth = cameraBudgetBytes_ > cameraBytes ? cameraBudgetBytes_ - cameraBytes : 0;
    auto const need = bytes * (1 + kUploadExpansion) + cameraGrowth + kUploadReserveBytes;
    auto const free = freeBytes();
    if (free < need)
    {
        auto const gb = [](std::uint64_t value) { return std::to_string(value / 1000000000ull) + "." + std::to_string(value / 100000000ull % 10); };
        return {507, "not enough free storage for this upload: about " + gb(need) + " GB needed (the file, its frames, the camera buffers' growth, a reserve), " +
                         gb(free) + " GB free"};
    }
    return {0, {}};
}

std::uint64_t Engine::freeBytes() const
{
    struct statvfs st{};
    if (statvfs(config_.storageDir.c_str(), &st) != 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_frsize);
}

Catalog::Playlist const& Engine::currentPlaylist(int channel)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto const key = runtime.playlistId + "#" + std::to_string(catalogVersion_);
    if (key != runtime.planKey)
    {
        runtime.planKey = key;
        runtime.plan = runtime.playlistId.empty() ? Catalog::Playlist{} : findPlaylist(catalog_, runtime.playlistId);
        runtime.planLengths.clear();
        runtime.planMaxTransition = 0;
        for (auto const& entry : runtime.plan.entries)
        {
            auto const clip = catalog_.clip(entry.clipId);
            runtime.planLengths.push_back(clip.id.empty() ? -1 : remainingNs(static_cast<std::int64_t>(clip.inNs), clip.outNs, entry.speed));
            runtime.planMaxTransition = std::max(runtime.planMaxTransition, transitionPlan(entry).length());
        }
        runtime.planAuxLength = runtime.plan.aux.source.empty() ? 0 : auxLength(runtime.plan.aux.source);
    }
    return runtime.plan;
}

void Engine::maybeStartTransition(int channel)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    if (runtime.mix.active || runtime.liveMode || runtime.black || !runtime.scheduler.playing || runtime.playlistId.empty() || runtime.clipId.empty() ||
        !runtime.hasOut)
    {
        return;
    }
    auto const period = runtime.scheduler.framePeriodNs;
    double const speed = runtime.scheduler.speed;
    auto const position = runtime.scheduler.positionNs;
    auto const out = static_cast<std::int64_t>(runtime.outNs);
    if (speed < kMinClipSpeed || period <= 0 || position >= out)
    {
        return; // at OUT finishClip cuts
    }
    // The grains that still show the outgoing entry, this one included.
    auto const left = static_cast<std::int64_t>(std::ceil(static_cast<double>(out - position) / (speed * static_cast<double>(period))));
    if (left > kMaxTransitionFrames)
    {
        return;
    }
    // Nothing to look up in a playlist without transitions, or long before its longest one.
    (void)currentPlaylist(channel);
    if (left > runtime.planMaxTransition)
    {
        return;
    }
    auto const list = runtime.plan;
    int const index = runtime.playlistIndex;
    if (list.id.empty() || index < 0 || index >= static_cast<int>(list.entries.size()))
    {
        return;
    }
    auto const& entry = list.entries[static_cast<std::size_t>(index)];
    auto const exists = [&](int i) { return !catalog_.clip(list.entries[static_cast<std::size_t>(i)].clipId).id.empty(); };
    // The entry finishClip plays next: the idle playlist plays every entry on; otherwise `next` (or a looped
    // playlist's wrap) with auto-advance.
    int next = -1;
    if (runtime.idle)
    {
        std::vector<bool> present;
        for (int i = 0; i < static_cast<int>(list.entries.size()); ++i)
        {
            present.push_back(exists(i));
        }
        next = idleNext(present, index);
    }
    else
    {
        int later = playlistNext(list.entries, index);
        while (later >= 0 && !exists(later))
        {
            later = playlistNext(list.entries, later);
        }
        bool const wraps = list.loop && later < 0 && (entry.end == EndAction::Freeze || entry.end == EndAction::Black);
        bool const advances = (entry.end == EndAction::Next && entry.autoAdvance) || wraps;
        if (!advances)
        {
            return;
        }
        next = later;
        if (next < 0 && list.loop)
        {
            ClipRef first;
            next = firstPlayable(catalog_, list, first);
        }
    }
    if (next < 0)
    {
        return;
    }
    auto const& incoming = list.entries[static_cast<std::size_t>(next)];
    auto plan = transitionPlan(incoming);
    // A transition must complete before the incoming entry reaches its own OUT (finishClip there would cut the
    // mix off mid-crossfade): it takes at most the incoming entry's length less the default cut crossfade (so the
    // join after it still gets one), and at most the outgoing grains left when it starts late.
    if (auto const nextClip = catalog_.clip(incoming.clipId); !nextClip.id.empty() && nextClip.outNs > nextClip.inNs)
    {
        double const nextSpeed = std::max(kMinClipSpeed, incoming.speed);
        auto const frames = static_cast<int>(std::floor(static_cast<double>(nextClip.outNs - nextClip.inNs) / (nextSpeed * static_cast<double>(period))));
        int const most = std::max(1, std::min(frames - kDefaultCutAudioFrames, static_cast<int>(left)));
        plan.videoFrames = std::min(plan.videoFrames, most);
        plan.audioFrames = std::min(plan.audioFrames, most);
    }
    if (plan.length() == 0 || left > plan.length())
    {
        return;
    }
    auto const& cfg = config_.channelList[static_cast<std::size_t>(channel - 1)];
    ChannelRuntime::Mix mix;
    mix.scheduler = runtime.scheduler;
    mix.outNs = runtime.outNs;
    mix.videoFrames = plan.videoFrames;
    mix.audioFrames = plan.audioFrames;
    mix.audioTotal = static_cast<std::int64_t>(std::llround(static_cast<double>(plan.audioFrames) * 48000.0 * config_.format.rateDen / config_.format.rateNum));
    mix.ownAudio = runtime.ownAudio;
    mix.audio = cfg.audio;
    mix.audioNs = runtime.audioNs;
    auto const outgoingClip = runtime.clipId;
    finishClip(channel);
    if (runtime.playlistIndex != next || !runtime.scheduler.playing || runtime.liveMode)
    {
        return;
    }
    mix.active = true;
    runtime.mix = mix;
    metrics_.inc("transitions_total", {{"channel", std::to_string(channel)}, {"kind", plan.videoFrames > 0 ? "mix" : "cut"}});
    logInfo("transition", {{"channel", std::to_string(channel)}, {"playlist", runtime.playlistId}, {"from", outgoingClip}, {"to", runtime.clipId},
                              {"frames", std::to_string(plan.videoFrames)}, {"audio_frames", std::to_string(plan.audioFrames)}});
}

std::string Engine::audioItemPath(std::string const& id) const
{
    return libraryDir_ + "/audio/" + id + ".f32";
}

std::int64_t Engine::auxLength(std::string const& source) const
{
    if (source.rfind(kAudioItemPrefix, 0) == 0)
    {
        return catalog_.audioItem(source).samples;
    }
    auto const clip = catalog_.clip(source);
    if (clip.id.empty() || clip.outNs < clip.inNs)
    {
        return 0;
    }
    auto const period = static_cast<std::uint64_t>(framePeriodNs(config_.format.rateNum, config_.format.rateDen));
    auto const first = clip.inNs / period;
    auto const frames = (clip.outNs - clip.inNs) / period + 1;
    return static_cast<std::int64_t>(audioSamplesUntil(first + frames, config_.format.rateNum, config_.format.rateDen) -
                                     audioSamplesUntil(first, config_.format.rateNum, config_.format.rateDen));
}

std::vector<float> Engine::readAuxSamples(std::string const& source, ClipRef const& clip, std::int64_t position, std::int64_t count) const
{
    // Called without the engine lock: the caller read the clip from the catalog; the rings have their own lock.
    std::vector<float> out(static_cast<std::size_t>(std::max<std::int64_t>(0, count)) * 2, 0.f);
    if (count <= 0 || position < 0)
    {
        return out;
    }
    if (source.rfind(kAudioItemPrefix, 0) == 0)
    {
        auto const path = audioItemPath(source);
        int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            auto const bytes = static_cast<std::size_t>(count) * 2 * sizeof(float);
            auto const got = ::pread(fd, out.data(), bytes, static_cast<off_t>(position) * 2 * static_cast<off_t>(sizeof(float)));
            (void)got; // a short read leaves zeros
            ::close(fd);
        }
        return out;
    }
    auto const* ring = clip.id.empty() ? nullptr : ringOf(clip.camera);
    if (ring == nullptr)
    {
        return out;
    }
    // The clip's frames carry the audio of their house frame (audioSamplesForFrame); sample frame `s` of the clip
    // is in clip frame f with U(f) <= s < U(f+1), U counted from the clip's IN frame.
    auto const num = config_.format.rateNum;
    auto const den = config_.format.rateDen;
    auto const period = static_cast<std::uint64_t>(framePeriodNs(num, den));
    auto const first = clip.inNs / period;
    auto const until = [&](std::int64_t f) {
        return static_cast<std::int64_t>(audioSamplesUntil(first + static_cast<std::uint64_t>(f), num, den) - audioSamplesUntil(first, num, den));
    };
    auto const frames = static_cast<std::int64_t>((clip.outNs - clip.inNs) / period + 1);
    auto f = static_cast<std::int64_t>(static_cast<double>(position) * num / (48000.0 * den));
    while (f > 0 && until(f) > position)
    {
        --f;
    }
    while (until(f + 1) <= position)
    {
        ++f;
    }
    std::int64_t done = 0;
    while (done < count && f < frames)
    {
        auto const start = until(f);
        auto const size = until(f + 1) - start;
        auto const audio = ring->findNearestAudio(clip.inNs + static_cast<std::uint64_t>(f) * period);
        for (auto s = position + done - start; s < size && done < count; ++s, ++done)
        {
            auto const i = static_cast<std::size_t>(s) * 2;
            auto const o = static_cast<std::size_t>(done) * 2;
            out[o] = i < audio.size() ? audio[i] : 0.f;
            out[o + 1] = i + 1 < audio.size() ? audio[i + 1] : 0.f;
        }
        ++f;
    }
    return out;
}

void Engine::mixAux(int channel, std::unique_lock<std::recursive_mutex>& lock, std::vector<float>& pcm, bool still, bool ownAudible, std::uint64_t outputTaiNs)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto const& list = currentPlaylist(channel);
    bool const onList = !runtime.liveMode && !runtime.playlistId.empty() && !list.id.empty() && !list.aux.source.empty();
    if (onList && (runtime.auxPlaylist != list.id || runtime.auxRestart || runtime.aux.source != list.aux.source))
    {
        // A playlist that starts (or another aux source): the aux starts at its beginning, faded in.
        runtime.auxPlaylist = list.id;
        runtime.auxPosition = 0;
        runtime.auxRun = 0;
        runtime.duck = {};
    }
    runtime.auxRestart = false;
    if (onList)
    {
        runtime.aux = list.aux;
    }
    bool const wanted = onList && !still;
    if ((!wanted && runtime.auxRun <= 0) || runtime.aux.source.empty())
    {
        runtime.auxRun = 0;
        runtime.auxGainDb = -120;
        if (!onList)
        {
            runtime.auxPlaylist.clear();
        }
        return;
    }
    auto const aux = runtime.aux;
    auto const length = aux.source == list.aux.source ? runtime.planAuxLength : auxLength(aux.source);
    auto const clip = aux.source.rfind(kAudioItemPrefix, 0) == 0 ? ClipRef{} : catalog_.clip(aux.source);
    // The time to the playlist's end (its countdown) for the end fade: none when it does not end. A looped or
    // idle playlist does not end: its countdown is only the end of this pass (no fade, no jump back at the wrap).
    std::int64_t remaining = -1;
    if (aux.fadeOutFrames > 0 && onList && !list.loop && !runtime.idle)
    {
        std::int64_t clipLeft = -1;
        remaining = countdowns(runtime, list, runtime.planLengths, clipLeft);
    }
    auto const fadeNs = static_cast<std::int64_t>(aux.fadeOutFrames) * framePeriodNs(config_.format.rateNum, config_.format.rateDen);
    auto const frames = static_cast<std::int64_t>(pcm.size() / 2);
    auto position = runtime.auxPosition;
    lock.unlock();
    // The samples of this grain, from the source's start again when it loops.
    std::vector<float> samples(pcm.size(), 0.f);
    std::vector<std::int64_t> where(static_cast<std::size_t>(frames), -1);
    std::int64_t done = 0;
    while (done < frames && length > 0 && (position < length || aux.loop))
    {
        if (position >= length)
        {
            position = 0;
        }
        auto const n = std::min(frames - done, length - position);
        auto const part = readAuxSamples(aux.source, clip, position, n);
        std::copy(part.begin(), part.end(), samples.begin() + done * 2);
        for (std::int64_t i = 0; i < n; ++i)
        {
            where[static_cast<std::size_t>(done + i)] = position + i;
        }
        done += n;
        position += n;
    }
    lock.lock();
    auto& rt = channels_[static_cast<std::size_t>(channel - 1)];
    double const level = dbToGain(aux.levelDb);
    double const runStep = 1.0 / (2.0 * static_cast<double>(std::max<std::int64_t>(1, frames)));
    double const attack = aux.attackMs * 48.0;
    double const release = aux.releaseMs * 48.0;
    double duckGain = 1;
    double end = 1;
    for (std::int64_t i = 0; i < frames; ++i)
    {
        rt.auxRun = wanted ? std::min(1.0, rt.auxRun + runStep) : std::max(0.0, rt.auxRun - runStep);
        duckGain = rt.duck.next(ownAudible, aux.duckDb, attack, release);
        end = endFadeGain(remaining < 0 ? -1 : std::max<std::int64_t>(0, remaining - i * 1000000000 / 48000), fadeNs);
        auto const at = where[static_cast<std::size_t>(i)];
        double const edge = at < 0 ? 0.0 : edgeGain(at, length);
        auto const gain = static_cast<float>(level * duckGain * end * edge * rt.auxRun);
        pcm[static_cast<std::size_t>(2 * i)] += samples[static_cast<std::size_t>(2 * i)] * gain;
        pcm[static_cast<std::size_t>(2 * i + 1)] += samples[static_cast<std::size_t>(2 * i + 1)] * gain;
    }
    rt.auxPosition = aux.loop && length > 0 ? position % length : std::min(position, length);
    double const steady = level * duckGain * end;
    rt.auxGainDb = steady > 1e-6 ? 20.0 * std::log10(steady) : -120;
    (void)outputTaiNs;
}

std::int64_t Engine::countdowns(ChannelRuntime const& channel, Catalog::Playlist const& list, std::int64_t& clipLeft) const
{
    std::vector<std::int64_t> lengths;
    for (auto const& entry : list.entries)
    {
        auto const clip = catalog_.clip(entry.clipId);
        lengths.push_back(clip.id.empty() ? -1 : remainingNs(static_cast<std::int64_t>(clip.inNs), clip.outNs, entry.speed));
    }
    return countdowns(channel, list, lengths, clipLeft);
}

std::int64_t Engine::countdowns(ChannelRuntime const& channel, Catalog::Playlist const& list, std::vector<std::int64_t> const& lengths, std::int64_t& clipLeft) const
{
    clipLeft = -1;
    std::int64_t listLeft = -1;
    bool const onClip = !channel.liveMode && !channel.clipId.empty() && channel.hasOut;
    if (onClip)
    {
        clipLeft = channel.shot.state == ShotState::Ended || channel.black ? 0 : remainingNs(channel.scheduler.positionNs, channel.outNs, channel.scheduler.targetSpeed);
    }
    if (onClip && !list.id.empty())
    {
        auto entries = list.entries;
        for (auto& entry : entries)
        {
            // The idle playlist plays every entry on.
            if (channel.idle)
            {
                entry.end = EndAction::Next;
            }
        }
        // An ended shot (stopped, or the playlist's last clip at its end) has nothing left.
        listLeft = clipLeft == 0 && channel.shot.state == ShotState::Ended
                       ? 0
                       : playlistRemainingNs(entries, lengths, channel.playlistIndex, clipLeft, framePeriodNs(config_.format.rateNum, config_.format.rateDen));
    }
    return listLeft;
}

std::string Engine::statusJson() const
{
    std::lock_guard lock{mutex_};
    std::ostringstream out;
    auto const period = framePeriodNs(config_.format.rateNum, config_.format.rateDen);
    auto const now = taiNowNs();
    auto const tc = [&](std::int64_t ns) {
        return jsonString(formatTimecode(static_cast<std::uint64_t>(std::max<std::int64_t>(0, ns)), config_.format.rateNum, config_.format.rateDen));
    };
    auto const routeState = [&](int camera, int phase, bool video) {
        auto const it = routes_.find(std::to_string(camera) + (video ? "v" : "a") + std::to_string(phase));
        return jsonString(it == routes_.end() ? std::string{} : it->second.state);
    };
    out << "{\"version\":\"" << REPLAY_VERSION << "\",\"mxl\":\"" << REPLAY_MXL_REVISION << "\",\"nmos_cpp\":\"" << REPLAY_NMOS_CPP_REVISION
        << "\",\"label\":" << jsonString(deviceLabel(config_)) << ",\"tai_ns\":" << now << ",\"frame_ns\":" << period
        << ",\"format\":" << jsonString(config_.format.token()) << ",\"gpu\":" << (gpuInterpolate() ? "true" : "false")
        << ",\"flow\":" << jsonString(flowModuleName(config_.flowModule)) << ",\"preset\":" << jsonString(presetName(config_.preset))
        << ",\"storage_bps\":" << storageBps_ << ",\"free_bytes\":" << freeBytes() << ",\"jpeg\":\"" << jpegRuntimeBackend()
        << "\",\"jpeg_bit_depth\":" << jpegStorageBitDepth() << ",\"tsl_enable\":" << (config_.tslEnable ? "true" : "false")
        << ",\"cameras\":[";
    for (std::size_t i = 0; i < cameras_.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        auto const& camera = cameras_[i];
        auto const& cfg = config_.cameras[i];
        out << "{\"index\":" << cfg.index << ",\"label\":" << jsonString(cfg.label) << ",\"colour\":" << jsonString(cfg.colour)
            << ",\"record\":" << (cfg.record ? "true" : "false") << ",\"phases\":" << cfg.phases << ",\"hfr_factor\":" << hfrFactor(cfg.index)
            << ",\"frames\":" << camera.ring.size() << ",\"dropped\":" << camera.dropped << ",\"phase_missing\":" << camera.phaseMissing
            << ",\"scaled\":" << (camera.scaled ? "true" : "false") << ",\"protected_bytes\":" << camera.ring.protectedBytes()
            << ",\"disk_bytes\":" << camera.ring.diskBytes() << ",\"segments\":" << camera.ring.segmentCount();
        // The buffer on the timeline; recording: a frame within the last second.
        auto const newest = camera.ring.newestNs();
        out << ",\"oldest_ns\":" << camera.ring.oldestNs() << ",\"newest_ns\":" << newest << ",\"buffer_hours\":" << cfg.bufferHours
            << ",\"recording\":" << (cfg.record && newest != 0 && newest + 1000000000ull >= now ? "true" : "false")
            << ",\"last_frame_age_s\":" << lastFrameAge(newest, startedNs_, now)
            << ",\"audio_record\":" << (cfg.audio ? "true" : "false") << ",\"video_states\":[";
        for (int phase = 1; phase <= cfg.phases; ++phase)
        {
            out << (phase > 1 ? "," : "") << routeState(cfg.index, phase, true);
        }
        out << "],\"audio_state\":" << routeState(cfg.index, 1, false) << '}';
    }
    out << "],\"channels\":[";
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        auto const& channel = channels_[i];
        auto const& cfg = config_.channelList[i];
        out << "{\"index\":" << cfg.index << ",\"label\":" << jsonString(cfg.label) << ",\"live\":" << (channel.liveMode ? "true" : "false")
            << ",\"playing\":" << (channel.scheduler.playing ? "true" : "false") << ",\"speed\":" << channel.scheduler.speed
            << ",\"target_speed\":" << channel.scheduler.targetSpeed
            << ",\"position_ns\":" << channel.scheduler.positionNs << ",\"camera\":" << channel.scheduler.camera
            << ",\"motion\":" << jsonString(motionName(cfg.motion)) << ",\"audio\":" << jsonString(audioModeName(cfg.audio))
            << ",\"shot\":" << jsonString(shotStateName(channel.shot.state)) << ",\"clip\":" << jsonString(channel.clipId)
            << ",\"in_ns\":" << channel.inNs << ",\"out_ns\":" << channel.outNs << ",\"lock\":" << (cfg.lockToFirst ? "true" : "false")
            << ",\"has_in\":" << (channel.hasIn ? "true" : "false") << ",\"has_out\":" << (channel.hasOut ? "true" : "false")
            << ",\"black\":" << (channel.black ? "true" : "false") << ",\"timecode\":" << jsonString(channel.timecode)
            << ",\"tc_mode\":" << jsonString(tcModeName(cfg.timecode)) << ",\"position_tc\":" << tc(channel.scheduler.positionNs)
            << ",\"in_tc\":" << tc(static_cast<std::int64_t>(channel.inNs)) << ",\"out_tc\":" << tc(static_cast<std::int64_t>(channel.outNs))
            << ",\"shot_id\":" << jsonString(channel.shot.clipId) << ",\"playlist\":" << jsonString(channel.playlistId)
            << ",\"playlist_index\":" << channel.playlistIndex << ",\"play_on_tally\":" << (cfg.playOnTally ? "true" : "false")
            << ",\"tsl_lh\":" << channel.tslLh << ",\"tsl_rh\":" << channel.tslRh << ",\"tsl_text_tally\":" << channel.tslText
            << ",\"tally\":" << effectiveTally(channel.tslLh, channel.tslRh, channel.tslText) << ",\"tally_play_ns\":" << channel.tallyPlayNs
            << ",\"idle_playlist\":" << jsonString(cfg.idlePlaylist) << ",\"idle\":" << (channel.idle ? "true" : "false");
        // Shotbox countdowns (SPECIFICATION.md §8.1): playing time to OUT and to the playlist's end at the speed set; null
        // without a clip, or when the speed never gets there.
        std::int64_t clipLeft = -1;
        bool const onClip = !channel.liveMode && !channel.clipId.empty() && channel.hasOut;
        auto const list = onClip && !channel.playlistId.empty() ? findPlaylist(catalog_, channel.playlistId) : Catalog::Playlist{};
        std::int64_t const listLeft = countdowns(channel, list, clipLeft);
        // A transition into the current entry (1.6.0) and the aux track.
        out << ",\"own_audio\":" << (channel.ownAudio ? "true" : "false") << ",\"transition\":";
        if (channel.mix.active)
        {
            out << "{\"kind\":\"" << (channel.mix.videoFrames > 0 ? "mix" : "cut") << "\",\"frames\":" << channel.mix.videoFrames
                << ",\"audio_frames\":" << channel.mix.audioFrames << ",\"done\":" << channel.mix.done << '}';
        }
        else
        {
            out << "null";
        }
        out << ",\"aux\":";
        if (!channel.auxPlaylist.empty() && channel.auxRun > 0 && !channel.aux.source.empty())
        {
            out << "{\"source\":" << jsonString(channel.aux.source) << ",\"gain_db\":" << std::round(channel.auxGainDb * 10) / 10
                << ",\"ducked\":" << (channel.duck.db < -0.05 ? "true" : "false") << ",\"position_ns\":" << channel.auxPosition * 1000000000 / 48000 << '}';
        }
        else
        {
            out << "null";
        }
        auto const nsOrNull = [](std::int64_t ns) { return ns < 0 ? std::string("null") : std::to_string(ns); };
        out << ",\"clip_remaining_ns\":" << nsOrNull(clipLeft) << ",\"playlist_remaining_ns\":" << nsOrNull(listLeft)
            << ",\"playlist_loop\":" << (list.loop ? "true" : "false") << '}';
    }
    out << "],\"preview\":" << previewStatusJson(config_, previewStatus_ ? previewStatus_() : PreviewStatus{}) << '}';
    return out.str();
}
} // namespace replay
