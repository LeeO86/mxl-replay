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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

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

Frame10 blendFrames(Frame10 const& a, Frame10 const& b, double phase)
{
    Frame10 out;
    out.allocate(a.width, a.height);
    auto mix = [&](std::uint16_t left, std::uint16_t right) {
        double const v = static_cast<double>(left) * (1.0 - phase) + static_cast<double>(right) * phase;
        return static_cast<std::uint16_t>(std::clamp(v, 0.0, 1023.0));
    };
    for (std::size_t i = 0; i < out.y.size(); ++i)
    {
        out.y[i] = mix(a.y[i], b.y[i]);
    }
    for (std::size_t i = 0; i < out.cb.size(); ++i)
    {
        out.cb[i] = mix(a.cb[i], b.cb[i]);
        out.cr[i] = mix(a.cr[i], b.cr[i]);
    }
    return out;
}

std::vector<std::uint8_t> tinyPreview(Frame10 const& frame)
{
    Frame10 small;
    small.allocate(32, 16);
    scaleFrame(frame, small, ScaleFilter::Bilinear);
    return encodeJpeg422(small, 70);
}

std::string jsonString(std::string const& text)
{
    std::string out = "\"";
    for (char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}
} // namespace

Engine::CameraRuntime::CameraRuntime(std::size_t capacity)
    : ring(capacity)
{
}

Engine::Engine(Config config)
    : config_(std::move(config))
    , ids_(makeNmosIds(config_.nmosSeed))
{
    std::error_code ec;
    std::filesystem::create_directories(config_.storageDir, ec);
    if (ec)
    {
        throw StartupError(75, "cannot create storage directory " + config_.storageDir + ": " + ec.message());
    }
    struct statvfs st{};
    if (statvfs(config_.storageDir.c_str(), &st) != 0)
    {
        throw StartupError(75, "cannot stat storage directory " + config_.storageDir);
    }
    auto const freeBytes = static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_frsize);
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
    catalog_.open(config_.storageDir + "/index.sqlite");
    gpu_ = cudaFlowAvailable();
    auto const period = framePeriodNs(config_.format.rateNum, config_.format.rateDen);
    for (auto const& camera : config_.cameras)
    {
        double const hours = camera.bufferHours;
        double const fps = camera.nativeFps > 0 ? static_cast<double>(camera.nativeFps)
                                                 : static_cast<double>(config_.format.rateNum) / static_cast<double>(config_.format.rateDen);
        auto const capacity = static_cast<std::size_t>(std::max(8.0, hours * 3600.0 * fps));
        cameras_.emplace_back(capacity);
        auto& runtime = cameras_.back();
        runtime.phases.resize(static_cast<std::size_t>(camera.phases));
        runtime.writer = std::make_unique<SegmentWriter>(config_.storageDir + "/cam" + std::to_string(camera.index), camera.index, config_.segmentSeconds,
            config_.odirect);
    }
    for (auto const& channel : config_.channelList)
    {
        ChannelRuntime runtime;
        runtime.scheduler.framePeriodNs = period;
        runtime.scheduler.camera = 1;
        runtime.scheduler.rampFrames = config_.rampFrames;
        runtime.shot.playOnFirstClick = config_.playOnFirstClick;
        runtime.last.allocate(config_.format.width, config_.format.height);
        runtime.last.fill(64, 512, 512);
        (void)channel;
        channels_.push_back(std::move(runtime));
    }
    logInfo("replay_ready", {{"storage", config_.storageDir}, {"inputs", std::to_string(config_.inputs)}, {"channels", std::to_string(config_.channels)}});
}

Engine::~Engine() = default;

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

void Engine::storeFrame(CameraRuntime& camera, std::uint64_t taiNs, Frame10 const& frame, std::vector<float> const& audio)
{
    StoredFrame stored;
    stored.taiNs = taiNs;
    stored.jpeg = gpuEncodeFrame10(frame, config_.jpegQuality);
    if (stored.jpeg.empty())
    {
        stored.jpeg = encodeJpeg422(frame, config_.jpegQuality);
    }
    stored.audio = audio;
    if (!camera.ring.push(stored))
    {
        ++camera.dropped;
        return;
    }
    if (camera.writer)
    {
        camera.writer->write(taiNs, stored.jpeg.data(), stored.jpeg.size(), audio.data(), audio.size());
    }
    ++camera.recorded;
    camera.preview = tinyPreview(frame);
}

