#include "mxl/io.hpp"

#include "domain/scan.hpp"
#include "flow/cuda_flow.hpp"
#include "media/timebase.hpp"
#include "media/v210.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#if defined(REPLAY_WITH_MXL)
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#endif

namespace replay
{
struct MxlBridge::Impl
{
#if defined(REPLAY_WITH_MXL)
    mxlInstance instance = nullptr;
    struct Writer
    {
        mxlFlowWriter video = nullptr;
        mxlFlowWriter audio = nullptr;
        mxlFlowWriter anc = nullptr;
    };
    std::vector<Writer> writers;
    struct Reader
    {
        mxlFlowReader handle = nullptr;
        int camera = 0;
        int phase = 0;
        bool video = true;
        std::string flowId;
    };
    std::vector<Reader> readers;
#endif
};

MxlBridge::MxlBridge(Engine& engine)
    : engine_(engine)
    , impl_(new Impl)
{
}

MxlBridge::~MxlBridge()
{
    stop();
    delete impl_;
}

void MxlBridge::start()
{
#if !defined(REPLAY_WITH_MXL)
    logInfo("mxl_disabled", {{"reason", "built without libmxl"}});
#else
    auto const& cfg = engine_.config();
    auto const domain = ensureOutputDomain(cfg.outputDomainDir, cfg.outputDomainId, cfg.historyDurationNs);
    if (domain.status != DomainStatus::Ready)
    {
        logError("mxl_domain_rejected", {{"dir", cfg.outputDomainDir}, {"error", domain.message}});
        int const code = domain.status == DomainStatus::Failed ? 75 : 78;
        throw StartupError(code, domain.message.empty() ? "output domain was rejected" : domain.message);
    }
    impl_->instance = mxlCreateInstance(cfg.outputDomainDir.c_str(), nullptr);
    if (impl_->instance == nullptr)
    {
        logError("mxl_instance_failed", {{"dir", cfg.outputDomainDir}});
        return;
    }
    auto const& ids = engine_.ids();
    mxlRational const rate{cfg.format.rateNum, cfg.format.rateDen};
    (void)rate;
    for (auto const& channel : cfg.channelList)
    {
        Impl::Writer writer;
        auto videoDef = std::string("{\"id\":\"") + ids.videoFlow(channel.index, cfg.format.token()) +
                        "\",\"format\":\"urn:x-nmos:format:video\",\"label\":\"" + channel.label +
                        "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" + channel.label + ":Video\"]},\"media_type\":\"video/v210\",\"grain_rate\":{\"numerator\":" +
                        std::to_string(cfg.format.rateNum) + ",\"denominator\":" + std::to_string(cfg.format.rateDen) + "},\"frame_width\":" +
                        std::to_string(cfg.format.width) + ",\"frame_height\":" + std::to_string(cfg.format.height) +
                        ",\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"components\":[{\"name\":\"Y\",\"width\":" +
                        std::to_string(cfg.format.width) + ",\"height\":" + std::to_string(cfg.format.height) +
                        ",\"bit_depth\":10},{\"name\":\"Cb\",\"width\":" + std::to_string(cfg.format.width / 2) + ",\"height\":" +
                        std::to_string(cfg.format.height) + ",\"bit_depth\":10},{\"name\":\"Cr\",\"width\":" + std::to_string(cfg.format.width / 2) +
                        ",\"height\":" + std::to_string(cfg.format.height) + ",\"bit_depth\":10}]}";
        bool created = false;
        mxlFlowConfigInfo info{};
        if (mxlCreateFlowWriter(impl_->instance, videoDef.c_str(), "{}", &writer.video, &info, &created) != MXL_STATUS_OK)
        {
            logWarn("mxl_video_writer_failed", {{"channel", std::to_string(channel.index)}});
        }
        auto audioDef = std::string("{\"id\":\"") + ids.audioFlow(channel.index) + "\",\"format\":\"urn:x-nmos:format:audio\",\"label\":\"" + channel.label +
                        "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" + channel.label +
                        ":Audio\"]},\"media_type\":\"audio/float32\",\"sample_rate\":{\"numerator\":48000,\"denominator\":1},\"channel_count\":2,\"bit_depth\":32}";
        mxlCreateFlowWriter(impl_->instance, audioDef.c_str(), "{}", &writer.audio, nullptr, &created);
        auto ancDef = std::string("{\"id\":\"") + ids.dataFlow(channel.index, cfg.format.token()) + "\",\"format\":\"urn:x-nmos:format:data\",\"label\":\"" +
                      channel.label + "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" + channel.label +
                      ":Data\"]},\"media_type\":\"video/smpte291\",\"grain_rate\":{\"numerator\":" + std::to_string(cfg.format.rateNum) +
                      ",\"denominator\":" + std::to_string(cfg.format.rateDen) + "}}";
        mxlCreateFlowWriter(impl_->instance, ancDef.c_str(), "{}", &writer.anc, nullptr, &created);
        impl_->writers.push_back(writer);
    }
    active_ = true;
    run_ = true;
    for (auto const& camera : cfg.cameras)
    {
        for (int phase = 1; phase <= camera.phases; ++phase)
        {
            threads_.emplace_back([this, index = camera.index, phase] { readInput(index, phase); });
        }
    }
    logInfo("mxl_started", {{"domain", cfg.outputDomainId}});
#endif
}

void MxlBridge::stop()
{
    run_ = false;
    for (auto& thread : threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    threads_.clear();
#if defined(REPLAY_WITH_MXL)
    if (impl_ != nullptr && impl_->instance != nullptr)
    {
        for (auto& writer : impl_->writers)
        {
            if (writer.video != nullptr)
            {
                mxlReleaseFlowWriter(impl_->instance, writer.video);
            }
            if (writer.audio != nullptr)
            {
                mxlReleaseFlowWriter(impl_->instance, writer.audio);
            }
            if (writer.anc != nullptr)
            {
                mxlReleaseFlowWriter(impl_->instance, writer.anc);
            }
        }
        impl_->writers.clear();
        for (auto& reader : impl_->readers)
        {
            if (reader.handle != nullptr)
            {
                mxlReleaseFlowReader(impl_->instance, reader.handle);
            }
        }
        impl_->readers.clear();
        mxlDestroyInstance(impl_->instance);
        impl_->instance = nullptr;
    }
#endif
    active_ = false;
}

void MxlBridge::publish(int channel, RenderedFrame const& frame, std::uint64_t taiNs)
{
#if !defined(REPLAY_WITH_MXL)
    (void)channel;
    (void)frame;
    (void)taiNs;
#else
    if (!active_ || channel < 1 || channel > static_cast<int>(impl_->writers.size()))
    {
        return;
    }
    auto const& cfg = engine_.config();
    mxlRational const rate{cfg.format.rateNum, cfg.format.rateDen};
    auto const index = mxlTimestampToIndex(&rate, taiNs);
    auto& writer = impl_->writers[static_cast<std::size_t>(channel - 1)];
    if (writer.video != nullptr && !frame.v210.empty())
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowWriterOpenGrain(writer.video, index, &info, &payload) == MXL_STATUS_OK && payload != nullptr)
        {
            auto const n = std::min(frame.v210.size(), static_cast<std::size_t>(info.grainSize));
            std::memcpy(payload, frame.v210.data(), n);
            info.flags = 0;
            info.validSlices = info.totalSlices;
            mxlFlowWriterCommitGrain(writer.video, &info);
        }
    }
    if (writer.anc != nullptr && !frame.anc.empty())
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowWriterOpenGrain(writer.anc, index, &info, &payload) == MXL_STATUS_OK && payload != nullptr)
        {
            auto const n = std::min(frame.anc.size(), static_cast<std::size_t>(info.grainSize));
            std::memcpy(payload, frame.anc.data(), n);
            info.flags = 0;
            info.validSlices = info.totalSlices;
            mxlFlowWriterCommitGrain(writer.anc, &info);
        }
    }
    if (writer.audio != nullptr && !frame.audio.empty())
    {
        // The frame's samples [first, end) at 48 kHz, addressed by their end index; each
        // channel has its own ring (`stride` apart), possibly wrapped into two fragments.
        auto const first = audioSamplesUntil(index, cfg.format.rateNum, cfg.format.rateDen, 48000);
        auto const end = audioSamplesUntil(index + 1, cfg.format.rateNum, cfg.format.rateDen, 48000);
        auto const count = std::min(static_cast<std::size_t>(end - first), frame.audio.size() / 2);
        mxlMutableWrappedMultiBufferSlice slices{};
        if (count > 0 && mxlFlowWriterOpenSamples(writer.audio, first + count, count, &slices) == MXL_STATUS_OK)
        {
            for (std::size_t channel = 0; channel < slices.count && channel < 2; ++channel)
            {
                std::size_t sample = 0;
                for (auto const& fragment : slices.base.fragments)
                {
                    auto* out = reinterpret_cast<float*>(static_cast<std::uint8_t*>(fragment.pointer) + channel * slices.stride);
                    for (std::size_t i = 0; i < fragment.size / sizeof(float) && sample < count; ++i, ++sample)
                    {
                        out[i] = frame.audio[sample * 2 + channel];
                    }
                }
            }
            mxlFlowWriterCommitSamples(writer.audio);
        }
    }
