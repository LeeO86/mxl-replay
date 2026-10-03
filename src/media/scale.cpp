#include "media/scale.hpp"

#include <algorithm>
#include <cmath>

namespace replay
{
namespace
{
float cubic(float x)
{
    x = std::fabs(x);
    if (x <= 1.f)
    {
        return 1.5f * x * x * x - 2.5f * x * x + 1.f;
    }
    if (x < 2.f)
    {
        return -0.5f * x * x * x + 2.5f * x * x - 4.f * x + 2.f;
    }
    return 0.f;
}

std::uint16_t samplePlane(std::vector<std::uint16_t> const& plane, int width, int height, float x, float y, ScaleFilter filter)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    x = std::clamp(x, 0.f, static_cast<float>(width - 1));
    y = std::clamp(y, 0.f, static_cast<float>(height - 1));
    auto at = [&](int px, int py) {
        px = std::clamp(px, 0, width - 1);
        py = std::clamp(py, 0, height - 1);
        return static_cast<float>(plane[static_cast<std::size_t>(py * width + px)]);
    };
    if (filter == ScaleFilter::Bilinear)
    {
        int const x0 = static_cast<int>(std::floor(x));
        int const y0 = static_cast<int>(std::floor(y));
        int const x1 = std::min(x0 + 1, width - 1);
        int const y1 = std::min(y0 + 1, height - 1);
        float const fx = x - static_cast<float>(x0);
        float const fy = y - static_cast<float>(y0);
        float const v = (at(x0, y0) * (1.f - fx) + at(x1, y0) * fx) * (1.f - fy) + (at(x0, y1) * (1.f - fx) + at(x1, y1) * fx) * fy;
        return static_cast<std::uint16_t>(std::clamp(v, 0.f, 1023.f));
    }
    int const x0 = static_cast<int>(std::floor(x));
    int const y0 = static_cast<int>(std::floor(y));
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    float sum = 0.f;
    float wsum = 0.f;
    for (int ky = -1; ky <= 2; ++ky)
    {
        for (int kx = -1; kx <= 2; ++kx)
        {
            float const w = cubic(static_cast<float>(kx) - fx) * cubic(static_cast<float>(ky) - fy);
            sum += at(x0 + kx, y0 + ky) * w;
            wsum += w;
        }
    }
    float const v = wsum == 0.f ? 0.f : sum / wsum;
    return static_cast<std::uint16_t>(std::clamp(v, 0.f, 1023.f));
}
} // namespace

void scaleFrame(Frame10 const& src, Frame10& dst, ScaleFilter filter)
{
    if (dst.width <= 0 || dst.height <= 0)
    {
        return;
    }
    if (dst.y.size() != static_cast<std::size_t>(dst.width * dst.height))
    {
        dst.allocate(dst.width, dst.height);
    }
    if (src.width == dst.width && src.height == dst.height)
    {
        dst = src;
        return;
    }
    int const scw = std::max(1, src.width / 2);
    int const dcw = std::max(1, dst.width / 2);
    for (int y = 0; y < dst.height; ++y)
    {
        float const sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(src.height) / static_cast<float>(dst.height) - 0.5f;
        for (int x = 0; x < dst.width; ++x)
        {
            float const sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(src.width) / static_cast<float>(dst.width) - 0.5f;
            dst.y[static_cast<std::size_t>(y * dst.width + x)] = samplePlane(src.y, src.width, src.height, sx, sy, filter);
        }
        for (int x = 0; x < dcw; ++x)
        {
            float const sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(scw) / static_cast<float>(dcw) - 0.5f;
            dst.cb[static_cast<std::size_t>(y * dcw + x)] = samplePlane(src.cb, scw, src.height, sx, sy, filter);
            dst.cr[static_cast<std::size_t>(y * dcw + x)] = samplePlane(src.cr, scw, src.height, sx, sy, filter);
        }
    }
}
} // namespace replay