void Engine::flushHouse(CameraRuntime& camera, CameraConfig const& cfg)
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
        storeFrame(camera, item.timeNs, picture.empty() ? slot.frame : picture, audio);
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
    auto jpeg = gpuEncodeV210(packed, config_.format.width, config_.format.height, static_cast<int>(v210RowBytes(config_.format.width)), config_.jpegQuality);
    if (jpeg.empty())
    {
        Frame10 frame;
        frame.allocate(config_.format.width, config_.format.height);
        unpackV210(packed, static_cast<int>(v210RowBytes(config_.format.width)), frame);
        ingestVideo(camera, phase, taiNs, std::move(frame));
        return;
    }
    std::lock_guard lock{mutex_};
    if (camera < 1 || camera > static_cast<int>(cameras_.size()) || !config_.cameras[static_cast<std::size_t>(camera - 1)].record)
    {
        return;
    }
    auto& runtime = cameras_[static_cast<std::size_t>(camera - 1)];
    StoredFrame stored;
    stored.taiNs = taiNs;
    stored.jpeg = std::move(jpeg);
    stored.audio = runtime.audio;
    runtime.audio.clear();
    if (!runtime.ring.push(std::move(stored)))
    {
        ++runtime.dropped;
        return;
    }
    if (runtime.writer)
    {
        auto const& kept = runtime.ring.findNearest(taiNs);
        if (kept)
        {
            runtime.writer->write(taiNs, kept->jpeg.data(), kept->jpeg.size(), kept->audio.data(), kept->audio.size());
        }
    }
    ++runtime.recorded;
}