#endif
}

void MxlBridge::readInput(int camera, int phase)
{
#if defined(REPLAY_WITH_MXL)
    auto const& cfg = engine_.config();
    mxlRational const rate{cfg.format.rateNum, cfg.format.rateDen};
    // The reader is kept open while its route stays the same. Opening a reader scans
    // every domain and maps every grain of the flow, so doing it per grain recorded
    // about 9 of 50 grains per camera.
    struct Input
    {
        std::string domainId;
        std::string flowId;
        mxlInstance instance = nullptr;
        mxlFlowReader reader = nullptr;
        std::uint64_t next = 0;
        // Video only: how long the flow keeps a grain, and whether the reader waits for its writer.
        std::uint64_t historyNs = 0;
        ReaderWait wait;
    };
    Input input;
    auto close = [](Input& open, bool pinned) {
        if (open.reader != nullptr)
        {
            if (pinned)
            {
                // The encoder page-locked this reader's grains; unlock before they are unmapped.
                gpuReleaseHostMemory();
            }
            mxlReleaseFlowReader(open.instance, open.reader);
        }
        if (open.instance != nullptr)
        {
            mxlDestroyInstance(open.instance);
        }
        open = Input{};
    };
    // Opens the reader for `route` unless it is open already; false when the flow cannot be read yet.
    auto attach = [&](Input& open, Route const& route, bool pinned) -> bool {
        if (open.reader != nullptr && open.domainId == route.domainId && open.flowId == route.flowId)
        {
            return true;
        }
        close(open, pinned);
        auto const domain = resolveDomain(cfg.scanPath, route.domainId);
        if (domain)
        {
            open.instance = mxlCreateInstance(domain->path.c_str(), nullptr);
        }
        if (open.instance == nullptr || mxlCreateFlowReader(open.instance, route.flowId.c_str(), nullptr, &open.reader) != MXL_STATUS_OK)
        {
            close(open, pinned);
            return false;
        }
        open.domainId = route.domainId;
        open.flowId = route.flowId;
        return true;
    };
    // Phase 1 also reads the camera's audio: the samples of each video grain's frame time are
    // stored with that frame (SPECIFICATION.md §4.2), as interleaved stereo (mono doubled).
    bool const withAudio = phase == 1 && camera >= 1 && camera <= static_cast<int>(cfg.cameras.size()) &&
                           cfg.cameras[static_cast<std::size_t>(camera - 1)].audio;
    Input audio;
    std::string audioState;
    // A writer may commit a frame's audio a little after its video: wait up to one frame for
    // it. After three misses in a row, wait once every 50 frames only, so a stalled audio flow
    // costs the video at most one frame per 50.
    std::uint64_t audioMisses = 0;
    auto const audioWaitNs = static_cast<std::uint64_t>(framePeriodNs(cfg.format.rateNum, cfg.format.rateDen));
    // Why the last audio read gave no samples: "" (it did), "off" (not routed, or not 48 kHz),
    // "flow_missing" or "no_samples".
    std::string audioProblem;
    auto audioRoute = [&](Route const& route, std::string const& state) {
        if (state != audioState)
        {
            audioState = state;
            engine_.setRoute(camera, 1, false, Route{true, route.domainId, route.flowId, route.senderId, state});
        }
    };
    auto readAudio = [&](std::uint64_t index) -> std::vector<float> {
        auto const route = engine_.route(camera, 1, false);
        audioProblem = "off";
        if (!route.active || route.flowId.empty() || route.domainId.empty())
        {
            close(audio, false);
            audioState.clear();
            audioMisses = 0;
            return {};
        }
        bool const reopened = audio.reader == nullptr || audio.domainId != route.domainId || audio.flowId != route.flowId;
        if (!attach(audio, route, false))
        {
            audioRoute(route, "waiting");
            audioProblem = "flow_missing";
            return {};
        }
        if (reopened)
        {
            audioMisses = 0;
            mxlFlowConfigInfo info{};
            if (mxlFlowReaderGetConfigInfo(audio.reader, &info) != MXL_STATUS_OK || info.common.grainRate.numerator != 48000 ||
                info.common.grainRate.denominator != 1 || info.continuous.channelCount == 0)
            {
                logWarn("mxl_audio_unsupported", {{"camera", std::to_string(camera)}, {"flow", route.flowId}, {"reason", "needs 48 kHz float32 audio"}});
                close(audio, false);
                audioRoute(route, "unsupported");
                return {};
            }
        }
        auto const first = audioSamplesUntil(index, cfg.format.rateNum, cfg.format.rateDen, 48000);
        auto const end = audioSamplesUntil(index + 1, cfg.format.rateNum, cfg.format.rateDen, 48000);
        auto const count = static_cast<std::size_t>(end - first);
        // The samples of [first, end): MXL addresses `count` samples ending at `end`.
        mxlWrappedMultiBufferSlice slices{};
        bool const wait = audioMisses < 3 || audioMisses % 50 == 0;
        auto const status = count == 0 ? MXL_ERR_INVALID_ARG
                            : wait     ? mxlFlowReaderGetSamples(audio.reader, end, count, audioWaitNs, &slices)
                                       : mxlFlowReaderGetSamplesNonBlocking(audio.reader, end, count, &slices);
        if (status != MXL_STATUS_OK || slices.count == 0)
        {
            audioProblem = "no_samples";
            ++audioMisses;
            if (status == MXL_ERR_FLOW_INVALID)
            {
                // Removed or created again (its writer restarted): open it again with the next frame.
                close(audio, false);
            }
            return {};
        }
        audioMisses = 0;
        std::vector<float> pcm(count * 2, 0.f);
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            auto const source = std::min(channel, slices.count - 1);
            std::size_t sample = 0;
            for (auto const& fragment : slices.base.fragments)
            {
                auto const* in = reinterpret_cast<float const*>(static_cast<std::uint8_t const*>(fragment.pointer) + source * slices.stride);
                for (std::size_t i = 0; i < fragment.size / sizeof(float) && sample < count; ++i, ++sample)
                {
                    pcm[sample * 2 + channel] = in[i];
                }
            }
        }
        audioRoute(route, "running");
        audioProblem.clear();
        return pcm;
    };
    // Logs once when the camera's audio stops while its video records, and again when it is back.
    auto const period = framePeriodNs(cfg.format.rateNum, cfg.format.rateDen);
    AudioWatch audioWatch{static_cast<std::uint64_t>(std::ceil(cfg.inputStallS * 1e9 / static_cast<double>(period)))};
    auto watchAudio = [&](bool samples) {
        if (audioProblem == "off")
        {
            audioWatch.reset();
            return;
        }
        std::uint64_t gap = 0;
        auto const change = audioWatch.frame(samples, &gap);
        if (change == AudioWatch::Change::Stopped)
        {
            logWarn("recording_audio_stopped", {{"camera", std::to_string(camera)}, {"reason", audioProblem}});
        }
        else if (change == AudioWatch::Change::Resumed)
        {
            logInfo("recording_audio_resumed", {{"camera", std::to_string(camera)}, {"gap_ms", std::to_string(gap * static_cast<std::uint64_t>(period) / 1000000)}});
        }
    };
    // Logs once when the camera input stops (InputWatch) and again when it resumes.
    InputWatch watch{static_cast<std::uint64_t>(cfg.inputStallS * 1e9)};
    LogFields const where{{"camera", std::to_string(camera)}, {"phase", std::to_string(phase)}};
    auto stalled = [&](bool open) {
        if (watch.idle(mxlGetTime()))
        {
            auto fields = where;
            fields.emplace_back("reason", watch.reason(open));
            logWarn("recording_stopped", fields);
        }
    };
    // Reads (and encodes) every grain due since the last call; false when nothing was read.
    auto step = [&]() -> bool {
        auto const route = engine_.route(camera, phase, true);
        if (!route.active || route.flowId.empty() || route.domainId.empty())
        {
            close(input, true);
            watch = InputWatch{watch.timeoutNs};
            return false;
        }
        watch.connect(mxlGetTime());
        if (!attach(input, route, true))
        {
            engine_.setRoute(camera, phase, true, Route{true, route.domainId, route.flowId, route.senderId, "waiting"});
            stalled(false);
            return false;
        }
        if (input.historyNs == 0)
        {
            // A grain is readable while the writer writes the next grainCount - 1 grains.
            mxlFlowConfigInfo info{};
            if (mxlFlowReaderGetConfigInfo(input.reader, &info) == MXL_STATUS_OK && info.common.grainRate.numerator > 0 && info.discrete.grainCount > 1)
            {
                auto const period = framePeriodNs(static_cast<int>(info.common.grainRate.numerator), static_cast<int>(info.common.grainRate.denominator));
                input.historyNs = (info.discrete.grainCount - 1) * static_cast<std::uint64_t>(period);
            }
        }
        // Stay two grains behind the current index, as before, and read every grain since the last one.
        auto const now = mxlTimestampToIndex(&rate, mxlGetTime());
        auto const last = now > 2 ? now - 2 : now;
        if (input.next == 0 || input.next > last + 1)
        {
            input.next = last;
        }
        bool read = false;
        for (; input.next <= last; ++input.next)
        {
            mxlGrainInfo info{};
            std::uint8_t* payload = nullptr;
            auto const status = mxlFlowReaderGetGrainNonBlocking(input.reader, input.next, &info, &payload);
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
            {
                // Gone from the history. Not a drop when the reader was waiting for it: the
                // writer started (or resumed) after it, so it was never written.
                if (input.wait.lateIsDrop(mxlGetTime(), input.historyNs))
                {
                    engine_.countDropped(camera, 1);
                }
                continue;
            }
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
            {
                // Still incomplete although the writer's head is past it: a writer that started
                // or resumed after this grain never writes it (one that moves on marks a grain
                // it gave up as invalid). Skip it instead of waiting until the history drops it.
                mxlFlowRuntimeInfo runtime{};
                if (mxlFlowReaderGetRuntimeInfo(input.reader, &runtime) == MXL_STATUS_OK && runtime.headIndex > input.next &&
                    mxlFlowReaderGetGrainNonBlocking(input.reader, input.next, &info, &payload) == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
                {
                    ++watch.skipped;
                    continue;
                }
                input.wait.wait(mxlGetTime());
                break;
            }
            if (status != MXL_STATUS_OK)
            {
                close(input, true);
                engine_.setRoute(camera, phase, true, Route{true, route.domainId, route.flowId, route.senderId, "waiting"});
                break;
            }
            input.wait.read();
            if (payload != nullptr && (info.flags & MXL_GRAIN_FLAG_INVALID) == 0)
            {
                if (withAudio)
                {
                    // Taken by the next frame this phase stores, which is this grain's.
                    auto pcm = readAudio(input.next);
                    watchAudio(!pcm.empty());
                    engine_.ingestAudio(camera, mxlIndexToTimestamp(&rate, input.next), std::move(pcm), 2);
                }
                engine_.ingestV210(camera, phase, mxlIndexToTimestamp(&rate, input.next), payload, info.grainSize);
                engine_.setRoute(camera, phase, true, Route{true, route.domainId, route.flowId, route.senderId, "running"});
                read = true;
                if (auto const gap = watch.recorded(mxlGetTime()); gap != 0)
                {
                    auto fields = where;
                    fields.emplace_back("gap_ms", std::to_string(gap / 1000000));
                    logInfo("recording_resumed", fields);
                }
            }
            else
            {
                ++watch.skipped;
            }
        }
        if (!read)
        {
            stalled(input.reader != nullptr);
        }
        return read;
    };
    while (run_.load())
    {
        if (!step())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    close(input, true);
    close(audio, false);
#else
    (void)engine_;
    (void)camera;
    (void)phase;
#endif
}
} // namespace replay
