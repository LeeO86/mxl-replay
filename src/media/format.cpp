#include "media/format.hpp"

#include "media/timebase.hpp"

#include <cctype>
#include <stdexcept>

namespace replay
{
namespace
{
std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}
} // namespace

std::string VideoFormat::token() const
{
    return std::to_string(width) + "x" + std::to_string(height) + (interlaced ? "i" : "p") + std::to_string(rateNum) +
           (rateDen == 1 ? "" : "/" + std::to_string(rateDen));
}

VideoFormat parseVideoFormat(std::string const& token)
{
    auto text = lower(token);
    VideoFormat fmt;
    auto const p = text.find_first_of("pi");
    if (p == std::string::npos)
    {
        throw std::runtime_error("video format must look like 1080p50 or 1080i25");
    }
    fmt.interlaced = text[p] == 'i';
    auto const raster = text.substr(0, p);
    auto rate = text.substr(p + 1);
    if (fmt.interlaced)
    {
        if (rate.ends_with("bff"))
        {
            throw std::runtime_error("bottom field first is not supported (" + token + "); use a top field first format such as 1080i25");
        }
        if (rate.ends_with("tff"))
        {
            rate.resize(rate.size() - 3);
        }
    }
    if (raster == "720")
    {
        fmt.width = 1280;
        fmt.height = 720;
    }
    else if (raster == "1080")
    {
        fmt.width = 1920;
        fmt.height = 1080;
    }
    else if (raster == "2160")
    {
        fmt.width = 3840;
        fmt.height = 2160;
    }
    else
    {
        auto const x = raster.find('x');
        if (x == std::string::npos)
        {
            throw std::runtime_error("unrecognised raster in " + token);
        }
        fmt.width = std::stoi(raster.substr(0, x));
        fmt.height = std::stoi(raster.substr(x + 1));
    }
    if (!parseRateToken(rate, fmt.rateNum, fmt.rateDen) || fmt.width < 2 || fmt.height < 2 || (fmt.width % 2) != 0)
    {
        throw std::runtime_error("unrecognised video format " + token);
    }
    // MXL takes interlaced flows at 25/1 and 30000/1001 only; replay's interlaced house is 1920×1080.
    if (fmt.interlaced && (fmt.width != 1920 || fmt.height != 1080 || !((fmt.rateNum == 25 && fmt.rateDen == 1) || (fmt.rateNum == 30000 && fmt.rateDen == 1001))))
    {
        throw std::runtime_error("interlaced formats are 1080i25 or 1080i2997 (the frame rate: 1080i25 is 50 fields a second): " + token);
    }
    return fmt;
}

std::vector<std::string> acceptedInterlaceModes(VideoFormat const& house)
{
    if (house.interlaced)
    {
        return {"progressive", "interlaced_tff"};
    }
    return {"progressive"};
}
} // namespace replay