void Engine::ingestVideo(int camera, int phase, std::uint64_t taiNs, Frame10 frame)
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
        storeFrame(runtime, taiNs, frame, runtime.audio);
        runtime.audio.clear();
        return;
    }
    auto const houseIndex = timestampToIndex(config_.format.rateNum, config_.format.rateDen, taiNs);
    if (runtime.houseOpen && houseIndex != runtime.openHouse)
    {
        flushHouse(runtime, cfg);
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
        flushHouse(runtime, cfg);
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
    if (camera < 1 || camera > static_cast<int>(cameras_.size()))
    {
        return frame;
    }
    auto const stored = cameras_[static_cast<std::size_t>(camera - 1)].ring.findNearest(taiNs);
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

RenderedFrame Engine::render(int channel, std::uint64_t outputTaiNs)
{
    std::lock_guard lock{mutex_};
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
    if (!runtime.black && cudaFlowAvailable() && camera >= 1 && camera <= static_cast<int>(cameras_.size()))
    {
        auto const storedA = cameras_[static_cast<std::size_t>(camera - 1)].ring.findNearest(taiA);
        auto const storedB = cameras_[static_cast<std::size_t>(camera - 1)].ring.findNearest(taiB);
        bool const second = storedB && storedA && storedB->taiNs != storedA->taiNs && pick.kind != SourcePick::Kind::Exact && pick.kind != SourcePick::Kind::Repeat;
        GpuPicture gpuPicture;
        auto const key = std::to_string(camera) + ":" + std::to_string(taiA) + ":" + std::to_string(taiB);
        if (storedA && !storedA->jpeg.empty() &&
            gpuRenderFromJpeg(storedA->jpeg.data(), storedA->jpeg.size(), second ? storedB->jpeg.data() : nullptr, second ? storedB->jpeg.size() : 0,
                static_cast<float>(pick.phase), motion == MotionMode::Interpolate && pick.kind == SourcePick::Kind::Interpolate, operatingPoint(config_.preset), key,
                config_.format.width, config_.format.height, gpuPicture))
        {
            rendered.v210 = std::move(gpuPicture.v210);
            runtime.preview = std::move(gpuPicture.preview);
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
    if (renderedOnGpu)
    {
        foundA = true;
    }
    else
    {
        frameA = frameAt(camera, taiA, &foundA);
        frameB = frameAt(camera, taiB, &foundB);
    }
    if (runtime.black || (!foundA && !foundB))
    {
        picture.allocate(config_.format.width, config_.format.height);
        if (cfg.idle == IdleSource::Last && !runtime.last.empty() && !runtime.black)
        {
            picture = runtime.last;
        }
        else if (cfg.idle == IdleSource::E2e && !runtime.black)
        {
            picture = frameAt(camera, outputTaiNs, &foundA);
            if (!foundA)
            {
                picture.allocate(config_.format.width, config_.format.height);
                picture.fill(64, 512, 512);
            }
        }
        else
        {
            picture.fill(64, 512, 512);
        }
        rendered.black = runtime.black || (!foundA && !foundB);
        if (!runtime.black && cfg.idle == IdleSource::Last && !runtime.lastV210.empty() && !foundA)
        {
            rendered.v210 = runtime.lastV210;
            renderedOnGpu = true;
            rendered.black = false;
        }
    }
    else if (renderedOnGpu)
    {
        exact = pick.kind == SourcePick::Kind::Exact || pick.kind == SourcePick::Kind::Repeat;
    }
    else if (!foundB || pick.kind == SourcePick::Kind::Exact || pick.kind == SourcePick::Kind::Repeat || std::fabs(pick.phase) < 1e-6)
    {
        picture = foundA ? frameA : frameB;
        exact = true;
    }
    else if (pick.kind == SourcePick::Kind::Blend || motion == MotionMode::Blend)
    {
        picture = blendFrames(frameA, frameB, pick.phase);
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
    if (!renderedOnGpu)
    {
        runtime.last = picture;
        runtime.preview = tinyPreview(picture);
        rendered.v210.resize(v210Size(picture.width, picture.height));
        packV210(picture, rendered.v210.data(), 0);
        runtime.lastV210 = rendered.v210;
    }
    rendered.positionNs = position;
    rendered.speed = runtime.scheduler.speed;
    rendered.motion = motionName(exact && motion == MotionMode::Interpolate ? MotionMode::Repeat : motion);
    rendered.exact = exact;
    rendered.camera = camera;
    auto const tcTai = cfg.timecode == TcMode::Source ? position : outputTaiNs;
    auto const tc = timecodeFromTai(tcTai, config_.format.rateNum, config_.format.rateDen, false);
    rendered.timecode = tc.format();
    AncPacket packet;
    packet.tc = tc;
    packet.sequence = static_cast<std::uint16_t>(runtime.ancSequence++);
    rendered.anc = encodeAncGrain(packet);

    int const samples = audioSamplesForFrame(timestampToIndex(config_.format.rateNum, config_.format.rateDen, outputTaiNs), config_.format.rateNum,
        config_.format.rateDen, 48000);
    int const audioCamera = cfg.atmosCamera > 0 ? cfg.atmosCamera : camera;
    std::vector<float> pcm(static_cast<std::size_t>(samples) * 2, 0.f);
    if (audioCamera >= 1 && audioCamera <= static_cast<int>(cameras_.size()))
    {
        auto const stored = cameras_[static_cast<std::size_t>(audioCamera - 1)].ring.findNearest(position);
        if (stored && !stored->audio.empty())
        {
            pcm.assign(stored->audio.begin(), stored->audio.begin() + static_cast<std::ptrdiff_t>(std::min(stored->audio.size(), pcm.size())));
            pcm.resize(static_cast<std::size_t>(samples) * 2, 0.f);
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
    auto const action = clip.id.empty() ? runtime.shot.end : clip.end;
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
    }
    else if (action == EndAction::Next && !runtime.playlistId.empty())
    {
        auto const lists = catalog_.playlists();
        for (auto const& list : lists)
        {
            if (list.id != runtime.playlistId)
            {
                continue;
            }
            int const next = playlistNext(list.entries, runtime.playlistIndex);
            if (next < 0)
            {
                runtime.scheduler.playing = false;
                runtime.scheduler.positionNs = static_cast<std::int64_t>(runtime.outNs);
            }
            else
            {
                runtime.playlistIndex = next;
                playClip(channel, list.entries[static_cast<std::size_t>(next)].clipId);
            }
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
    runtime.clipId.clear();
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

std::string Engine::createClip(int channel, std::string const& name, bool allAngles, bool force, std::string& error)
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
        ClipRef clip;
        clip.id = "clip-" + std::to_string(clipSerial_++);
        clip.name = name.empty() ? clip.id : name;
        if (allAngles)
        {
            clip.name += " " + config_.cameras[static_cast<std::size_t>(camera - 1)].label;
        }
        clip.camera = camera;
        clip.inNs = runtime.inNs;
        clip.outNs = runtime.outNs;
        clip.speed = runtime.scheduler.speed;
        clip.motion = motionName(config_.channelList[static_cast<std::size_t>(channel - 1)].motion);
        clip.audio = audioModeName(config_.channelList[static_cast<std::size_t>(channel - 1)].audio);
        clip.colour = config_.cameras[static_cast<std::size_t>(camera - 1)].colour;
        clip.end = EndAction::Freeze;
        clip.groupId = allAngles ? group : "";
        catalog_.upsertClip(clip);
        cameras_[static_cast<std::size_t>(camera - 1)].ring.protect(clip.inNs, clip.outNs);
        if (first.empty())
        {
            first = clip.id;
        }
    }
    return first;
}

void Engine::updateClip(ClipRef clip)
{
    std::lock_guard lock{mutex_};
    if (clip.id.empty())
    {
        return;
    }
    auto existing = catalog_.clip(clip.id);
    if (existing.id.empty())
    {
        return;
    }
    if (clip.name.empty())
    {
        clip.name = existing.name;
    }
    catalog_.upsertClip(clip);
    if (clip.camera >= 1 && clip.camera <= static_cast<int>(cameras_.size()))
    {
        cameras_[static_cast<std::size_t>(clip.camera - 1)].ring.protect(clip.inNs, clip.outNs);
    }
}

void Engine::deleteClip(std::string const& id)
{
    std::lock_guard lock{mutex_};
    auto clip = catalog_.clip(id);
    catalog_.deleteClip(id);
    if (!clip.id.empty() && clip.camera >= 1 && clip.camera <= static_cast<int>(cameras_.size()))
    {
        cameras_[static_cast<std::size_t>(clip.camera - 1)].ring.unprotect(clip.inNs, clip.outNs);
    }
}

std::vector<ClipRef> Engine::clips() const
{
    std::lock_guard lock{mutex_};
    return catalog_.clips();
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
    runtime.liveMode = false;
    runtime.black = false;
    runtime.clipId = clip.id;
    runtime.hasIn = true;
    runtime.hasOut = true;
    runtime.inNs = clip.inNs;
    runtime.outNs = clip.outNs;
    runtime.scheduler.camera = clip.camera;
    runtime.scheduler.positionNs = static_cast<std::int64_t>(clip.inNs);
    runtime.scheduler.setTargetSpeed(clip.speed, 0);
    runtime.scheduler.playing = true;
    runtime.shot.clipId = clip.id;
    runtime.shot.end = clip.end;
    runtime.shot.state = ShotState::Playing;
    runtime.fadeFramesLeft = 0;
    bool ok = false;
    config_.channelList[static_cast<std::size_t>(channel - 1)].motion = parseMotion(clip.motion, &ok);
    config_.channelList[static_cast<std::size_t>(channel - 1)].audio = parseAudioMode(clip.audio, &ok);
}

ShotState Engine::shotClick(int channel, std::string const& clipId)
{
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return ShotState::Idle;
    }
    auto& runtime = channels_[static_cast<std::size_t>(channel - 1)];
    if (runtime.clipId != clipId)
    {
        runtime.shot.state = ShotState::Idle;
        runtime.shot.clipId = clipId;
    }
    runtime.shot.playOnFirstClick = config_.playOnFirstClick;
    auto const state = runtime.shot.click();
    if (state == ShotState::Cued)
    {
        auto clip = catalog_.clip(clipId);
        if (!clip.id.empty())
        {
            runtime.scheduler.positionNs = static_cast<std::int64_t>(clip.inNs);
            runtime.scheduler.playing = false;
            runtime.liveMode = false;
            runtime.clipId = clipId;
            runtime.inNs = clip.inNs;
            runtime.outNs = clip.outNs;
            runtime.hasIn = true;
            runtime.hasOut = true;
        }
    }
    else if (state == ShotState::Playing)
    {
        playClip(channel, clipId);
    }
    else if (state == ShotState::Paused)
    {
        runtime.scheduler.playing = false;
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

void Engine::deletePlaylist(std::string const& id)
{
    std::lock_guard lock{mutex_};
    catalog_.deletePlaylist(id);
}

std::vector<Catalog::Playlist> Engine::playlists() const
{
    std::lock_guard lock{mutex_};
    return catalog_.playlists();
}

void Engine::playPlaylist(int channel, std::string const& id)
{
    std::lock_guard lock{mutex_};
    for (auto const& playlist : catalog_.playlists())
    {
        if (playlist.id == id && !playlist.entries.empty())
        {
            channels_[static_cast<std::size_t>(channel - 1)].playlistId = id;
            channels_[static_cast<std::size_t>(channel - 1)].playlistIndex = 0;
            playClip(channel, playlist.entries[0].clipId);
        }
    }
}

std::string Engine::upload(std::uint8_t const* data, std::size_t size, std::string const& name, std::string& error)
{
    auto converted = convertUpload(data, size, config_.format);
    if (converted.frames.empty())
    {
        error = converted.error.empty() ? "upload failed" : converted.error;
        return {};
    }
    std::lock_guard lock{mutex_};
    error.clear();
    auto const id = "upload-" + std::to_string(clipSerial_++);
    auto const period = static_cast<std::uint64_t>(framePeriodNs(config_.format.rateNum, config_.format.rateDen));
    std::uint64_t const origin = 1;
    for (std::size_t i = 0; i < converted.frames.size(); ++i)
    {
        std::vector<float> audio;
        if (!converted.audio.empty())
        {
            int const samples = audioSamplesForFrame(i, config_.format.rateNum, config_.format.rateDen, 48000);
            std::size_t const offset = i * static_cast<std::size_t>(samples) * static_cast<std::size_t>(converted.channels);
            if (offset < converted.audio.size())
            {
                audio.assign(converted.audio.begin() + static_cast<std::ptrdiff_t>(offset),
                    converted.audio.begin() + static_cast<std::ptrdiff_t>(std::min(converted.audio.size(), offset + static_cast<std::size_t>(samples * converted.channels))));
            }
        }
        storeFrame(cameras_[0], origin + static_cast<std::uint64_t>(i) * period, converted.frames[i], audio);
    }
    ClipRef clip;
    clip.id = id;
    clip.name = name.empty() ? id : name;
    clip.camera = 1;
    clip.inNs = origin;
    clip.outNs = origin + static_cast<std::uint64_t>(converted.frames.size() - 1) * period;
    clip.speed = 1;
    clip.motion = "interpolate";
    clip.audio = "mute";
    clip.library = true;
    clip.end = EndAction::Freeze;
    catalog_.upsertClip(clip);
    cameras_[0].ring.protect(clip.inNs, clip.outNs);
    if (converted.error == "scaled")
    {
        cameras_[0].scaled = true;
    }
    return id;
}

std::string Engine::exportClip(std::string const& id, std::string& error)
{
    std::lock_guard lock{mutex_};
    auto clip = catalog_.clip(id);
    if (clip.id.empty())
    {
        error = "unknown clip";
        return {};
    }
    auto const dir = libraryDir_ + "/" + id;
    std::filesystem::create_directories(dir);
    auto const period = sourcePeriod(clip.camera);
    std::vector<float> audio;
    int index = 0;
    for (std::uint64_t tai = clip.inNs; tai <= clip.outNs; tai += period)
    {
        bool found = false;
        auto frame = frameAt(clip.camera, tai, &found);
        if (!found)
        {
            continue;
        }
        auto const jpeg = encodeJpeg422(frame, config_.jpegQuality);
        std::ofstream out(dir + "/frame_" + std::to_string(index) + ".jpg", std::ios::binary);
        out.write(reinterpret_cast<char const*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        auto const stored = cameras_[static_cast<std::size_t>(clip.camera - 1)].ring.findNearest(tai);
        if (stored)
        {
            audio.insert(audio.end(), stored->audio.begin(), stored->audio.end());
        }
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

void Engine::setRoute(int camera, int phase, bool video, Route route)
{
    std::lock_guard lock{mutex_};
    routes_[std::to_string(camera) + (video ? "v" : "a") + std::to_string(phase)] = std::move(route);
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
    std::lock_guard lock{mutex_};
    if (channel < 1 || channel > static_cast<int>(channels_.size()))
    {
        return {};
    }
    return channels_[static_cast<std::size_t>(channel - 1)].preview;
}

bool Engine::gpuInterpolate() const
{
    return gpu_ || config_.allowCpuInterp;
}

void Engine::setGpuPresent(bool present)
{
    gpu_ = present || cudaFlowAvailable();
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
    out << "{\"version\":\"" << REPLAY_VERSION << "\",\"format\":" << jsonString(config_.format.token()) << ",\"gpu\":" << (gpuInterpolate() ? "true" : "false")
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
            << ",\"scaled\":" << (camera.scaled ? "true" : "false") << ",\"protected_bytes\":" << camera.ring.protectedBytes() << '}';
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
            << ",\"position_ns\":" << channel.scheduler.positionNs << ",\"camera\":" << channel.scheduler.camera
            << ",\"motion\":" << jsonString(motionName(cfg.motion)) << ",\"audio\":" << jsonString(audioModeName(cfg.audio))
            << ",\"shot\":" << jsonString(shotStateName(channel.shot.state)) << ",\"clip\":" << jsonString(channel.clipId)
            << ",\"in_ns\":" << channel.inNs << ",\"out_ns\":" << channel.outNs << ",\"lock\":" << (cfg.lockToFirst ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}
} // namespace replay
