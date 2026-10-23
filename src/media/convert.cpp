#include "media/convert.hpp"

#include "media/jpeg.hpp"
#include "media/scale.hpp"

#include <algorithm>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace replay
{
namespace
{
struct MemFile
{
    std::uint8_t const* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
};

int readPacket(void* opaque, std::uint8_t* buf, int bufSize)
{
    auto* file = static_cast<MemFile*>(opaque);
    if (file->pos >= file->size)
    {
        return AVERROR_EOF;
    }
    int const n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(bufSize), file->size - file->pos));
    std::copy(file->data + file->pos, file->data + file->pos + static_cast<std::size_t>(n), buf);
    file->pos += static_cast<std::size_t>(n);
    return n;
}

Frame10 fromYuv(std::uint8_t const* y, std::uint8_t const* u, std::uint8_t const* v, int width, int height, int ys, int us, int vs)
{
    Frame10 frame;
    frame.allocate(width, height);
    int const cw = width / 2;
    for (int row = 0; row < height; ++row)
    {
        for (int x = 0; x < width; ++x)
        {
            frame.y[static_cast<std::size_t>(row * width + x)] = static_cast<std::uint16_t>(y[row * ys + x]) << 2;
        }
        for (int x = 0; x < cw; ++x)
        {
            frame.cb[static_cast<std::size_t>(row * cw + x)] = static_cast<std::uint16_t>(u[row * us + x]) << 2;
            frame.cr[static_cast<std::size_t>(row * cw + x)] = static_cast<std::uint16_t>(v[row * vs + x]) << 2;
        }
    }
    return frame;
}
} // namespace

