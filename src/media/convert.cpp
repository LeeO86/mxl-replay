#include "media/convert.hpp"

#include "media/jpeg.hpp"
#include "media/scale.hpp"

#include <algorithm>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
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
        avcodec_open2(vctx, codec, nullptr);
        sws = sws_getContext(vctx->width, vctx->height, vctx->pix_fmt, house.width, house.height, AV_PIX_FMT_YUV422P, SWS_BILINEAR, nullptr, nullptr, nullptr);
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
    while (av_read_frame(fmt, packet) >= 0)
    {
        if (vctx != nullptr && packet->stream_index == video)
        {
            if (avcodec_send_packet(vctx, packet) >= 0)
            {
                while (avcodec_receive_frame(vctx, frame) >= 0)
                {
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, yuv->data, yuv->linesize);
                    out.frames.push_back(fromYuv(yuv->data[0], yuv->data[1], yuv->data[2], house.width, house.height, yuv->linesize[0], yuv->linesize[1],
                        yuv->linesize[2]));
                }
            }
        }
        else if (actx != nullptr && packet->stream_index == audio && swr != nullptr)
        {
            if (avcodec_send_packet(actx, packet) >= 0)
            {
                while (avcodec_receive_frame(actx, frame) >= 0)
                {
                    std::vector<float> interleaved(static_cast<std::size_t>(frame->nb_samples) * 2);
                    std::uint8_t* dest[1] = {reinterpret_cast<std::uint8_t*>(interleaved.data())};
                    auto** inNonConst = frame->extended_data;
                    int const got = swr_convert(swr, dest, frame->nb_samples, const_cast<std::uint8_t const**>(inNonConst), frame->nb_samples);
                    if (got > 0)
                    {
                        out.audio.insert(out.audio.end(), interleaved.begin(), interleaved.begin() + static_cast<std::ptrdiff_t>(got * 2));
                    }
                }
            }
        }
        av_packet_unref(packet);
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
    if (out.frames.empty())
    {
        out.error = "no video frames";
    }
    return out;
}
} // namespace replay
