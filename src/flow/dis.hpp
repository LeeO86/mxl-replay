// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the Futatabi optical-flow pipeline (part of Nageru).
// Copyright (C) 2018 Steinar H. Gunderson
// Copyright (C) 2026 the mxl-replay authors
//
// Algorithm and operating points follow Futatabi's GPU port of
// Till Kroeger, Radu Timofte, Dengxin Dai and Luc Van Gool,
// "Fast Optical Flow using Dense Inverse Search", ECCV 2016,
// and the occlusion-aware temporal interpolation Futatabi applies on top.

#pragma once

#include <string>
#include <vector>

namespace replay
{
enum class InterpPreset
{
    Fast,
    Balanced,
    Quality
};

// Futatabi operating points. fast = point 1, balanced = point 3 (the one Futatabi
// documents as thoroughly tested; finest level 1 is half resolution), quality = point 4.
struct OperatingPoint
{
    int coarsestLevel = 5;
    int finestLevel = 1;
    int searchIterations = 8;
    int patchSize = 12;
    float overlap = 0.75f;
    bool variational = true;
    float splatSize = 4.f;
};

[[nodiscard]] OperatingPoint operatingPoint(InterpPreset preset);
[[nodiscard]] char const* presetName(InterpPreset preset);
[[nodiscard]] InterpPreset parsePreset(std::string const& text, bool* ok = nullptr);

struct GrayImage
{
    int width = 0;
    int height = 0;
    std::vector<float> samples;

    [[nodiscard]] float at(int x, int y) const { return samples[static_cast<std::size_t>(y * width + x)]; }
};

struct FlowField
{
    int width = 0;
    int height = 0;
    std::vector<float> u;
    std::vector<float> v;

    [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
};

struct Yuv422
{
    int width = 0;
    int height = 0;
    std::vector<float> y;
    std::vector<float> cb;
    std::vector<float> cr;
};

struct FlowPair
{
    FlowField forward;
    FlowField backward;
};

[[nodiscard]] GrayImage grayFromLuma(Yuv422 const& frame);
[[nodiscard]] FlowField computeFlow(GrayImage const& from, GrayImage const& to, OperatingPoint const& op);
[[nodiscard]] FlowPair computeFlowPair(GrayImage const& from, GrayImage const& to, OperatingPoint const& op);
[[nodiscard]] Yuv422 interpolateFrames(Yuv422 const& from, Yuv422 const& to, FlowField const& forward, FlowField const& backward, float phase,
    OperatingPoint const& op);

// 8-bit PSNR / mean SSIM on the luma plane. Images must share a raster.
[[nodiscard]] double psnrLuma(Yuv422 const& a, Yuv422 const& b);
[[nodiscard]] double ssimLuma(Yuv422 const& a, Yuv422 const& b);

// Scale a flow field (pixel units) onto another raster. Used when the operating
// level is coarser than the picture.
[[nodiscard]] FlowField scaleFlow(FlowField const& flow, int width, int height);
} // namespace replay