ConvertedMedia convertUpload(std::uint8_t const* data, std::size_t size, VideoFormat const& house)
{
    ConvertedMedia out;
    if (data == nullptr || size < 4)
    {
        out.error = "empty upload";
        return out;
    }
    if (data[0] == 0xff && data[1] == 0xd8)
    {
        Frame10 decoded;
        if (!decodeJpeg422(data, size, decoded))
        {
            out.error = "jpeg decode failed";
            return out;
        }
        Frame10 scaled;
        scaled.allocate(house.width, house.height);
        bool const mismatch = decoded.width != house.width || decoded.height != house.height;
        scaleFrame(decoded, scaled, ScaleFilter::Bilinear);
        out.frames.push_back(std::move(scaled));
        out.order.push_back(0);
        if (mismatch)
        {
            out.error = "scaled";
        }
        return out;
    }
    MemFile mem{data, size, 0};
    auto* io = avio_alloc_context(static_cast<unsigned char*>(av_malloc(4096)), 4096, 0, &mem, readPacket, nullptr, nullptr);
    if (io == nullptr)
    {
        out.error = "avio alloc failed";
        return out;
    }
    AVFormatContext* fmt = avformat_alloc_context();
    fmt->pb = io;
    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0)
    {
        avformat_free_context(fmt);
        av_freep(&io->buffer);
        avio_context_free(&io);
        out.error = "unrecognised media";
        return out;
    }
    avformat_find_stream_info(fmt, nullptr);
    int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVCodecContext* vctx = nullptr;
    AVCodecContext* actx = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    if (video >= 0)
    {
        AVStream* stream = fmt->streams[video];
        AVCodec const* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        vctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(vctx, stream->codecpar);
        bool const opened = codec != nullptr && avcodec_open2(vctx, codec, nullptr) >= 0;
        // sws_getContext asserts (aborts the process) on AV_PIX_FMT_NONE: check the input first.
        if (opened && vctx->width > 0 && vctx->height > 0 && vctx->pix_fmt != AV_PIX_FMT_NONE && sws_isSupportedInput(vctx->pix_fmt) > 0)
        {
            sws = sws_getContext(vctx->width, vctx->height, vctx->pix_fmt, house.width, house.height, AV_PIX_FMT_YUV422P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        }
        if (sws == nullptr)
        {
            // No decoder or no scaler for this picture (unknown codec or pixel format, no size): fail the
            // upload with the reason, do not read it.
            char const* name = vctx->pix_fmt != AV_PIX_FMT_NONE ? av_get_pix_fmt_name(vctx->pix_fmt) : nullptr;
            auto const codecName = stream->codecpar->codec_id == AV_CODEC_ID_NONE ? std::string("an unknown codec") : std::string(avcodec_get_name(stream->codecpar->codec_id));
            out.error = !opened ? "unsupported video: no decoder for " + codecName
                                : "unsupported video: cannot convert " + std::string(name != nullptr ? name : "an unknown pixel format") + " " +
                                      std::to_string(vctx->width) + "x" + std::to_string(vctx->height) + " to the house format";
        }
    }
    if (audio >= 0)
    {
        AVStream* stream = fmt->streams[audio];
        AVCodec const* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        actx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(actx, stream->codecpar);
        avcodec_open2(actx, codec, nullptr);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &actx->ch_layout, actx->sample_fmt, actx->sample_rate, 0, nullptr);
        if (swr != nullptr)
        {
            swr_init(swr);
        }
        out.channels = 2;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUV422P;
    yuv->width = house.width;
    yuv->height = house.height;
    av_frame_get_buffer(yuv, 32);
    // Frame-rate conversion to the house rate (1.8.1, FrameRateConverter): each source frame is kept
    // once in out.frames, and out.order lists the one each house frame shows. A frame without a
    // timestamp follows the previous one by its duration (the stream's frame rate, else the house's).
    FrameRateConverter rate(house.rateNum, house.rateDen);
    AVRational const nanos{1, 1000000000};
    AVRational const timeBase = video >= 0 ? fmt->streams[video]->time_base : AVRational{1, 1};
    AVRational const streamRate = video >= 0 ? fmt->streams[video]->avg_frame_rate : AVRational{0, 1};
    std::int64_t const fallbackNs = streamRate.num > 0 && streamRate.den > 0 ? av_rescale(1000000000LL, streamRate.den, streamRate.num)
                                                                              : av_rescale(1000000000LL, house.rateDen, house.rateNum);
    std::int64_t firstPts = AV_NOPTS_VALUE;
    SourceClock clock{fallbackNs};
    auto take = [&](AVFrame* decoded) {
        sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height, yuv->data, yuv->linesize);
        auto const pts = decoded->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE && firstPts == AV_NOPTS_VALUE)
        {
            firstPts = pts;
        }
        auto const ns = clock.start(pts != AV_NOPTS_VALUE ? std::max<std::int64_t>(0, av_rescale_q(pts - firstPts, timeBase, nanos)) : -1,
            decoded->duration > 0 ? av_rescale_q(decoded->duration, timeBase, nanos) : 0);
        out.frames.push_back(fromYuv(yuv->data[0], yuv->data[1], yuv->data[2], house.width, house.height, yuv->linesize[0], yuv->linesize[1], yuv->linesize[2]));
        rate.add(static_cast<std::uint32_t>(out.frames.size() - 1), ns, out.order);
    };
    // Without a scaler for its video the file is not read at all (the upload fails).
    bool const readable = video < 0 || sws != nullptr;
    while (readable && av_read_frame(fmt, packet) >= 0)
    {
        if (vctx != nullptr && packet->stream_index == video)
        {
            if (avcodec_send_packet(vctx, packet) >= 0)
            {
                while (avcodec_receive_frame(vctx, frame) >= 0)
                {
                    take(frame);
                }
            }
        }
        else if (actx != nullptr && packet->stream_index == audio && swr != nullptr)
        {
            if (avcodec_send_packet(actx, packet) >= 0)
            {
                while (avcodec_receive_frame(actx, frame) >= 0)
                {
                    // Room for every output sample (44.1 kHz gives more samples at 48 kHz; the resampler kept the rest
                    // until the end of the file before 1.6.0).
                    std::vector<float> interleaved(static_cast<std::size_t>(swr_get_out_samples(swr, frame->nb_samples)) * 2 + 2);
                    std::uint8_t* dest[1] = {reinterpret_cast<std::uint8_t*>(interleaved.data())};
                    auto** inNonConst = frame->extended_data;
                    int const got = swr_convert(swr, dest, static_cast<int>(interleaved.size() / 2), const_cast<std::uint8_t const**>(inNonConst), frame->nb_samples);
                    if (got > 0)
                    {
                        out.audio.insert(out.audio.end(), interleaved.begin(), interleaved.begin() + static_cast<std::ptrdiff_t>(got * 2));
                    }
                }
            }
        }
        av_packet_unref(packet);
    }
    // Drain the video decoder (frames it still holds), then the last source frame for its own duration.
    if (vctx != nullptr && readable && avcodec_send_packet(vctx, nullptr) >= 0)
    {
        while (avcodec_receive_frame(vctx, frame) >= 0)
        {
            take(frame);
        }
    }
    if (out.frames.size() == 1)
    {
        out.order.assign(1, 0); // a still image: one frame, as before
    }
    else
    {
        rate.finish(clock.endNs(), out.order);
    }
    // Drain the audio decoder, then the resampler's delay line (the last milliseconds of the file).
    if (actx != nullptr && swr != nullptr && avcodec_send_packet(actx, nullptr) >= 0)
    {
        while (avcodec_receive_frame(actx, frame) >= 0)
        {
            std::vector<float> interleaved(static_cast<std::size_t>(swr_get_out_samples(swr, frame->nb_samples)) * 2 + 2);
            std::uint8_t* dest[1] = {reinterpret_cast<std::uint8_t*>(interleaved.data())};
            int const got = swr_convert(swr, dest, static_cast<int>(interleaved.size() / 2), const_cast<std::uint8_t const**>(frame->extended_data), frame->nb_samples);
            if (got > 0)
            {
                out.audio.insert(out.audio.end(), interleaved.begin(), interleaved.begin() + static_cast<std::ptrdiff_t>(got * 2));
            }
        }
    }
    while (swr != nullptr)
    {
        std::vector<float> tail(4096 * 2);
        std::uint8_t* dest[1] = {reinterpret_cast<std::uint8_t*>(tail.data())};
        int const got = swr_convert(swr, dest, 4096, nullptr, 0);
        if (got <= 0)
        {
            break;
        }
        out.audio.insert(out.audio.end(), tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(got * 2));
    }
    av_frame_free(&yuv);
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws);
    swr_free(&swr);
    avcodec_free_context(&vctx);
    avcodec_free_context(&actx);
    avformat_close_input(&fmt);
    av_freep(&io->buffer);
    avio_context_free(&io);
    out.audioOnly = video < 0 && audio >= 0;
    if (out.error.empty() && out.frames.empty() && !(out.audioOnly && !out.audio.empty()))
    {
        out.error = out.audioOnly ? "no audio samples" : "no video frames";
    }
    return out;
}
} // namespace replay
