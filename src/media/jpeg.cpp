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
} // namespace replay
