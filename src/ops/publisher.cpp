#include "ops/publisher.hpp"

#include "flow/cuda_flow.hpp"
#include "util/logging.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#if defined(REPLAY_WITH_CUDA)
#include <libavutil/hwcontext_cuda.h>
#endif
}

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <mutex>
#include <thread>

namespace replay
{
namespace
{
// CBR: 200 kbit per picture at 25 pictures a second.
constexpr std::int64_t kBitrate = 5'000'000;

std::string avError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

// FFmpeg's warnings go to this process's log, and the last error explains a failed open (for NVENC: no
// libnvidia-encode, no capable GPU).
std::mutex gAvMu;
std::string gAvLast;

void avLog(void* context, int level, char const* format, va_list args)
{
    if (level > AV_LOG_WARNING)
    {
        return;
    }
    char line[1024] = {};
    int prefix = 1;
    av_log_format_line2(context, level, format, args, line, sizeof(line), &prefix);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    if (text.empty())
    {
        return;
    }
    if (level <= AV_LOG_ERROR)
    {
        std::lock_guard lock{gAvMu};
        gAvLast = text;
    }
    logMessage(level <= AV_LOG_ERROR ? LogLevel::Warn : LogLevel::Debug, "ffmpeg", {{"message", text}});
}

std::string lastAvError(int code)
{
    std::lock_guard lock{gAvMu};
    auto text = gAvLast.empty() ? avError(code) : gAvLast + " (" + avError(code) + ")";
    gAvLast.clear();
    return text;
}
} // namespace

struct PreviewPublisher::Impl
{
    PreviewMosaic const& mosaic;
    std::string url;
    AVRational fps;
    std::function<void(double)> observe;
    std::thread thread;
    std::mutex runMu;
    std::condition_variable wake;
    bool stopping = false;
    mutable std::mutex statusMu;
    PreviewStatus status;

    AVBufferRef* device = nullptr;
    AVBufferRef* frames = nullptr;
    AVCodecContext* enc = nullptr;
    AVFormatContext* out = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    bool nvenc = false;
    bool nvencFailed = false;
    bool idr = true;

    Impl(PreviewMosaic const& mosaicIn, std::string urlIn, int fpsNum, int fpsDen)
        : mosaic(mosaicIn)
        , url(std::move(urlIn))
        , fps(AVRational{std::max(1, fpsNum), std::max(1, fpsDen)})
    {
    }

    void setState(std::string const& state, std::string const& error)
    {
        std::lock_guard lock{statusMu};
        if (state == "error" && status.error != error)
        {
            logWarn("preview_publish_error", {{"url", url}, {"error", error}});
        }
        else if (state == "publishing" && status.state != "publishing")
        {
            logInfo("preview_publishing", {{"url", url}, {"encoder", status.encoder}});
        }
        status.state = state;
        status.error = error;
    }

    AVCodecContext* context(AVCodec const* codec) const
    {
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        auto const& layout = mosaic.layout();
        ctx->width = layout.width;
        ctx->height = layout.height;
        ctx->time_base = AVRational{fps.den, fps.num};
        ctx->framerate = fps;
        ctx->color_range = AVCOL_RANGE_MPEG;
        ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_BT709;
        ctx->bit_rate = kBitrate;
        ctx->rc_max_rate = kBitrate;
        ctx->rc_buffer_size = static_cast<int>(kBitrate * fps.den / fps.num * 2);
        // One second GOP: a new WHEP viewer has a picture within a second. No B-frames.
        ctx->gop_size = std::max(1, static_cast<int>(std::lround(av_q2d(fps))));
        ctx->max_b_frames = 0;
        // SPS and PPS in the RTSP SDP; MediaMTX repeats them for WebRTC readers.
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        return ctx;
    }

