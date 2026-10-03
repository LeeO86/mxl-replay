#pragma once

#include "media/frame.hpp"

namespace replay
{
enum class ScaleFilter
{
    Bilinear,
    Bicubic
};

// Scale a 10-bit 4:2:2 frame onto another raster. Used when an input is not the house format.
void scaleFrame(Frame10 const& src, Frame10& dst, ScaleFilter filter);
} // namespace replay
