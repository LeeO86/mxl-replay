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
#include "util/logging.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
} // namespace

Engine::CameraRuntime::CameraRuntime(std::string directory, std::uint64_t retentionNs, int segmentSeconds)
    : ring(std::move(directory), retentionNs, segmentSeconds)
{
}

Engine::Engine(Config config, std::map<std::string, std::string> settings)
    : config_(std::move(config))
    , settings_(std::move(settings))
    , ids_(makeNmosIds(config_.nmosSeed))
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
    for (auto const& camera : config_.cameras)
    {
        int const fpsNum = camera.nativeFps > 0 ? camera.nativeFps : config_.format.rateNum;
        int const fpsDen = camera.nativeFps > 0 ? 1 : config_.format.rateDen;
        required += bytesPerHourEstimate(config_.format.width, config_.format.height, fpsNum, fpsDen, config_.jpegQuality) * camera.bufferHours;
        required += 48000.0 * 2.0 * 4.0 * 3600.0 * camera.bufferHours;
    }
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
        (void)channel;
        channels_.push_back(std::move(runtime));
    }
    if (settings_.empty())
    {
        std::istringstream lines(exportKeyValue(config_));
        std::string line;
        while (std::getline(lines, line))
        {
            auto const eq = line.find('=');
            if (eq != std::string::npos)
            {
                settings_[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
    }
    logInfo("replay_ready", {{"storage", config_.storageDir}, {"state", config_.stateDir}, {"inputs", std::to_string(config_.inputs)},
        {"channels", std::to_string(config_.channels)}});
}

Engine::~Engine() = default;

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
    // Clips keep their frames: protect them again before old segments expire.
    for (auto const& clip : catalog_.clips())
    {
        if (auto* ring = ringOf(clip.camera))
        {
            ring->protect(clip.inNs, clip.outNs);
        }
    }
    for (auto& camera : cameras_)
    {
        camera.ring.enforceRetention();
    }
    library_->enforceRetention();
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
    // Ids end in "-<serial>" (clip-, upload-, group-, playlist-). Continue after the catalog's:
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
    for (auto& frame : frames)
    {
        recorded += camera.ring.push(std::move(frame)) ? 1 : 0;
    }
    std::lock_guard lock{mutex_};
    camera.recorded += recorded;
    camera.dropped += frames.size() - recorded;
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

bool Engine::v210At(int camera, std::uint64_t taiNs, std::vector<std::uint8_t>& v210, bool* found) const
{
    *found = false;
    auto const* ring = ringOf(camera);
    if (ring == nullptr)
    {
        return false;
    }
    auto const stored = ring->findNearest(taiNs);
    if (!stored)
    {
        return false;
    }
    *found = true;
    v210.resize(v210Size(config_.format.width, config_.format.height));
    return decodeJpegToV210(stored->jpeg.data(), stored->jpeg.size(), config_.format.width, config_.format.height, 0, v210.data());
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
    }
    if (!runtime.clipId.empty() && runtime.hasOut && runtime.scheduler.positionNs >= static_cast<std::int64_t>(runtime.outNs) && runtime.scheduler.playing)
    {
        finishClip(channel);
    }
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
    bool exact = pick.kind == SourcePick::Kind::Exact;
    bool renderedOnGpu = false;
    if (auto const* ring = ringOf(camera); !runtime.black && cudaFlowAvailable() && ring != nullptr)
    {
        // Read, decode, interpolate and download without the engine lock, so channels and
        // the recorder do not wait for each other's disk or GPU work.
        auto const op = operatingPoint(config_.preset);
        bool const wantB = pick.kind != SourcePick::Kind::Exact && pick.kind != SourcePick::Kind::Repeat;
        lock.unlock();
        auto const storedA = ring->findNearest(taiA);
        auto const storedB = wantB ? ring->findNearest(taiB) : std::nullopt;
        bool const second = storedB && storedA && storedB->taiNs != storedA->taiNs;
        GpuPicture gpuPicture;
        auto const key = std::to_string(camera) + ":" + std::to_string(taiA) + ":" + std::to_string(taiB);
        bool onGpu = false;
        if (storedA && !storedA->jpeg.empty())
        {
            auto const keyA = std::to_string(camera) + ":" + std::to_string(storedA->taiNs);
            auto const keyB = second ? std::to_string(camera) + ":" + std::to_string(storedB->taiNs) : std::string{};
            onGpu = gpuRenderFromJpeg(storedA->jpeg.data(), storedA->jpeg.size(), keyA, second ? storedB->jpeg.data() : nullptr, second ? storedB->jpeg.size() : 0,
                keyB, static_cast<float>(pick.phase), motion == MotionMode::Interpolate && pick.kind == SourcePick::Kind::Interpolate, op, key, config_.format.width,
                config_.format.height, gpuPicture);
        }
        lock.lock();
        if (onGpu)
        {
            rendered.v210 = std::move(gpuPicture.v210);
            runtime.lastV210 = rendered.v210;
            renderedOnGpu = true;
            if (motion == MotionMode::Interpolate && pick.kind == SourcePick::Kind::Interpolate)
            {
                metrics_.inc("flow_cache_hits_total", {{"channel", std::to_string(channel)}});
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
    if (renderedOnGpu)
    {
        foundA = true;
    }
    else
    {
        bool const needB = pick.kind != SourcePick::Kind::Exact && pick.kind != SourcePick::Kind::Repeat && std::fabs(pick.phase) >= 1e-6;
        bool const black = runtime.black;
        lock.unlock();
        bool decoded = false;
        if (!needB && !black)
        {
            decoded = v210At(camera, taiA, rendered.v210, &foundA);
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
        auto const key = std::to_string(camera) + ":" + std::to_string(taiA) + ":" + std::to_string(taiB);
        if (flowCache_.count(key) == 0)
        {
            auto const op = operatingPoint(config_.preset);
            flowCache_[key] = computeFlowPair(grayFromLuma(toFloat(frameA)), grayFromLuma(toFloat(frameB)), op);
            metrics_.inc("flow_cache_miss_total", {{"channel", std::to_string(channel)}});
        }
        else
        {
            metrics_.inc("flow_cache_hits_total", {{"channel", std::to_string(channel)}});
        }
        auto const& pair = flowCache_[key];
        auto const op = operatingPoint(config_.preset);
        picture = fromFloat(interpolateFrames(toFloat(frameA), toFloat(frameB), pair.forward, pair.backward, static_cast<float>(pick.phase), op));
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
    rendered.motion = motionName(exact && motion == MotionMode::Interpolate ? MotionMode::Repeat : motion);
    rendered.exact = exact;
    rendered.camera = camera;
    auto const tcTai = cfg.timecode == TcMode::Source ? position : outputTaiNs;
    auto const tc = timecodeFromTai(tcTai, config_.format.rateNum, config_.format.rateDen, false);
    rendered.timecode = tc.format();
    runtime.timecode = rendered.timecode;
    AncPacket packet;
    packet.tc = tc;
    packet.sequence = static_cast<std::uint16_t>(runtime.ancSequence++);
    rendered.anc = encodeAncGrain(packet);

    int const samples = audioSamplesForFrame(timestampToIndex(config_.format.rateNum, config_.format.rateDen, outputTaiNs), config_.format.rateNum,
        config_.format.rateDen, 48000);
    int const audioCamera = cfg.atmosCamera > 0 ? cfg.atmosCamera : camera;
    std::vector<float> pcm(static_cast<std::size_t>(samples) * 2, 0.f);
    if (auto const* ring = ringOf(audioCamera))
    {
        // Audio is stored with the frames on house times (phase 1 of an HFR camera).
        auto const house = static_cast<std::uint64_t>(std::max<std::int64_t>(1, period));
        auto const at = (position + house / 2) / house * house;
        lock.unlock();
        auto audio = ring->findNearestAudio(at);
        lock.lock();
        if (!audio.empty())
        {
            audio.resize(pcm.size(), 0.f);
            pcm = std::move(audio);
        }
    }
    double const speed = std::fabs(runtime.scheduler.speed);
    if (!runtime.liveMode && speed < 0.999)
    {
        if (cfg.audio == AudioMode::Mute)
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
        else if (cfg.audio == AudioMode::Stretch && speed > 0.01)
        {
            pcm = timeStretch(pcm, 2, speed).samples;
            pcm.resize(static_cast<std::size_t>(samples) * 2, 0.f);
        }
        else if (cfg.audio == AudioMode::Follow && speed > 0.01)
        {
            pcm = resampleLinear(pcm, 2, speed);
            pcm.resize(static_cast<std::size_t>(samples) * 2, 0.f);
        }
    }
    else
    {
        runtime.fadeFramesLeft = 0;
    }
    rendered.audio = std::move(pcm);
    metrics_.set("channel_speed", {{"channel", std::to_string(channel)}}, runtime.scheduler.speed);
    metrics_.set("channel_state", {{"channel", std::to_string(channel)}}, runtime.liveMode ? 0 : runtime.scheduler.playing ? 1 : 2);
    return rendered;
}

void Engine::finishClip(int channel)
{
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    auto clip = catalog_.clip(runtime.clipId);
    // A playlist entry has its own end action and auto-advance.
    auto const list = runtime.playlistId.empty() ? Catalog::Playlist{} : findPlaylist(catalog_, runtime.playlistId);
    PlaylistEntry const* entry = nullptr;
    if (runtime.playlistIndex >= 0 && runtime.playlistIndex < static_cast<int>(list.entries.size()))
    {
        entry = &list.entries[static_cast<std::size_t>(runtime.playlistIndex)];
    }
    auto const action = entry != nullptr ? entry->end : clip.id.empty() ? runtime.shot.end : clip.end;
    runtime.shot.end = action;
    runtime.shot.finished();
    if (action == EndAction::Loop)
    {
        runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.inNs);
        runtime.scheduler.playing = true;
        runtime.shot.state = ShotState::Playing;
    }
    else if (action == EndAction::Black)
    {
        runtime.scheduler.playing = false;
        runtime.black = true;
    }
    else if (action == EndAction::ReturnToLive)
    {
        runtime.liveMode = true;
        runtime.scheduler.playing = false;
        runtime.black = false;
        runtime.clipId.clear();
        runtime.playlistId.clear();
        runtime.playlistIndex = -1;
    }
    else if (action == EndAction::Next && entry != nullptr)
    {
        // The next entry whose clip still exists; auto-advance plays it, otherwise it is cued.
        bool const autoAdvance = entry->autoAdvance;
        int next = playlistNext(list.entries, runtime.playlistIndex);
        ClipRef nextClip;
        while (next >= 0 && (nextClip = catalog_.clip(list.entries[static_cast<std::size_t>(next)].clipId)).id.empty())
        {
            next = playlistNext(list.entries, next);
        }
        if (next < 0)
        {
            runtime.scheduler.playing = false;
            runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.outNs);
        }
        else
        {
            runtime.playlistIndex = next;
            cueClip(channel, nextClip, &list.entries[static_cast<std::size_t>(next)]);
            runtime.scheduler.playing = autoAdvance;
            runtime.shot.state = autoAdvance ? ShotState::Playing : ShotState::Cued;
        }
    }
    else
    {
        runtime.scheduler.playing = false;
        runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.outNs);
    }
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
    runtime.liveMode = true;
    runtime.black = false;
    runtime.scheduler.playing = false;
    // Uploads have no live picture: after one, live follows the first camera.
    if (runtime.scheduler.camera == kLibraryCamera)
    {
        runtime.scheduler.camera = 1;
    }
    runtime.clipId.clear();
    runtime.playlistId.clear();
    runtime.playlistIndex = -1;
    runtime.shot.state = ShotState::Idle;
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
    std::uint64_t budget = 0;
    std::uint64_t protectedBytes = 0;
    for (std::size_t i = 0; i < cameras_.size(); ++i)
    {
        double const fps = static_cast<double>(config_.format.rateNum) / static_cast<double>(config_.format.rateDen) * hfrFactor(static_cast<int>(i + 1));
        budget += static_cast<std::uint64_t>(bytesPerFrameEstimate(config_.format.width, config_.format.height, config_.jpegQuality) * fps * 3600.0 *
                                              config_.cameras[i].bufferHours);
        protectedBytes += cameras_[i].ring.protectedBytes();
    }
    if (budget > 0 && (100.0 * static_cast<double>(protectedBytes) / static_cast<double>(budget)) >= config_.protectMaxPct && !force)
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
        // The speed the operator set (a ramp only runs while playing).
        clip.speed = runtime.scheduler.targetSpeed;
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

bool Engine::updateClip(ClipRef clip)
{
    std::lock_guard lock{mutex_};
    auto const existing = clip.id.empty() ? ClipRef{} : catalog_.clip(clip.id);
    if (existing.id.empty())
    {
        return false;
    }
    if (clip.name.empty())
    {
        clip.name = existing.name;
    }
    // A new IN or OUT lands on the nearest recorded frame (an index lookup, no read).
    if (auto const* ring = ringOf(clip.camera))
    {
        auto const period = sourcePeriod(clip.camera);
        auto const snap = [&](std::uint64_t ns) {
            auto const nearest = ring->nearestNs(ns);
            return nearest != 0 && (nearest > ns ? nearest - ns : ns - nearest) <= period ? nearest : ns;
        };
        clip.inNs = clip.inNs == existing.inNs ? clip.inNs : snap(clip.inNs);
        clip.outNs = clip.outNs == existing.outNs ? clip.outNs : snap(clip.outNs);
    }
    catalog_.upsertClip(clip);
    // The protected range moves with IN and OUT.
    if (existing.camera != clip.camera || existing.inNs != clip.inNs || existing.outNs != clip.outNs)
    {
        if (auto* ring = ringOf(existing.camera))
        {
            ring->unprotect(existing.inNs, existing.outNs);
        }
        if (auto* ring = ringOf(clip.camera))
        {
            ring->protect(clip.inNs, clip.outNs);
        }
    }
    return true;
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
        ring = ringOf(clip.camera);
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
    runtime.fadeFramesLeft = 0;
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
        runtime.scheduler.playing = state == ShotState::Playing;
    }
    return runtime.shot.state;
}

std::string Engine::createPlaylist(std::string const& name, std::vector<PlaylistEntry> const& entries)
{
    std::lock_guard lock{mutex_};
    Catalog::Playlist playlist;
    playlist.id = "playlist-" + std::to_string(clipSerial_++);
    playlist.name = name.empty() ? playlist.id : name;
    playlist.entries = entries;
    catalog_.upsertPlaylist(playlist);
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

void Engine::playPlaylist(int channel, std::string const& id)
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
    runtime.scheduler.playing = true;
    runtime.shot.clipId = id;
    runtime.shot.state = ShotState::Playing;
}

std::string Engine::upload(std::uint8_t const* data, std::size_t size, std::string const& name, std::string& error)
{
    auto converted = convertUpload(data, size, config_.format);
    if (converted.frames.empty())
    {
        error = converted.error.empty() ? "upload failed" : converted.error;
        return {};
    }
    error.clear();
    auto const period = static_cast<std::uint64_t>(framePeriodNs(config_.format.rateNum, config_.format.rateDen));
    // Uploads go to the library ring, on the frame grid after its newest frame: a camera
    // ring only appends, so frames older than its live recording would be refused.
    std::lock_guard serial{uploadMutex_};
    auto const origin = (library_->newestNs() / period + 1) * period;
    ClipRef clip;
    clip.camera = kLibraryCamera;
    clip.inNs = origin;
    clip.outNs = origin + static_cast<std::uint64_t>(converted.frames.size() - 1) * period;
    // Protected first: the library ring keeps only segments a clip touches.
    library_->protect(clip.inNs, clip.outNs);
    std::size_t stored = 0;
    for (std::size_t i = 0; i < converted.frames.size(); ++i)
    {
        StoredFrame frame;
        frame.taiNs = origin + static_cast<std::uint64_t>(i) * period;
        frame.jpeg = gpuEncodeFrame10(converted.frames[i], config_.jpegQuality);
        if (frame.jpeg.empty())
        {
            frame.jpeg = encodeJpeg422(converted.frames[i], config_.jpegQuality);
        }
        if (!converted.audio.empty())
        {
            int const samples = audioSamplesForFrame(i, config_.format.rateNum, config_.format.rateDen, 48000);
            std::size_t const offset = i * static_cast<std::size_t>(samples) * static_cast<std::size_t>(converted.channels);
            if (offset < converted.audio.size())
            {
                frame.audio.assign(converted.audio.begin() + static_cast<std::ptrdiff_t>(offset),
                    converted.audio.begin() + static_cast<std::ptrdiff_t>(std::min(converted.audio.size(), offset + static_cast<std::size_t>(samples * converted.channels))));
            }
        }
        stored += library_->push(std::move(frame)) ? 1 : 0;
    }
    if (stored == 0)
    {
        library_->unprotect(clip.inNs, clip.outNs);
        error = "upload could not be stored";
        return {};
    }
    std::lock_guard lock{mutex_};
    clip.id = "upload-" + std::to_string(clipSerial_++);
    clip.name = name.empty() ? clip.id : name;
    clip.speed = 1;
    clip.motion = "interpolate";
    clip.audio = "mute";
    clip.library = true;
    clip.end = EndAction::Freeze;
    catalog_.upsertClip(clip);
    return clip.id;
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
    clip.library = true;
    catalog_.upsertClip(clip);
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

namespace
{
std::string jsonObject(std::string const& body, std::string const& key)
{
    auto const needle = "\"" + key + "\"";
    auto const pos = body.find(needle);
    if (pos == std::string::npos)
    {
        return {};
    }
    auto const brace = body.find('{', pos + needle.size());
    if (brace == std::string::npos || brace > pos + needle.size() + 8)
    {
        return {};
    }
    int depth = 0;
    bool inString = false;
    for (std::size_t i = brace; i < body.size(); ++i)
    {
        char const c = body[i];
        if (inString)
        {
            if (c == '\\' && i + 1 < body.size())
            {
                ++i;
                continue;
            }
            if (c == '"')
            {
                inString = false;
            }
            continue;
        }
        if (c == '"')
        {
            inString = true;
        }
        else if (c == '{')
        {
            ++depth;
        }
        else if (c == '}')
        {
            --depth;
            if (depth == 0)
            {
                return body.substr(brace, i - brace + 1);
            }
        }
    }
    return {};
}

std::string jsonArray(std::string const& body, std::string const& key)
{
    auto const needle = "\"" + key + "\"";
    auto const pos = body.find(needle);
    if (pos == std::string::npos)
    {
        return {};
    }
    auto const brace = body.find('[', pos + needle.size());
    if (brace == std::string::npos || brace > pos + needle.size() + 8)
    {
        return {};
    }
    int depth = 0;
    bool inString = false;
    for (std::size_t i = brace; i < body.size(); ++i)
    {
        char const c = body[i];
        if (inString)
        {
            if (c == '\\' && i + 1 < body.size())
            {
                ++i;
                continue;
            }
            if (c == '"')
            {
                inString = false;
            }
            continue;
        }
        if (c == '"')
        {
            inString = true;
        }
        else if (c == '[')
        {
            ++depth;
        }
        else if (c == ']')
        {
            --depth;
            if (depth == 0)
            {
                return body.substr(brace, i - brace + 1);
            }
        }
    }
    return {};
}

std::map<std::string, std::string> parseStringObject(std::string const& object)
{
    std::map<std::string, std::string> values;
    std::size_t i = 0;
    while (i < object.size())
    {
        auto const keyStart = object.find('"', i);
        if (keyStart == std::string::npos)
        {
            break;
        }
        auto const keyEnd = object.find('"', keyStart + 1);
        if (keyEnd == std::string::npos)
        {
            break;
        }
        auto const key = object.substr(keyStart + 1, keyEnd - keyStart - 1);
        auto const colon = object.find(':', keyEnd);
        if (colon == std::string::npos)
        {
            break;
        }
        auto const valueStart = object.find('"', colon + 1);
        if (valueStart == std::string::npos)
        {
            throw ConfigError("imported settings must be strings");
        }
        std::string value;
        for (std::size_t j = valueStart + 1; j < object.size(); ++j)
        {
            if (object[j] == '\\' && j + 1 < object.size())
            {
                char const next = object[j + 1];
                if (next == 'n')
                {
                    value.push_back('\n');
                }
                else if (next == 'r')
                {
                    value.push_back('\r');
                }
                else if (next == 't')
                {
                    value.push_back('\t');
                }
                else
                {
                    value.push_back(next);
                }
                ++j;
                continue;
            }
            if (object[j] == '"')
            {
                i = j + 1;
                break;
            }
            value.push_back(object[j]);
        }
        values[key] = value;
    }
    return values;
}

std::string fieldOf(std::string const& item, char const* key)
{
    auto const needle = std::string("\"") + key + "\"";
    auto const pos = item.find(needle);
    if (pos == std::string::npos)
    {
        return {};
    }
    auto const colon = item.find(':', pos + needle.size());
    if (colon == std::string::npos)
    {
        return {};
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
    while (stop < item.size() && item[stop] != ',' && item[stop] != '}' && item[stop] != ']')
    {
        ++stop;
    }
    return item.substr(i, stop - i);
}

std::vector<std::string> objectsIn(std::string const& array)
{
    std::vector<std::string> objects;
    int depth = 0;
    bool inString = false;
    std::size_t start = std::string::npos;
    for (std::size_t i = 0; i < array.size(); ++i)
    {
        char const c = array[i];
        if (inString)
        {
            if (c == '\\' && i + 1 < array.size())
            {
                ++i;
                continue;
            }
            if (c == '"')
            {
                inString = false;
            }
            continue;
        }
        if (c == '"')
        {
            inString = true;
        }
        else if (c == '{')
        {
            if (depth == 0)
            {
                start = i;
            }
            ++depth;
        }
        else if (c == '}')
        {
            --depth;
            if (depth == 0 && start != std::string::npos)
            {
                objects.push_back(array.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return objects;
}
} // namespace

std::string Engine::exportConfigJson() const
{
    std::lock_guard lock{mutex_};
    std::ostringstream out;
    out << "{\"version\":1,\"secrets\":false,\"settings\":{";
    bool first = true;
    for (auto const& [key, value] : settings_)
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
        out << "{\"id\":\"" << jsonEscape(playlist.id) << "\",\"name\":\"" << jsonEscape(playlist.name) << "\",\"entries\":[";
        bool entryFirst = true;
        for (auto const& entry : playlist.entries)
        {
            if (!entryFirst)
            {
                out << ',';
            }
            entryFirst = false;
            out << "{\"clip_id\":\"" << jsonEscape(entry.clipId) << "\",\"speed\":" << entry.speed << ",\"end\":\"" << endActionName(entry.end)
                << "\",\"auto_advance\":" << (entry.autoAdvance ? "true" : "false") << '}';
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

Engine::ImportResult Engine::importConfigJson(std::string const& body)
{
    ImportResult result;
    try
    {
        auto const settingsObject = jsonObject(body, "settings");
        if (settingsObject.empty() && body.find("\"settings\"") != std::string::npos)
        {
            result.error = "settings must be an object of strings";
            return result;
        }
        auto merged = settings_;
        if (!settingsObject.empty())
        {
            auto const imported = parseStringObject(settingsObject);
            for (auto const& [key, value] : imported)
            {
                merged[key] = value;
            }
        }
        else if (!body.empty() && body.find('{') != std::string::npos && body.find("\"clips\"") == std::string::npos)
        {
            auto const imported = parseStringObject(body);
            for (auto const& [key, value] : imported)
            {
                merged[key] = value;
            }
        }
        auto const loaded = loadConfig({}, {}, merged);
        static char const* restartKeys[] = {"REPLAY_FORMAT", "REPLAY_INPUTS", "REPLAY_CHANNELS", "REPLAY_STORAGE_DIR", "REPLAY_STATE_DIR", "WEB_PORT",
            "NMOS_PORT", "NMOS_SEED", "NMOS_LABEL", "NMOS_HOST_ADDRESS", "MXL_OUTPUT_DOMAIN_DIR", "MXL_OUTPUT_DOMAIN_ID", "NMOS_REGISTRY_ADDRESS",
            "NMOS_REGISTRY_PORT", "NMOS_QUERY_ADDRESS", "NMOS_QUERY_PORT"};
        for (auto const* key : restartKeys)
        {
            auto const before = settings_.count(key) ? settings_.at(key) : std::string{};
            auto const after = merged.count(key) ? merged.at(key) : std::string{};
            if (before != after)
            {
                result.restartRequired = true;
            }
        }
        std::lock_guard lock{mutex_};
        settings_ = merged;
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
            }
        }
        std::ofstream out(config_.stateDir + "/config.json");
        if (!out)
        {
            result.error = "cannot write " + config_.stateDir + "/config.json";
            return result;
        }
        out << "{\n";
        bool first = true;
        for (auto const& [key, value] : settings_)
        {
            if (!first)
            {
                out << ",\n";
            }
            first = false;
            out << "  \"" << jsonEscape(key) << "\": \"" << jsonEscape(value) << '"';
        }
        out << "\n}\n";
        for (auto const& item : objectsIn(jsonArray(body, "clips")))
        {
            ClipRef clip;
            clip.id = fieldOf(item, "id");
            clip.name = fieldOf(item, "name");
            if (clip.id.empty())
            {
                continue;
            }
            clip.camera = std::atoi(fieldOf(item, "camera").c_str());
            clip.inNs = static_cast<std::uint64_t>(std::strtoull(fieldOf(item, "in_ns").c_str(), nullptr, 10));
            clip.outNs = static_cast<std::uint64_t>(std::strtoull(fieldOf(item, "out_ns").c_str(), nullptr, 10));
            clip.speed = std::strtod(fieldOf(item, "speed").c_str(), nullptr);
            clip.motion = fieldOf(item, "motion");
            clip.audio = fieldOf(item, "audio");
            clip.colour = fieldOf(item, "colour");
            clip.tags = fieldOf(item, "tags");
            bool endOk = false;
            clip.end = parseEndAction(fieldOf(item, "end"), &endOk);
            if (!endOk)
            {
                clip.end = EndAction::Freeze;
            }
            clip.library = fieldOf(item, "library") == "true";
            clip.groupId = fieldOf(item, "group");
            catalog_.upsertClip(clip);
        }
        for (auto const& item : objectsIn(jsonArray(body, "playlists")))
        {
            Catalog::Playlist playlist;
            playlist.id = fieldOf(item, "id");
            playlist.name = fieldOf(item, "name");
            if (playlist.id.empty())
            {
                continue;
            }
            for (auto const& entryText : objectsIn(jsonArray(item, "entries")))
            {
                PlaylistEntry entry;
                entry.clipId = fieldOf(entryText, "clip_id");
                entry.speed = std::strtod(fieldOf(entryText, "speed").c_str(), nullptr);
                bool endOk = false;
                entry.end = parseEndAction(fieldOf(entryText, "end"), &endOk);
                if (!endOk)
                {
                    entry.end = EndAction::Next;
                }
                entry.autoAdvance = fieldOf(entryText, "auto_advance") != "false";
                playlist.entries.push_back(entry);
            }
            catalog_.upsertPlaylist(playlist);
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

std::vector<std::uint8_t> Engine::previewJpeg(int channel) const
{
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
    return encodeJpeg422(small, kPreviewQuality);
}

std::vector<std::uint8_t> Engine::storedPreview(int camera, std::uint64_t taiNs, int width) const
{
    FrameRing const* ring = nullptr;
    {
        std::lock_guard lock{mutex_};
        ring = ringOf(camera);
    }
    // The ring has its own lock; the read and the decode run without the engine lock.
    auto const stored = ring != nullptr ? ring->findNearest(taiNs) : std::nullopt;
    if (!stored || stored->jpeg.empty())
    {
        return {};
    }
    std::vector<std::uint8_t> v210(v210Size(config_.format.width, config_.format.height));
    if (!decodeJpegToV210(stored->jpeg.data(), stored->jpeg.size(), config_.format.width, config_.format.height, 0, v210.data()))
    {
        return {};
    }
    return encodeJpeg422(sampleV210(v210.data(), config_.format.width, config_.format.height, width), kPreviewQuality);
}

std::vector<std::uint8_t> Engine::cameraPreviewJpeg(int camera) const
{
    if (camera < 1)
    {
        return {};
    }
    std::uint64_t newest = 0;
    {
        std::lock_guard lock{mutex_};
        auto const* ring = ringOf(camera);
        newest = ring != nullptr ? ring->newestNs() : 0;
    }
    return newest == 0 ? std::vector<std::uint8_t>{} : storedPreview(camera, newest, 384);
}

std::vector<std::uint8_t> Engine::clipThumbnailJpeg(std::string const& id) const
{
    auto const clip = this->clip(id);
    return clip.id.empty() ? std::vector<std::uint8_t>{} : storedPreview(clip.camera, clip.inNs, 384);
}

bool Engine::gpuInterpolate() const
{
    return gpu_ || config_.allowCpuInterp;
}

void Engine::setGpuPresent(bool present)
{
    gpu_ = present || cudaFlowAvailable();
}

void Engine::updateMetrics()
{
    std::lock_guard lock{mutex_};
    for (std::size_t i = 0; i < cameras_.size(); ++i)
    {
        auto const& camera = cameras_[i];
        Labels const labels{{"camera", std::to_string(config_.cameras[i].index)}};
        metrics_.set("record_frames_total", labels, static_cast<double>(camera.recorded));
        metrics_.set("record_dropped_total", labels, static_cast<double>(camera.dropped));
        metrics_.set("phase_missing_total", labels, static_cast<double>(camera.phaseMissing));
        // Frames lost to a full disk or an I/O error (also part of record_dropped_total).
        metrics_.set("storage_write_failed_total", labels, static_cast<double>(camera.ring.writeFailures()));
        metrics_.set("protected_bytes", labels, static_cast<double>(camera.ring.protectedBytes()));
        metrics_.set("disk_bytes", labels, static_cast<double>(camera.ring.diskBytes()));
    }
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
        << "\",\"jpeg_bit_depth\":" << jpegStorageBitDepth()
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
            << ",\"playlist_index\":" << channel.playlistIndex << '}';
    }
    out << "]}";
    return out.str();
}
} // namespace replay
