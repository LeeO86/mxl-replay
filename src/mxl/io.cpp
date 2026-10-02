#include "mxl/io.hpp"

#include "domain/scan.hpp"
#include "media/timebase.hpp"
#include "media/v210.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <chrono>
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
    if (!ensureOutputDomain(cfg.outputDomainDir, cfg.outputDomainId, 2000000000ull))
    {
        logError("mxl_domain_rejected", {{"dir", cfg.outputDomainDir}});
        return;
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
    thread_ = std::thread([this] { readLoop(); });
    logInfo("mxl_started", {{"domain", cfg.outputDomainId}});
#endif
}

void MxlBridge::stop()
{
    run_ = false;
    if (thread_.joinable())
    {
        thread_.join();
    }
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
        mxlMutableWrappedMultiBufferSlice slices{};
        if (mxlFlowWriterOpenSamples(writer.audio, index, frame.audio.size() / 2, &slices) == MXL_STATUS_OK)
        {
            auto* dst = static_cast<float*>(slices.base.fragments[0].pointer);
            if (dst != nullptr)
            {
                std::memcpy(dst, frame.audio.data(), std::min(slices.base.fragments[0].size, frame.audio.size() * sizeof(float)));
            }
            mxlFlowWriterCommitSamples(writer.audio);
        }
    }
#endif
}

void MxlBridge::readLoop()
{
#if defined(REPLAY_WITH_MXL)
    auto const& cfg = engine_.config();
    mxlRational const rate{cfg.format.rateNum, cfg.format.rateDen};
    while (run_.load())
    {
        for (auto const& camera : cfg.cameras)
        {
            for (int phase = 1; phase <= camera.phases; ++phase)
            {
                auto const route = engine_.route(camera.index, phase, true);
                if (!route.active || route.flowId.empty() || route.domainId.empty())
                {
                    continue;
                }
                auto const domain = resolveDomain(cfg.scanPath, route.domainId);
                if (!domain || domain->mirror)
                {
                    engine_.setRoute(camera.index, phase, true, Route{true, route.domainId, route.flowId, route.senderId, "waiting"});
                    continue;
                }
                mxlInstance readerInstance = mxlCreateInstance(domain->path.c_str(), nullptr);
                if (readerInstance == nullptr)
                {
                    continue;
                }
                mxlFlowReader reader = nullptr;
                if (mxlCreateFlowReader(readerInstance, route.flowId.c_str(), nullptr, &reader) != MXL_STATUS_OK)
                {
                    mxlDestroyInstance(readerInstance);
                    engine_.setRoute(camera.index, phase, true, Route{true, route.domainId, route.flowId, route.senderId, "waiting"});
                    continue;
                }
                auto const now = mxlGetTime();
                auto const index = mxlTimestampToIndex(&rate, now);
                mxlGrainInfo info{};
                std::uint8_t* payload = nullptr;
                if (mxlFlowReaderGetGrainNonBlocking(reader, index > 2 ? index - 2 : index, &info, &payload) == MXL_STATUS_OK && payload != nullptr &&
                    (info.flags & MXL_GRAIN_FLAG_INVALID) == 0)
                {
                    engine_.ingestV210(camera.index, phase, mxlIndexToTimestamp(&rate, index), payload, info.grainSize);
                    engine_.setRoute(camera.index, phase, true, Route{true, route.domainId, route.flowId, route.senderId, "running"});
                }
                mxlReleaseFlowReader(readerInstance, reader);
                mxlDestroyInstance(readerInstance);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#else
    (void)engine_;
#endif
}
} // namespace replay
