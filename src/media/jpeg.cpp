#include "media/jpeg.hpp"

#include <jpeglib.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace replay
{
namespace
{
constexpr int kMcu = 8;

std::uint8_t to8(std::uint16_t sample)
{
    return static_cast<std::uint8_t>(sample >> 2);
}

std::uint16_t to10(std::uint8_t sample)
{
    return static_cast<std::uint16_t>(sample) << 2;
}

std::uint8_t sample8(std::uint32_t word, int shift)
{
    return static_cast<std::uint8_t>(((word >> shift) & 0x3ffu) >> 2);
}

// One v210 line to 8-bit planes, as unpackV210 then to8.
void v210LineTo8(std::uint8_t const* line, int width, std::uint8_t* y, std::uint8_t* cb, std::uint8_t* cr)
{
    int const cw = width / 2;
    int const groups = (width + 5) / 6;
    for (int group = 0; group < groups; ++group)
    {
        std::uint32_t w[4];
        std::memcpy(w, line + static_cast<std::size_t>(group) * 16u, sizeof(w));
        int const x = group * 6;
        if (x + 6 <= width)
        {
            y[x] = sample8(w[0], 10);
            y[x + 1] = sample8(w[1], 0);
            y[x + 2] = sample8(w[1], 20);
            y[x + 3] = sample8(w[2], 10);
            y[x + 4] = sample8(w[3], 0);
            y[x + 5] = sample8(w[3], 20);
            int const c = x / 2;
            cb[c] = sample8(w[0], 0);
            cr[c] = sample8(w[0], 20);
            cb[c + 1] = sample8(w[1], 10);
            cr[c + 1] = sample8(w[2], 0);
            cb[c + 2] = sample8(w[2], 20);
            cr[c + 2] = sample8(w[3], 10);
            continue;
        }
        std::uint8_t const yv[6] = {sample8(w[0], 10), sample8(w[1], 0), sample8(w[1], 20), sample8(w[2], 10), sample8(w[3], 0), sample8(w[3], 20)};
        std::uint8_t const cbv[3] = {sample8(w[0], 0), sample8(w[1], 10), sample8(w[2], 20)};
        std::uint8_t const crv[3] = {sample8(w[0], 20), sample8(w[2], 0), sample8(w[3], 10)};
        for (int i = 0; i < 6 && x + i < width; ++i)
        {
            y[x + i] = yv[i];
            if (((x + i) & 1) == 0 && (x + i) / 2 < cw)
            {
                cb[(x + i) / 2] = cbv[i / 2];
                cr[(x + i) / 2] = crv[i / 2];
            }
        }
    }
}

// 8-bit planes to one v210 line, as to10 then packV210 (zero padding).
void line8ToV210(std::uint8_t const* y, std::uint8_t const* cb, std::uint8_t const* cr, int width, int rowBytes, std::uint8_t* line)
{
    std::memset(line, 0, static_cast<std::size_t>(rowBytes));
    int const cw = width / 2;
    int const groups = (width + 5) / 6;
    auto const s = [](std::uint8_t v) { return static_cast<std::uint32_t>(v) << 2; };
    for (int group = 0; group < groups; ++group)
    {
        int const x = group * 6;
        std::uint32_t yv[6] = {};
        std::uint32_t cbv[3] = {};
        std::uint32_t crv[3] = {};
        for (int i = 0; i < 6 && x + i < width; ++i)
        {
            yv[i] = s(y[x + i]);
            if ((i % 2) == 0 && (x + i) / 2 < cw)
            {
                cbv[i / 2] = s(cb[(x + i) / 2]);
                crv[i / 2] = s(cr[(x + i) / 2]);
            }
        }
        std::uint32_t const w[4] = {cbv[0] | (yv[0] << 10) | (crv[0] << 20), yv[1] | (cbv[1] << 10) | (yv[2] << 20), crv[1] | (yv[3] << 10) | (cbv[2] << 20),
            yv[4] | (crv[2] << 10) | (yv[5] << 20)};
        std::memcpy(line + static_cast<std::size_t>(group) * 16u, w, sizeof(w));
    }
}

// libjpeg destination that writes into a std::vector, so the JPEG is not copied afterwards.
struct VectorDestination
{
    jpeg_destination_mgr pub{};
    std::vector<std::uint8_t>* out = nullptr;

    static void init(j_compress_ptr cinfo)
    {
        auto* self = reinterpret_cast<VectorDestination*>(cinfo->dest);
        self->pub.next_output_byte = self->out->data();
        self->pub.free_in_buffer = self->out->size();
    }
    static boolean empty(j_compress_ptr cinfo)
    {
        auto* self = reinterpret_cast<VectorDestination*>(cinfo->dest);
        auto const used = self->out->size();
        self->out->resize(used * 2);
        self->pub.next_output_byte = self->out->data() + used;
        self->pub.free_in_buffer = self->out->size() - used;
        return TRUE;
    }
    static void term(j_compress_ptr cinfo)
    {
        auto* self = reinterpret_cast<VectorDestination*>(cinfo->dest);
        self->out->resize(self->out->size() - self->pub.free_in_buffer);
    }
};

// One compressor, one decompressor and their row buffers per thread.
struct ThreadCodec
{
    jpeg_compress_struct encoder{};
    jpeg_decompress_struct decoder{};
    jpeg_error_mgr encoderErrors{};
    jpeg_error_mgr decoderErrors{};
    VectorDestination destination;
    std::vector<std::uint8_t> y, cb, cr;
    std::size_t lastSize = 256 * 1024;

    ThreadCodec()
    {
        encoder.err = jpeg_std_error(&encoderErrors);
        jpeg_create_compress(&encoder);
        decoder.err = jpeg_std_error(&decoderErrors);
        jpeg_create_decompress(&decoder);
        destination.pub.init_destination = &VectorDestination::init;
        destination.pub.empty_output_buffer = &VectorDestination::empty;
        destination.pub.term_destination = &VectorDestination::term;
    }
    ~ThreadCodec()
    {
        jpeg_destroy_compress(&encoder);
        jpeg_destroy_decompress(&decoder);
    }
    ThreadCodec(ThreadCodec const&) = delete;
    ThreadCodec& operator=(ThreadCodec const&) = delete;

    void rows(int width, int count)
    {
        y.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(count));
        cb.resize(static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(count));
        cr.resize(cb.size());
    }
};

ThreadCodec& threadCodec()
{
    thread_local ThreadCodec codec;
    return codec;
}
} // namespace

