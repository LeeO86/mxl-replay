#include "flow/dis.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
replay::Yuv422 loadPpm(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int width = 0;
    int height = 0;
    int maxValue = 0;
    in >> magic >> width >> height >> maxValue;
    in.get();
    replay::Yuv422 frame;
    frame.width = width;
    frame.height = height;
    frame.y.resize(static_cast<std::size_t>(width * height));
    frame.cb.assign(static_cast<std::size_t>((width / 2) * height), 0.5f);
    frame.cr.assign(frame.cb.size(), 0.5f);
    for (int i = 0; i < width * height; ++i)
    {
        unsigned char rgb[3] = {};
        in.read(reinterpret_cast<char*>(rgb), 3);
        frame.y[static_cast<std::size_t>(i)] = (0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]) / 255.f;
    }
    return frame;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: mxl-replay-harness <a.ppm> <b.ppm> <phase> [out.ppm]\n"
                  << "       mxl-replay-harness --compare <a.ppm> <b.ppm>\n";
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "--compare")
    {
        auto const a = loadPpm(argv[2]);
        auto const b = loadPpm(argv[3]);
        std::cout << "psnr " << replay::psnrLuma(a, b) << "\nssim " << replay::ssimLuma(a, b) << "\n";
        return replay::psnrLuma(a, b) >= 40.0 ? 0 : 1;
    }
    auto const a = loadPpm(argv[1]);
    auto const b = loadPpm(argv[2]);
    float const phase = std::stof(argv[3]);
    auto const op = replay::operatingPoint(replay::InterpPreset::Balanced);
    auto const pair = replay::computeFlowPair(replay::grayFromLuma(a), replay::grayFromLuma(b), op);
    auto const out = replay::interpolateFrames(a, b, pair.forward, pair.backward, phase, op);
    std::cout << "flow " << pair.forward.width << "x" << pair.forward.height << " phase " << phase << "\n";
    if (argc >= 5)
    {
        std::ofstream ppm(argv[4], std::ios::binary);
        ppm << "P6\n" << out.width << " " << out.height << "\n255\n";
        for (float y : out.y)
        {
            auto const v = static_cast<unsigned char>(std::max(0.f, std::min(1.f, y)) * 255.f);
            ppm.put(static_cast<char>(v));
            ppm.put(static_cast<char>(v));
            ppm.put(static_cast<char>(v));
        }
    }
    return 0;
}
