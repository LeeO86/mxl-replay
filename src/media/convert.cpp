#include "media/convert.hpp"

#include "media/jpeg.hpp"
#include "media/scale.hpp"
#include "media/timebase.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

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

// Seekable like a file (1.8.2): an MP4 with its index in front (faststart) did not get its pixel format
// from a reader that cannot seek ("cannot convert an unknown pixel format").
std::int64_t seekPacket(void* opaque, std::int64_t offset, int whence)
{
    auto* file = static_cast<MemFile*>(opaque);
    auto const size = static_cast<std::int64_t>(file->size);
    switch (whence & ~AVSEEK_FORCE)
    {
    case AVSEEK_SIZE:
        return size;
    case SEEK_SET:
        break;
    case SEEK_CUR:
        offset += static_cast<std::int64_t>(file->pos);
        break;
    case SEEK_END:
        offset += size;
        break;
    default:
        return AVERROR(EINVAL);
    }
    if (offset < 0 || offset > size)
    {
        return AVERROR(EINVAL);
    }
    file->pos = static_cast<std::size_t>(offset);
    return offset;
}

// 8-bit planar 4:2:2 into a 10-bit frame of the same size (allocated once, reused per frame).
void fromYuv(Frame10& frame, std::uint8_t const* y, std::uint8_t const* u, std::uint8_t const* v, int width, int height, int ys, int us, int vs)
{
    if (frame.width != width || frame.height != height)
    {
        frame.allocate(width, height);
    }
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
}

// One demuxer over the upload: the file on disk, or a buffer in memory (its own read position).
struct Demuxer
{
    MemFile mem;
    AVIOContext* io = nullptr;
    AVFormatContext* fmt = nullptr;

