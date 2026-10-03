#pragma once

#include <cstdint>
#include <vector>

namespace replay
{
// 10-bit 4:2:2 picture. Chroma is half the luma width.
struct Frame10
{
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> y;
    std::vector<std::uint16_t> cb;
    std::vector<std::uint16_t> cr;

    void allocate(int w, int h);
    void fill(std::uint16_t yValue, std::uint16_t cbValue, std::uint16_t crValue);
    [[nodiscard]] bool empty() const { return width <= 0 || y.empty(); }
};

struct PcmBuffer
{
    int channels = 2;
    std::vector<float> samples;
};
} // namespace replay
