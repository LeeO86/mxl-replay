#include "media/v210.hpp"

#include <algorithm>
#include <cstring>

namespace replay
{
void Frame10::allocate(int w, int h)
{
    width = w;
    height = h;
    y.assign(static_cast<std::size_t>(w * h), 0);
    cb.assign(static_cast<std::size_t>((w / 2) * h), 512);
    cr.assign(static_cast<std::size_t>((w / 2) * h), 512);
}

void Frame10::fill(std::uint16_t yValue, std::uint16_t cbValue, std::uint16_t crValue)
{
    std::fill(y.begin(), y.end(), yValue);
    std::fill(cb.begin(), cb.end(), cbValue);
    std::fill(cr.begin(), cr.end(), crValue);
}

std::size_t v210RowBytes(int width)
{
    int const groups = (width + 5) / 6;
    return static_cast<std::size_t>(groups) * 16u;
}

std::size_t v210Size(int width, int height)
{
    return v210RowBytes(width) * static_cast<std::size_t>(height);
}

namespace
{
std::uint16_t sample10(std::uint32_t word, int shift)
{
    return static_cast<std::uint16_t>((word >> shift) & 0x3ffu);
}
} // namespace

void unpackV210(std::uint8_t const* src, int srcRowBytes, Frame10& dst)
{
    if (srcRowBytes <= 0)
    {
        srcRowBytes = static_cast<int>(v210RowBytes(dst.width));
    }
    int const cw = dst.width / 2;
    for (int row = 0; row < dst.height; ++row)
    {
        auto const* line = src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcRowBytes);
        int x = 0;
        int const groups = (dst.width + 5) / 6;
        for (int group = 0; group < groups; ++group)
        {
            std::uint32_t words[4] = {};
            std::memcpy(words, line + static_cast<std::size_t>(group) * 16u, sizeof(words));
            std::uint16_t yv[6] = {sample10(words[0], 10), sample10(words[1], 0), sample10(words[1], 20), sample10(words[2], 10), sample10(words[3], 0),
                sample10(words[3], 20)};
            std::uint16_t cbv[3] = {sample10(words[0], 0), sample10(words[1], 10), sample10(words[2], 20)};
            std::uint16_t crv[3] = {sample10(words[0], 20), sample10(words[2], 0), sample10(words[3], 10)};
            for (int i = 0; i < 6 && x < dst.width; ++i, ++x)
            {
                dst.y[static_cast<std::size_t>(row * dst.width + x)] = yv[i];
                if ((x & 1) == 0 && (x / 2) < cw)
                {
                    dst.cb[static_cast<std::size_t>(row * cw + x / 2)] = cbv[i / 2];
                    dst.cr[static_cast<std::size_t>(row * cw + x / 2)] = crv[i / 2];
                }
            }
        }
    }
}

void packV210(Frame10 const& src, std::uint8_t* dst, int dstRowBytes)
{
    if (dstRowBytes <= 0)
    {
        dstRowBytes = static_cast<int>(v210RowBytes(src.width));
    }
    int const cw = src.width / 2;
    for (int row = 0; row < src.height; ++row)
    {
        auto* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstRowBytes);
        std::memset(line, 0, static_cast<std::size_t>(dstRowBytes));
        int const groups = (src.width + 5) / 6;
        for (int group = 0; group < groups; ++group)
        {
            std::uint16_t yv[6] = {};
            std::uint16_t cbv[3] = {};
            std::uint16_t crv[3] = {};
            for (int i = 0; i < 6; ++i)
            {
                int const x = group * 6 + i;
                if (x < src.width)
                {
                    yv[i] = src.y[static_cast<std::size_t>(row * src.width + x)];
                    if ((i % 2) == 0)
                    {
                        int const cx = x / 2;
                        if (cx < cw)
                        {
                            cbv[i / 2] = src.cb[static_cast<std::size_t>(row * cw + cx)];
                            crv[i / 2] = src.cr[static_cast<std::size_t>(row * cw + cx)];
                        }
                    }
                }
            }
            std::uint32_t words[4];
            words[0] = (cbv[0] & 0x3ffu) | (static_cast<std::uint32_t>(yv[0] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(crv[0] & 0x3ffu) << 20);
            words[1] = (yv[1] & 0x3ffu) | (static_cast<std::uint32_t>(cbv[1] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(yv[2] & 0x3ffu) << 20);
            words[2] = (crv[1] & 0x3ffu) | (static_cast<std::uint32_t>(yv[3] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(cbv[2] & 0x3ffu) << 20);
            words[3] = (yv[4] & 0x3ffu) | (static_cast<std::uint32_t>(crv[2] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(yv[5] & 0x3ffu) << 20);
            std::memcpy(line + static_cast<std::size_t>(group) * 16u, words, sizeof(words));
        }
    }
}
} // namespace replay