    bool open(UploadSource const& source)
    {
        fmt = avformat_alloc_context();
        if (fmt == nullptr)
        {
            return false;
        }
        if (source.path.empty())
        {
            mem = MemFile{source.data, source.size, 0};
            io = avio_alloc_context(static_cast<unsigned char*>(av_malloc(65536)), 65536, 0, &mem, readPacket, nullptr, seekPacket);
            if (io == nullptr)
            {
                avformat_free_context(fmt);
                fmt = nullptr;
                return false;
            }
            fmt->pb = io;
        }
        if (avformat_open_input(&fmt, source.path.empty() ? nullptr : source.path.c_str(), nullptr, nullptr) < 0)
        {
            fmt = nullptr; // freed by avformat_open_input
            return false;
        }
        avformat_find_stream_info(fmt, nullptr);
        return true;
    }
    ~Demuxer()
    {
        avformat_close_input(&fmt);
        if (io != nullptr)
        {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

// The audio of an upload as 48 kHz stereo float, read from its own demuxer, as far as asked for: the
// video and the audio can be interleaved in any way in the file, and only the samples not yet taken stay in memory.
class AudioReader
{
public:
    ~AudioReader()
    {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        swr_free(&swr_);
        avcodec_free_context(&ctx_);
    }

    bool open(UploadSource const& source, int stream)
    {
        if (!demux_.open(source) || stream >= static_cast<int>(demux_.fmt->nb_streams))
        {
            return false;
        }
        stream_ = stream;
        for (unsigned i = 0; i < demux_.fmt->nb_streams; ++i)
        {
            demux_.fmt->streams[i]->discard = static_cast<int>(i) == stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        }
        AVStream* s = demux_.fmt->streams[stream];
        AVCodec const* codec = avcodec_find_decoder(s->codecpar->codec_id);
        ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx_, s->codecpar);
        if (codec == nullptr || avcodec_open2(ctx_, codec, nullptr) < 0)
        {
            return false;
        }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, 48000, &ctx_->ch_layout, ctx_->sample_fmt, ctx_->sample_rate, 0, nullptr);
        if (swr_ == nullptr || swr_init(swr_) < 0)
        {
            return false;
        }
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        return packet_ != nullptr && frame_ != nullptr;
    }

    // Moves the next `samples` stereo samples (fewer at the end of the audio) to `out`.
    void take(std::size_t samples, std::vector<float>& out)
    {
        while (buffered() < samples * 2 && more())
        {
        }
        auto const n = std::min(samples * 2, buffered());
        out.assign(fifo_.begin() + static_cast<std::ptrdiff_t>(head_), fifo_.begin() + static_cast<std::ptrdiff_t>(head_ + n));
        head_ += n;
        if (head_ > 65536 && head_ * 2 > fifo_.size())
        {
            fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
    }

private:
    [[nodiscard]] std::size_t buffered() const { return fifo_.size() - head_; }

    void convert(AVFrame const* in)
    {
        // Room for every output sample (44.1 kHz gives more samples at 48 kHz; the resampler kept the rest
        // until the end of the file before 1.6.0).
        int const room = swr_get_out_samples(swr_, in != nullptr ? in->nb_samples : 0) + 1;
        auto const at = fifo_.size();
        fifo_.resize(at + static_cast<std::size_t>(room) * 2);
        std::uint8_t* dest[1] = {reinterpret_cast<std::uint8_t*>(fifo_.data() + at)};
        int const got = swr_convert(swr_, dest, room, in != nullptr ? const_cast<std::uint8_t const**>(in->extended_data) : nullptr, in != nullptr ? in->nb_samples : 0);
        fifo_.resize(at + static_cast<std::size_t>(std::max(got, 0)) * 2);
        if (in == nullptr && got <= 0)
        {
            drained_ = true;
        }
    }

    // Decodes one more packet (or drains the decoder, then the resampler's delay line). False at the end.
    bool more()
    {
        if (drained_)
        {
            return false;
        }
        if (!eof_)
        {
            if (av_read_frame(demux_.fmt, packet_) >= 0)
            {
                if (packet_->stream_index == stream_ && avcodec_send_packet(ctx_, packet_) >= 0)
                {
                    while (avcodec_receive_frame(ctx_, frame_) >= 0)
                    {
                        convert(frame_);
                    }
                }
                av_packet_unref(packet_);
                return true;
            }
            eof_ = true;
            if (avcodec_send_packet(ctx_, nullptr) >= 0)
            {
                while (avcodec_receive_frame(ctx_, frame_) >= 0)
                {
                    convert(frame_);
                }
            }
            return true;
        }
        convert(nullptr); // the last milliseconds of the file
        return true;
    }

    Demuxer demux_;
    int stream_ = -1;
    AVCodecContext* ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::vector<float> fifo_;
    std::size_t head_ = 0;
    bool eof_ = false;
    bool drained_ = false;
};

bool readFile(std::string const& path, std::vector<std::uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool startsWithJpeg(UploadSource const& source)
{
    if (source.path.empty())
    {
        return source.data != nullptr && source.size >= 2 && source.data[0] == 0xff && source.data[1] == 0xd8;
    }
    std::ifstream in(source.path, std::ios::binary);
    unsigned char head[2] = {0, 0};
    in.read(reinterpret_cast<char*>(head), 2);
    return in.gcount() == 2 && head[0] == 0xff && head[1] == 0xd8;
}
} // namespace

UploadResult streamUpload(UploadSource const& source, VideoFormat const& house, UploadSink const& sink)
{
    UploadResult out;
    std::size_t const size = source.path.empty() ? source.size : static_cast<std::size_t>(std::filesystem::exists(source.path) ? std::filesystem::file_size(source.path) : 0);
    if ((source.path.empty() && source.data == nullptr) || size < 4)
    {
        out.error = "empty upload";
        return out;
    }
    if (startsWithJpeg(source))
    {
        std::vector<std::uint8_t> file;
        if (!source.path.empty() && !readFile(source.path, file))
        {
            out.error = "upload could not be read";
            return out;
        }
        auto const* data = source.path.empty() ? source.data : file.data();
        auto const bytes = source.path.empty() ? source.size : file.size();
        Frame10 decoded;
        if (!decodeJpeg422(data, bytes, decoded))
        {
            out.error = "jpeg decode failed";
            return out;
        }
        Frame10 scaled;
        scaled.allocate(house.width, house.height);
        bool const mismatch = decoded.width != house.width || decoded.height != house.height;
        scaleFrame(decoded, scaled, ScaleFilter::Bilinear);
        if (!sink.frame(0, 0, scaled, {}))
        {
            out.stopped = true;
            return out;
        }
        out.frames = 1;
        if (mismatch)
        {
            out.error = "scaled";
        }
        return out;
    }
    Demuxer demux;
    if (!demux.open(source))
    {
        out.error = "unrecognised media";
        return out;
    }
    AVFormatContext* fmt = demux.fmt;
    int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    // The audio is read by a second demuxer of its own (the file is read twice): it is taken per house frame.
    AudioReader audioReader;
    bool const hasAudio = audio >= 0 && audioReader.open(source, audio);
    if (video < 0)
    {
        out.audioOnly = audio >= 0;
        if (!out.audioOnly)
        {
            out.error = "no video frames";
            return out;
        }
        std::vector<float> chunk;
        while (hasAudio)
        {
            audioReader.take(48000, chunk);
            if (chunk.empty())
            {
                break;
            }
            if (!sink.audio(chunk.data(), chunk.size()))
            {
                out.stopped = true;
                return out;
            }
            out.audioSamples += chunk.size() / 2;
        }
        if (out.audioSamples == 0)
        {
            out.error = "no audio samples";
        }
        return out;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
    {
        fmt->streams[i]->discard = static_cast<int>(i) == video ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    AVStream* stream = fmt->streams[video];
    AVCodec const* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* vctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(vctx, stream->codecpar);
    bool const opened = codec != nullptr && avcodec_open2(vctx, codec, nullptr) >= 0;
    SwsContext* sws = nullptr;
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
        avcodec_free_context(&vctx);
        return out;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUV422P;
    yuv->width = house.width;
    yuv->height = house.height;
    av_frame_get_buffer(yuv, 32);
    // Frame-rate conversion to the house rate (1.8.1, FrameRateConverter). Streamed (1.8.2): the converter
    // decides a source frame's house frames when the next one arrives, so only the previous and the current
    // source frame are in memory, and each house frame goes to the sink with its audio at once.
    FrameRateConverter rate(house.rateNum, house.rateDen);
    AVRational const nanos{1, 1000000000};
    AVRational const timeBase = stream->time_base;
    AVRational const streamRate = stream->avg_frame_rate;
    std::int64_t const fallbackNs = streamRate.num > 0 && streamRate.den > 0 ? av_rescale(1000000000LL, streamRate.den, streamRate.num)
                                                                              : av_rescale(1000000000LL, house.rateDen, house.rateNum);
    std::int64_t firstPts = AV_NOPTS_VALUE;
    SourceClock clock{fallbackNs};
    Frame10 previous;
    Frame10 current;
    std::uint32_t sources = 0;
    std::vector<std::uint32_t> order;
    std::vector<float> frameAudio;
    bool stopped = false;
    // House frames the converter decided, all showing the previous source frame.
    auto emit = [&] {
        for (auto const source : order)
        {
            if (stopped)
            {
                break;
            }
            if (hasAudio)
            {
                audioReader.take(static_cast<std::size_t>(audioSamplesForFrame(out.frames, house.rateNum, house.rateDen, 48000)), frameAudio);
            }
            stopped = !sink.frame(out.frames, source, previous, frameAudio);
            out.frames += stopped ? 0 : 1;
        }
        order.clear();
    };
    auto take = [&](AVFrame* decoded) {
        sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height, yuv->data, yuv->linesize);
        auto const pts = decoded->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE && firstPts == AV_NOPTS_VALUE)
        {
            firstPts = pts;
        }
        auto const ns = clock.start(pts != AV_NOPTS_VALUE ? std::max<std::int64_t>(0, av_rescale_q(pts - firstPts, timeBase, nanos)) : -1,
            decoded->duration > 0 ? av_rescale_q(decoded->duration, timeBase, nanos) : 0);
        fromYuv(current, yuv->data[0], yuv->data[1], yuv->data[2], house.width, house.height, yuv->linesize[0], yuv->linesize[1], yuv->linesize[2]);
        rate.add(sources++, ns, order);
        emit();
        std::swap(previous, current);
    };
    while (!stopped && av_read_frame(fmt, packet) >= 0)
    {
        if (packet->stream_index == video && avcodec_send_packet(vctx, packet) >= 0)
        {
            while (!stopped && avcodec_receive_frame(vctx, frame) >= 0)
            {
                take(frame);
            }
        }
        av_packet_unref(packet);
    }
    // Drain the video decoder (frames it still holds), then the last source frame for its own duration.
    if (!stopped && avcodec_send_packet(vctx, nullptr) >= 0)
    {
        while (!stopped && avcodec_receive_frame(vctx, frame) >= 0)
        {
            take(frame);
        }
    }
    if (!stopped && sources == 1)
    {
        order.assign(1, 0); // a still image: one frame, as before
        emit();
    }
    else if (!stopped)
    {
        rate.finish(clock.endNs(), order);
        emit();
    }
    av_frame_free(&yuv);
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws);
    avcodec_free_context(&vctx);
    out.stopped = stopped;
    if (out.error.empty() && !stopped && out.frames == 0)
    {
        out.error = "no video frames";
    }
    return out;
}

ConvertedMedia convertUpload(std::uint8_t const* data, std::size_t size, VideoFormat const& house)
{
    // Everything in memory (tests and small files); the engine streams uploads with streamUpload.
    ConvertedMedia out;
    std::uint32_t last = 0;
    UploadSink sink;
    sink.frame = [&](std::uint64_t, std::uint32_t source, Frame10 const& picture, std::vector<float> const& audio) {
        if (out.frames.empty() || source != last)
        {
            out.frames.push_back(picture);
            last = source;
        }
        out.order.push_back(static_cast<std::uint32_t>(out.frames.size() - 1));
        out.audio.insert(out.audio.end(), audio.begin(), audio.end());
        return true;
    };
    sink.audio = [&](float const* samples, std::size_t count) {
        out.audio.insert(out.audio.end(), samples, samples + count);
        return true;
    };
    auto const result = streamUpload(UploadSource{{}, data, size}, house, sink);
    out.audioOnly = result.audioOnly;
    out.error = result.error;
    return out;
}
} // namespace replay