    // h264_nvenc on CUDA frames in the process's own context: the canvas is copied on the GPU, the
    // encoder reads it there. Empty when it is open, else the reason it is not.
    std::string openNvenc(AVCodecContext* ctx)
    {
#if defined(REPLAY_WITH_CUDA)
        void* cuda = gpuContext();
        if (cuda == nullptr)
        {
            return "no CUDA context";
        }
        device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
        static_cast<AVCUDADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx)->cuda_ctx = static_cast<CUcontext>(cuda);
        int rc = av_hwdevice_ctx_init(device);
        if (rc < 0)
        {
            return "CUDA device: " + lastAvError(rc);
        }
        frames = av_hwframe_ctx_alloc(device);
        auto* pool = reinterpret_cast<AVHWFramesContext*>(frames->data);
        pool->format = AV_PIX_FMT_CUDA;
        pool->sw_format = AV_PIX_FMT_NV12;
        pool->width = ctx->width;
        pool->height = ctx->height;
        rc = av_hwframe_ctx_init(frames);
        if (rc < 0)
        {
            return "CUDA frames: " + lastAvError(rc);
        }
        ctx->pix_fmt = AV_PIX_FMT_CUDA;
        ctx->hw_frames_ctx = av_buffer_ref(frames);
        // The settings of mxl-webrtc-monitor and the FlowXer engine: CBR, fastest preset, ultra-low latency.
        AVDictionary* options = nullptr;
        av_dict_set(&options, "preset", "p1", 0);
        av_dict_set(&options, "tune", "ull", 0);
        av_dict_set(&options, "rc", "cbr", 0);
        av_dict_set(&options, "zerolatency", "1", 0);
        av_dict_set(&options, "delay", "0", 0);
        av_dict_set(&options, "forced-idr", "1", 0);
        rc = avcodec_open2(ctx, avcodec_find_encoder_by_name("h264_nvenc"), &options);
        av_dict_free(&options);
        return rc < 0 ? "h264_nvenc: " + lastAvError(rc) : std::string{};
#else
        (void)ctx;
        return "built without CUDA";
#endif
    }