int jpegStorageBitDepth()
{
    return 8;
}

bool jpegSupports10Bit()
{
    return false;
}

bool jpegSupports12Bit()
{
    return false;
}

double bytesPerFrameEstimate(int width, int height, int quality)
{
    double const q = quality <= 0 ? 1.0 : static_cast<double>(quality) / 92.0;
    return static_cast<double>(width) * static_cast<double>(height) * 0.1875 * q;
}

double bytesPerHourEstimate(int width, int height, int fpsNum, int fpsDen, int quality)
{
    if (fpsDen <= 0)
    {
        return 0;
    }
    double const fps = static_cast<double>(fpsNum) / static_cast<double>(fpsDen);
    return bytesPerFrameEstimate(width, height, quality) * fps * 3600.0;
}

std::vector<std::uint8_t> encodeJpeg422(Frame10 const& frame, int quality)
{
    if (frame.width < 2 || frame.height < 1 || (frame.width % 2) != 0)
    {
        throw std::runtime_error("jpeg encode needs an even width");
    }
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char* mem = nullptr;
    unsigned long memSize = 0;
    jpeg_mem_dest(&cinfo, &mem, &memSize);
    cinfo.image_width = static_cast<JDIMENSION>(frame.width);
    cinfo.image_height = static_cast<JDIMENSION>(frame.height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, JCS_YCbCr);
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 1;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
    cinfo.raw_data_in = TRUE;
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    int const cw = frame.width / 2;
    std::vector<std::uint8_t> yPlane(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(kMcu));
    std::vector<std::uint8_t> cbPlane(static_cast<std::size_t>(cw) * static_cast<std::size_t>(kMcu));
    std::vector<std::uint8_t> crPlane(cbPlane.size());
    JSAMPROW yRows[kMcu];
    JSAMPROW cbRows[kMcu];
    JSAMPROW crRows[kMcu];
    for (int i = 0; i < kMcu; ++i)
    {
        yRows[i] = yPlane.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(frame.width);
        cbRows[i] = cbPlane.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
        crRows[i] = crPlane.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
    }
    JSAMPARRAY planes[3] = {yRows, cbRows, crRows};
    while (cinfo.next_scanline < cinfo.image_height)
    {
        for (int i = 0; i < kMcu; ++i)
        {
            int const row = static_cast<int>(std::min<JDIMENSION>(cinfo.next_scanline + static_cast<JDIMENSION>(i), cinfo.image_height - 1));
            for (int x = 0; x < frame.width; ++x)
            {
                yRows[i][x] = to8(frame.y[static_cast<std::size_t>(row * frame.width + x)]);
            }
            for (int x = 0; x < cw; ++x)
            {
                cbRows[i][x] = to8(frame.cb[static_cast<std::size_t>(row * cw + x)]);
                crRows[i][x] = to8(frame.cr[static_cast<std::size_t>(row * cw + x)]);
            }
        }
        jpeg_write_raw_data(&cinfo, planes, kMcu);
    }
    jpeg_finish_compress(&cinfo);
    std::vector<std::uint8_t> out(mem, mem + memSize);
    jpeg_destroy_compress(&cinfo);
    free(mem);
    return out;
}

