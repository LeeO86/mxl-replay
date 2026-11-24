// Times a blended grain of a CPU slow motion, stage by stage, before and after the v210 blend.
//
//   mxl-replay-blend-bench <pictures.v210> <width> <height> [label]
//
// JPEG q92 on real pictures, one thread.
//
// In 1.8.7 the output thread did everything per blended grain. It decoded A and B to 16-bit Frame10s
// (allocation included), blended them (blendInto), packed the result (packV210) and copied it to the
// channel's last grain.
// Now the decode worker decodes one new picture per blended grain straight to v210 (decodeJpegToV210).
// The output thread copies it out of the worker's slot, blends A (kept from the grain before) and B
// straight into the grain in v210 (blendV210), and keeps the grain.
//
// The bench leaves out the stored picture's read from the ring (a pread of 0.5–1.5 MB from the page
// cache), which is the same in both.

#include "media/jpeg.hpp"
#include "media/v210.hpp"
#include "playout/transition.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace replay;

namespace
{
using Clock = std::chrono::steady_clock;

// a = a·(1−w) + b·w as render's blendInto.
void blendInto(Frame10& a, Frame10 const& b, std::uint32_t w)
{
    auto blend = [w](std::vector<std::uint16_t>& left, std::vector<std::uint16_t> const& right) {
        std::uint16_t* l = left.data();
        std::uint16_t const* r = right.data();
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            l[i] = static_cast<std::uint16_t>((l[i] * (1024u - w) + r[i] * w) >> 10);
        }
    };
    blend(a.y, b.y);
    blend(a.cb, b.cb);
    blend(a.cr, b.cr);
}

struct Stage
{
    std::string name;
    double total = 0;
    int count = 0;
};

class Stages
{
public:
    double time(std::string const& name, std::function<void()> const& work)
    {
        auto const t0 = Clock::now();
        work();
        double const ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        for (auto& stage : stages_)
        {
            if (stage.name == name)
            {
                stage.total += ms;
                ++stage.count;
                return ms;
            }
        }
        stages_.push_back({name, ms, 1});
        return ms;
    }
    void clear()
    {
        stages_.clear();
    }
    std::vector<Stage> const& stages() const
    {
        return stages_;
    }

private:
    std::vector<Stage> stages_;
};
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <pictures.v210> <width> <height> [label]\n", argv[0]);
        return 2;
    }
    int const width = std::atoi(argv[2]);
    int const height = std::atoi(argv[3]);
    std::string const label = argc > 4 ? argv[4] : argv[1];
    auto const bytes = v210Size(width, height);
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t const count = std::min<std::size_t>(file.size() / bytes, 60);
    if (count < 3)
    {
        std::fprintf(stderr, "need at least 3 pictures of %dx%d v210 in %s\n", width, height, argv[1]);
        return 2;
    }

    int const quality = 92;

    std::printf("%s: %zu pictures %dx%d; ms per blended grain (0.5x: frames k and k+1 at 50 %%), one thread\n", label.c_str(), count, width, height);
    std::uint32_t const w = blendWeight1024(0.5);
    {
        std::vector<std::vector<std::uint8_t>> stored(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            stored[i] = encodeJpegV210(file.data() + i * bytes, width, height, 0, quality);
        }
        Stages before;
        Stages after;
        std::vector<std::uint8_t> out(bytes);
        std::vector<std::uint8_t> last(bytes);
        // The channel's state after: the picture A the grain before gave (decoded), B (decodedB) and the
        // worker's slot for the new B.
        std::vector<std::uint8_t> cacheA(bytes);
        std::vector<std::uint8_t> cacheB(bytes);
        std::vector<std::uint8_t> slot(bytes);
        for (int pass = 0; pass < 2; ++pass)
        {
            before.clear();
            after.clear();
            for (std::size_t k = 0; k + 1 < count; ++k)
            {
                auto const& a = stored[k];
                auto const& b = stored[k + 1];
                // Before: everything on the output thread.
                Frame10 fa;
                Frame10 fb;
                before.time("decode A to 16-bit frame", [&] { decodeJpeg422(a.data(), a.size(), fa); });
                before.time("decode B to 16-bit frame", [&] { decodeJpeg422(b.data(), b.size(), fb); });
                before.time("blend 16-bit (blendInto)", [&] { blendInto(fa, fb, w); });
                before.time("pack to v210 (packV210)", [&] { packV210(fa, out.data(), 0); });
                before.time("copy to the channel's last grain", [&] { std::memcpy(last.data(), out.data(), bytes); });

                // After: the worker decodes the new B to v210; the output thread copies it out of the slot, blends
                // A (kept from the grain before) and B straight into the grain, and keeps the grain.
                after.time("worker: decode B to v210", [&] { decodeJpegToV210(b.data(), b.size(), width, height, 0, slot.data()); });
                after.time("B from the worker's slot (copy)", [&] { std::memcpy(cacheB.data(), slot.data(), bytes); });
                after.time("blend A and B into the grain (blendV210)", [&] { blendV210(cacheA.data(), cacheB.data(), out.data(), bytes, w); });
                after.time("copy to the channel's last grain", [&] { std::memcpy(last.data(), out.data(), bytes); });
                std::swap(cacheA, cacheB);
            }
        }
        auto print = [](char const* title, Stages const& stages, bool workerApart) {
            double total = 0;
            double output = 0;
            std::printf("\n| %s | ms |\n| --- | --- |\n", title);
            for (auto const& stage : stages.stages())
            {
                double const ms = stage.total / stage.count;
                std::printf("| %s | %.2f |\n", stage.name.c_str(), ms);
                total += ms;
                if (!workerApart || stage.name.rfind("worker", 0) != 0)
                {
                    output += ms;
                }
            }
            if (workerApart)
            {
                std::printf("| **output thread** | **%.2f** |\n| **CPU in all** | **%.2f** |\n", output, total);
            }
            else
            {
                std::printf("| **output thread = CPU in all** | **%.2f** |\n", total);
            }
        };
        std::printf("\n### JPEG q%d\n", quality);
        print("Before (1.8.7)", before, false);
        print("After (v210 blend)", after, true);
    }
    return 0;
}