    bool openEncoder()
    {
        if (!nvencFailed)
        {
            std::string reason = avcodec_find_encoder_by_name("h264_nvenc") == nullptr ? "h264_nvenc is not in this FFmpeg build"
                                 : !mosaic.onDevice()                                 ? "no CUDA device"
                                                                                      : std::string{};
            if (reason.empty())
            {
                AVCodecContext* ctx = context(avcodec_find_encoder_by_name("h264_nvenc"));
                reason = openNvenc(ctx);
                if (reason.empty())
                {
                    enc = ctx;
                    nvenc = true;
                    return opened("nvenc");
                }
                avcodec_free_context(&ctx);
                av_buffer_unref(&frames);
                av_buffer_unref(&device);
            }
            nvencFailed = true;
            logWarn("preview_nvenc_unavailable", {{"reason", reason}, {"fallback", "x264"}});
        }
        AVCodec const* x264 = avcodec_find_encoder_by_name("libx264");
        if (x264 == nullptr)
        {
            setState("error", "no H.264 encoder: h264_nvenc unavailable and libx264 not in this FFmpeg build");
            return false;
        }
        AVCodecContext* ctx = context(x264);
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "preset", "ultrafast", 0);
        av_dict_set(&options, "tune", "zerolatency", 0);
        av_dict_set(&options, "forced-idr", "1", 0);
        int const rc = avcodec_open2(ctx, x264, &options);
        av_dict_free(&options);
        if (rc < 0)
        {
            setState("error", "libx264: " + lastAvError(rc));
            avcodec_free_context(&ctx);
            return false;
        }
        enc = ctx;
        nvenc = false;
        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_NV12;
        frame->width = ctx->width;
        frame->height = ctx->height;
        if (av_frame_get_buffer(frame, 0) < 0)
        {
            closeEncoder();
            setState("error", "no memory for a preview picture");
            return false;
        }
        return opened("x264");
    }

    bool opened(char const* name)
    {
        packet = packet != nullptr ? packet : av_packet_alloc();
        {
            std::lock_guard lock{statusMu};
            status.encoder = name;
        }
        logInfo("preview_encoder", {{"encoder", name}, {"width", std::to_string(enc->width)}, {"height", std::to_string(enc->height)},
                                       {"fps", std::to_string(av_q2d(fps))}, {"bitrate", std::to_string(kBitrate)}});
        return true;
    }

    void closeEncoder()
    {
        avcodec_free_context(&enc);
        av_frame_free(&frame);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
    }

    bool openOutput()
    {
        AVFormatContext* ctx = nullptr;
        int rc = avformat_alloc_output_context2(&ctx, nullptr, "rtsp", url.c_str());
        if (rc < 0 || ctx == nullptr)
        {
            setState("error", "rtsp output: " + lastAvError(rc));
            return false;
        }
        AVStream* stream = avformat_new_stream(ctx, nullptr);
        avcodec_parameters_from_context(stream->codecpar, enc);
        stream->time_base = enc->time_base;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        // Socket timeout in microseconds: a stalled MediaMTX holds this thread for 2 s at most.
        av_dict_set(&options, "timeout", "2000000", 0);
        rc = avformat_write_header(ctx, &options);
        av_dict_free(&options);
        if (rc < 0)
        {
            setState("error", lastAvError(rc));
            avformat_free_context(ctx);
            return false;
        }
        out = ctx;
        idr = true;
        return true;
    }

    void closeOutput()
    {
        if (out != nullptr)
        {
            // The RTSP muxer sends TEARDOWN in the trailer, also after an error.
            av_write_trailer(out);
            avformat_free_context(out);
            out = nullptr;
        }
    }

    // Copy, encode and send one picture. False when the encoder or the connection failed.
    bool publish(std::int64_t pts)
    {
        AVFrame* picture = frame;
        AVFrame* onDevice = nullptr;
        if (nvenc)
        {
            onDevice = av_frame_alloc();
            if (av_hwframe_get_buffer(frames, onDevice, 0) < 0 ||
                !mosaic.copyTo(onDevice->data[0], onDevice->linesize[0], onDevice->data[1], onDevice->linesize[1], true))
            {
                av_frame_free(&onDevice);
                setState("error", "cannot copy the mosaic on the GPU");
                return false;
            }
            picture = onDevice;
        }
        else if (av_frame_make_writable(frame) < 0 || !mosaic.copyTo(frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], false))
        {
            setState("error", "cannot copy the mosaic");
            return false;
        }
        picture->pts = pts;
        // A new connection starts with an IDR picture.
        picture->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        idr = false;
        int rc = avcodec_send_frame(enc, picture);
        av_frame_free(&onDevice);
        if (rc < 0)
        {
            setState("error", "encode: " + lastAvError(rc));
            closeOutput();
            closeEncoder();
            return false;
        }
        std::uint64_t sent = 0;
        while ((rc = avcodec_receive_packet(enc, packet)) == 0)
        {
            packet->stream_index = 0;
            av_packet_rescale_ts(packet, enc->time_base, out->streams[0]->time_base);
            rc = av_interleaved_write_frame(out, packet);
            av_packet_unref(packet);
            if (rc < 0)
            {
                setState("error", "rtsp write: " + lastAvError(rc));
                closeOutput();
                return false;
            }
            ++sent;
        }
        if (sent > 0)
        {
            {
                std::lock_guard lock{statusMu};
                status.frames += sent;
            }
            setState("publishing", "");
        }
        return true;
    }

    void loop()
    {
        pthread_setname_np(pthread_self(), "rp-preview");
        using Clock = std::chrono::steady_clock;
        auto const period = std::chrono::nanoseconds(1'000'000'000LL * fps.den / fps.num);
        auto const start = Clock::now();
        auto next = start;
        Clock::time_point retryEncoder{};
        Clock::time_point retryOutput{};
        std::int64_t lastPts = -1;
        for (;;)
        {
            next += period;
            if (next < Clock::now())
            {
                next = Clock::now() + period;
            }
            {
                std::unique_lock lock{runMu};
                if (wake.wait_until(lock, next, [this] { return stopping; }))
                {
                    break;
                }
            }
            auto const now = Clock::now();
            // A failed open is tried again after a while (encoder 5 s, connection 2 s).
            if (enc == nullptr && (now < retryEncoder || !openEncoder()))
            {
                retryEncoder = now < retryEncoder ? retryEncoder : now + std::chrono::seconds(5);
                continue;
            }
            if (out == nullptr && (now < retryOutput || !openOutput()))
            {
                retryOutput = now < retryOutput ? retryOutput : now + std::chrono::seconds(2);
                continue;
            }
            // Timestamps follow the clock, so a skipped picture does not slow the stream down.
            std::int64_t const pts = std::max(lastPts + 1, static_cast<std::int64_t>((now - start) / period));
            lastPts = pts;
            auto const began = Clock::now();
            if (!publish(pts))
            {
                retryOutput = now + std::chrono::seconds(1);
                continue;
            }
            if (observe)
            {
                observe(std::chrono::duration<double>(Clock::now() - began).count());
            }
        }
        closeOutput();
        closeEncoder();
        av_packet_free(&packet);
    }
};

PreviewPublisher::PreviewPublisher(PreviewMosaic const& mosaic, std::string url, int fpsNum, int fpsDen)
    : impl_(std::make_unique<Impl>(mosaic, std::move(url), fpsNum, fpsDen))
{
    static std::once_flag logging;
    std::call_once(logging, [] { av_log_set_callback(avLog); });
}

PreviewPublisher::~PreviewPublisher()
{
    stop();
}

void PreviewPublisher::start(std::function<void(double)> observe)
{
    impl_->observe = std::move(observe);
    impl_->thread = std::thread([this] { impl_->loop(); });
}

void PreviewPublisher::stop()
{
    {
        std::lock_guard lock{impl_->runMu};
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}

PreviewStatus PreviewPublisher::status() const
{
    std::lock_guard lock{impl_->statusMu};
    return impl_->status;
}
} // namespace replay