bool decodeJpeg422(std::uint8_t const* data, std::size_t size, Frame10& frame)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
    {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.raw_data_out = TRUE;
    cinfo.out_color_space = JCS_YCbCr;
    jpeg_start_decompress(&cinfo);
    int const width = static_cast<int>(cinfo.output_width);
    int const height = static_cast<int>(cinfo.output_height);
    if (width < 2 || (width % 2) != 0 || cinfo.num_components < 3)
    {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    frame.allocate(width, height);
    int const cw = width / 2;
    int const mcu = static_cast<int>(cinfo.max_v_samp_factor) * DCTSIZE;
    std::vector<std::uint8_t> yPlane(static_cast<std::size_t>(width) * static_cast<std::size_t>(mcu));
    std::vector<std::uint8_t> cbPlane(static_cast<std::size_t>(cw) * static_cast<std::size_t>(mcu));
    std::vector<std::uint8_t> crPlane(cbPlane.size());
    std::vector<JSAMPROW> yRows(static_cast<std::size_t>(mcu));
    std::vector<JSAMPROW> cbRows(static_cast<std::size_t>(mcu));
    std::vector<JSAMPROW> crRows(static_cast<std::size_t>(mcu));
    for (int i = 0; i < mcu; ++i)
    {
        yRows[static_cast<std::size_t>(i)] = yPlane.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(width);
        cbRows[static_cast<std::size_t>(i)] = cbPlane.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
        crRows[static_cast<std::size_t>(i)] = crPlane.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
    }
    JSAMPARRAY planes[3] = {yRows.data(), cbRows.data(), crRows.data()};
    while (cinfo.output_scanline < cinfo.output_height)
    {
        JDIMENSION const scan = cinfo.output_scanline;
        jpeg_read_raw_data(&cinfo, planes, static_cast<JDIMENSION>(mcu));
        int const rows = std::min(mcu, height - static_cast<int>(scan));
        for (int i = 0; i < rows; ++i)
        {
            int const row = static_cast<int>(scan) + i;
            for (int x = 0; x < width; ++x)
            {
                frame.y[static_cast<std::size_t>(row * width + x)] = to10(yRows[static_cast<std::size_t>(i)][x]);
            }
            for (int x = 0; x < cw; ++x)
            {
                frame.cb[static_cast<std::size_t>(row * cw + x)] = to10(cbRows[static_cast<std::size_t>(i)][x]);
                frame.cr[static_cast<std::size_t>(row * cw + x)] = to10(crRows[static_cast<std::size_t>(i)][x]);
            }
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool decodeJpegPreview(std::uint8_t const* data, std::size_t size, int minWidth, Frame10& frame)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
    {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    unsigned denom = 8;
    while (denom > 1 && cinfo.image_width / denom < static_cast<unsigned>(std::max(1, minWidth)))
    {
        denom /= 2;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_YCbCr;
    jpeg_start_decompress(&cinfo);
    int const width = static_cast<int>(cinfo.output_width) & ~1;
    int const height = static_cast<int>(cinfo.output_height);
    if (width < 2 || height < 1 || cinfo.output_components != 3)
    {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    frame.allocate(width, height);
    int const cw = width / 2;
    std::vector<JSAMPLE> line(static_cast<std::size_t>(cinfo.output_width) * 3u);
    while (cinfo.output_scanline < cinfo.output_height)
    {
        auto const row = static_cast<std::size_t>(cinfo.output_scanline);
        JSAMPROW rows[1] = {line.data()};
        jpeg_read_scanlines(&cinfo, rows, 1);
        for (int x = 0; x < width; ++x)
        {
            frame.y[row * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = to10(line[static_cast<std::size_t>(x) * 3u]);
        }
        // 4:4:4 output: one chroma pair per two pixels, averaged.
        for (int x = 0; x < cw; ++x)
        {
            auto const a = static_cast<std::size_t>(2 * x) * 3u;
            auto const b = a + 3u;
            frame.cb[row * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x)] = static_cast<std::uint16_t>((to10(line[a + 1]) + to10(line[b + 1])) / 2);
            frame.cr[row * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x)] = static_cast<std::uint16_t>((to10(line[a + 2]) + to10(line[b + 2])) / 2);
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

std::vector<std::uint8_t> encodeJpegV210(std::uint8_t const* v210, int width, int height, int rowBytes, int quality)
{
    if (width < 2 || height < 1 || (width % 2) != 0)
    {
        throw std::runtime_error("jpeg encode needs an even width");
    }
    if (rowBytes <= 0)
    {
        rowBytes = static_cast<int>(((width + 5) / 6) * 16);
    }
    auto& codec = threadCodec();
    auto& cinfo = codec.encoder;
    std::vector<std::uint8_t> out(codec.lastSize + codec.lastSize / 4);
    codec.destination.out = &out;
    cinfo.dest = &codec.destination.pub;
    cinfo.image_width = static_cast<JDIMENSION>(width);
    cinfo.image_height = static_cast<JDIMENSION>(height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, JCS_YCbCr);
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 1;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
    cinfo.raw_data_in = TRUE;
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    int const cw = width / 2;
    codec.rows(width, kMcu);
    JSAMPROW yRows[kMcu];
    JSAMPROW cbRows[kMcu];
    JSAMPROW crRows[kMcu];
    for (int i = 0; i < kMcu; ++i)
    {
        yRows[i] = codec.y.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(width);
        cbRows[i] = codec.cb.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
        crRows[i] = codec.cr.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
    }
    JSAMPARRAY planes[3] = {yRows, cbRows, crRows};
    while (cinfo.next_scanline < cinfo.image_height)
    {
        for (int i = 0; i < kMcu; ++i)
        {
            // Rows past the bottom repeat the last line, as encodeJpeg422 does.
            int const row = static_cast<int>(std::min<JDIMENSION>(cinfo.next_scanline + static_cast<JDIMENSION>(i), cinfo.image_height - 1));
            v210LineTo8(v210 + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes), width, yRows[i], cbRows[i], crRows[i]);
        }
        jpeg_write_raw_data(&cinfo, planes, kMcu);
    }
    jpeg_finish_compress(&cinfo);
    codec.lastSize = out.size();
    return out;
}

bool decodeJpegToV210(std::uint8_t const* data, std::size_t size, int width, int height, int rowBytes, std::uint8_t* v210)
{
    if (data == nullptr || size == 0 || v210 == nullptr)
    {
        return false;
    }
    if (rowBytes <= 0)
    {
        rowBytes = static_cast<int>(((width + 5) / 6) * 16);
    }
    auto& codec = threadCodec();
    auto& cinfo = codec.decoder;
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
    {
        jpeg_abort_decompress(&cinfo);
        return false;
    }
    if (static_cast<int>(cinfo.image_width) != width || static_cast<int>(cinfo.image_height) != height || cinfo.num_components < 3 ||
        cinfo.max_v_samp_factor != 1 || cinfo.comp_info[0].h_samp_factor != 2)
    {
        jpeg_abort_decompress(&cinfo);
        return false;
    }
    cinfo.raw_data_out = TRUE;
    cinfo.out_color_space = JCS_YCbCr;
    jpeg_start_decompress(&cinfo);
    int const cw = width / 2;
    int const mcu = static_cast<int>(cinfo.max_v_samp_factor) * DCTSIZE;
    codec.rows(width, mcu);
    JSAMPROW yRows[DCTSIZE];
    JSAMPROW cbRows[DCTSIZE];
    JSAMPROW crRows[DCTSIZE];
    for (int i = 0; i < mcu; ++i)
    {
        yRows[i] = codec.y.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(width);
        cbRows[i] = codec.cb.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
        crRows[i] = codec.cr.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(cw);
    }
    JSAMPARRAY planes[3] = {yRows, cbRows, crRows};
    while (cinfo.output_scanline < cinfo.output_height)
    {
        JDIMENSION const scan = cinfo.output_scanline;
        jpeg_read_raw_data(&cinfo, planes, static_cast<JDIMENSION>(mcu));
        int const rows = std::min(mcu, height - static_cast<int>(scan));
        for (int i = 0; i < rows; ++i)
        {
            line8ToV210(yRows[i], cbRows[i], crRows[i], width, rowBytes, v210 + (static_cast<std::size_t>(scan) + static_cast<std::size_t>(i)) * static_cast<std::size_t>(rowBytes));
        }
    }
    jpeg_finish_decompress(&cinfo);
    return true;
}
} // namespace replay
