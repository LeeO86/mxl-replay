// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the Futatabi optical-flow pipeline (part of Nageru).
// Copyright (C) 2018 Steinar H. Gunderson
// Copyright (C) 2026 the mxl-replay authors
//
// The numeric steps follow futatabi/motion_search.frag, densify.frag,
// prewarp.frag, derivatives.frag, diffusivity.frag, equations.frag, sor.frag,
// splat.frag, hole_fill.frag, hole_blend.frag and blend.frag from Nageru 2.3.4.
// Variational weights are Futatabi's vr_alpha=1, vr_delta=0.25, vr_gamma=0.25.
// Five SOR iterations, outer iterations = pyramid level + 1, omega = 1.8.

#include "flow/dis.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace replay
{
namespace
{
constexpr float kVrAlpha = 1.f;
constexpr float kVrDelta = 0.25f;
constexpr float kVrGamma = 0.25f;
constexpr float kOmega = 1.8f;
constexpr int kSorIterations = 5;

float clampf(float v, float lo, float hi)
{
    return std::max(lo, std::min(hi, v));
}

int clampi(int v, int lo, int hi)
{
    return std::max(lo, std::min(hi, v));
}

float sampleGray(GrayImage const& image, float x, float y)
{
    if (image.width <= 0 || image.height <= 0)
    {
        return 0.f;
    }
    x = clampf(x, 0.f, static_cast<float>(image.width - 1));
    y = clampf(y, 0.f, static_cast<float>(image.height - 1));
    int const x0 = static_cast<int>(std::floor(x));
    int const y0 = static_cast<int>(std::floor(y));
    int const x1 = std::min(x0 + 1, image.width - 1);
    int const y1 = std::min(y0 + 1, image.height - 1);
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    float const a = image.at(x0, y0);
    float const b = image.at(x1, y0);
    float const c = image.at(x0, y1);
    float const d = image.at(x1, y1);
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + d * fx) * fy;
}

float samplePlane(std::vector<float> const& plane, int width, int height, float x, float y)
{
    if (width <= 0 || height <= 0 || plane.empty())
    {
        return 0.f;
    }
    x = clampf(x, 0.f, static_cast<float>(width - 1));
    y = clampf(y, 0.f, static_cast<float>(height - 1));
    int const x0 = static_cast<int>(std::floor(x));
    int const y0 = static_cast<int>(std::floor(y));
    int const x1 = std::min(x0 + 1, width - 1);
    int const y1 = std::min(y0 + 1, height - 1);
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    auto at = [&](int px, int py) { return plane[static_cast<std::size_t>(py * width + px)]; };
    return (at(x0, y0) * (1.f - fx) + at(x1, y0) * fx) * (1.f - fy) + (at(x0, y1) * (1.f - fx) + at(x1, y1) * fx) * fy;
}

GrayImage downsample(GrayImage const& src)
{
    GrayImage dst;
    dst.width = std::max(1, src.width >> 1);
    dst.height = std::max(1, src.height >> 1);
    dst.samples.assign(static_cast<std::size_t>(dst.width * dst.height), 0.f);
    for (int y = 0; y < dst.height; ++y)
    {
        for (int x = 0; x < dst.width; ++x)
        {
            float sum = 0.f;
            int n = 0;
            for (int dy = 0; dy < 2; ++dy)
            {
                for (int dx = 0; dx < 2; ++dx)
                {
                    int const xx = x * 2 + dx;
                    int const yy = y * 2 + dy;
                    if (xx < src.width && yy < src.height)
                    {
                        sum += src.at(xx, yy);
                        ++n;
                    }
                }
            }
            dst.samples[static_cast<std::size_t>(y * dst.width + x)] = n > 0 ? sum / static_cast<float>(n) : 0.f;
        }
    }
    return dst;
}

std::vector<GrayImage> buildPyramid(GrayImage const& full, int levels)
{
    std::vector<GrayImage> pyramid;
    pyramid.push_back(full);
    for (int level = 1; level <= levels; ++level)
    {
        pyramid.push_back(downsample(pyramid.back()));
    }
    return pyramid;
}

struct Grad
{
    float x = 0.f;
    float y = 0.f;
    float value = 0.f;
};

Grad sobelAt(GrayImage const& image, int x, int y)
{
    Grad g;
    if (x < 0 || y < 0 || x >= image.width || y >= image.height)
    {
        return g;
    }
    auto at = [&](int px, int py) {
        px = clampi(px, 0, image.width - 1);
        py = clampi(py, 0, image.height - 1);
        return image.at(px, py);
    };
    float const tl = at(x - 1, y - 1);
    float const left = at(x - 1, y);
    float const bl = at(x - 1, y + 1);
    float const top = at(x, y - 1);
    float const bottom = at(x, y + 1);
    float const tr = at(x + 1, y - 1);
    float const right = at(x + 1, y);
    float const br = at(x + 1, y + 1);
    // y points down, so the vertical Sobel is the opposite of Futatabi's bottom-left GL origin.
    g.x = ((tr + 2.f * right + br) - (tl + 2.f * left + bl)) * 0.125f;
    g.y = ((bl + 2.f * bottom + br) - (tl + 2.f * top + tr)) * 0.125f;
    g.x = clampf(g.x, -0.5f, 0.5f);
    g.y = clampf(g.y, -0.5f, 0.5f);
    g.value = image.at(x, y);
    return g;
}

float sampleFlow(FlowField const& flow, float nx, float ny, bool vertical)
{
    if (flow.width <= 0 || flow.height <= 0)
    {
        return 0.f;
    }
    float const x = clampf(nx, 0.f, 1.f) * static_cast<float>(flow.width - 1);
    float const y = clampf(ny, 0.f, 1.f) * static_cast<float>(flow.height - 1);
    auto const& plane = vertical ? flow.v : flow.u;
    return samplePlane(plane, flow.width, flow.height, x, y);
}

struct PatchFlow
{
    float u = 0.f;
    float v = 0.f;
    float meanDiff = 0.f;
};

PatchFlow searchPatch(GrayImage const& reference, GrayImage const& search, int patch, int iterations, float centerNx, float centerNy, float initialU,
    float initialV)
{
    int const lockedX = static_cast<int>(std::round(centerNx * static_cast<float>(reference.width)));
    int const lockedY = static_cast<int>(std::round(centerNy * static_cast<float>(reference.height)));
    int const firstX = lockedX - patch / 2;
    int const firstY = lockedY - patch / 2;

    float h00 = 0.f;
    float h11 = 0.f;
    float h01 = 0.f;
    float gradSumX = 0.f;
    float gradSumY = 0.f;
    float templateSum = 0.f;
    for (int y = 0; y < patch; ++y)
    {
        for (int x = 0; x < patch; ++x)
        {
            int const px = firstX + x;
            int const py = firstY + y;
            Grad g = sobelAt(reference, px, py);
            if (px < 0 || py < 0 || px >= reference.width || py >= reference.height)
            {
                g.x = 0.f;
                g.y = 0.f;
                g.value = sampleGray(reference, static_cast<float>(px), static_cast<float>(py));
            }
            h00 += g.x * g.x;
            h11 += g.y * g.y;
            h01 += g.x * g.y;
            templateSum += g.value;
            gradSumX += g.x;
            gradSumY += g.y;
        }
    }
    float det = h00 * h11 - h01 * h01;
    if (det < 1e-6f)
    {
        h00 += 1e-6f;
        h11 += 1e-6f;
        det = h00 * h11 - h01 * h01;
    }
    float const inv00 = h11 / det;
    float const inv11 = h00 / det;
    float const inv01 = -h01 / det;

    float u = initialU;
    float v = initialV;
    float meanDiff = 0.f;
    float firstMean = 0.f;
    float const area = static_cast<float>(patch * patch);
    for (int iter = 0; iter < iterations; ++iter)
    {
        float du = 0.f;
        float dv = 0.f;
        float warpedSum = 0.f;
        for (int y = 0; y < patch; ++y)
        {
            for (int x = 0; x < patch; ++x)
            {
                int const px = firstX + x;
                int const py = firstY + y;
                Grad g = sobelAt(reference, px, py);
                if (px < 0 || py < 0 || px >= reference.width || py >= reference.height)
                {
                    g.x = 0.f;
                    g.y = 0.f;
                    g.value = sampleGray(reference, static_cast<float>(px), static_cast<float>(py));
                }
                float const warped = sampleGray(search, static_cast<float>(px) + u, static_cast<float>(py) + v);
                float const residual = warped - g.value;
                du += g.x * residual;
                dv += g.y * residual;
                warpedSum += warped;
            }
        }
        meanDiff = (warpedSum - templateSum) / area;
        du -= gradSumX * meanDiff;
        dv -= gradSumY * meanDiff;
        if (iter == 0)
        {
            firstMean = meanDiff;
        }
        float const su = inv00 * du + inv01 * dv;
        float const sv = inv01 * du + inv11 * dv;
        u -= su;
        v -= sv;
    }

    float const moved = std::sqrt((u - initialU) * (u - initialU) + (v - initialV) * (v - initialV));
    float const cx = static_cast<float>(lockedX) + u;
    float const cy = static_cast<float>(lockedY) + v;
    float const margin = static_cast<float>(patch) * 0.5f;
    bool const outside = cx < -margin || static_cast<float>(reference.width) - cx < -margin || cy < -margin ||
                         static_cast<float>(reference.height) - cy < -margin;
    if (moved > margin || outside)
    {
        u = initialU;
        v = initialV;
        meanDiff = firstMean;
    }
    return PatchFlow{u, v, meanDiff};
}

FlowField motionSearch(GrayImage const& reference, GrayImage const& search, OperatingPoint const& op, FlowField const* previous)
{
    float const spacing = static_cast<float>(op.patchSize) * (1.f - op.overlap);
    int const patchesX = 1 + static_cast<int>(std::ceil(static_cast<float>(reference.width) / std::max(spacing, 1.f)));
    int const patchesY = 1 + static_cast<int>(std::ceil(static_cast<float>(reference.height) / std::max(spacing, 1.f)));
    FlowField dense;
    dense.width = reference.width;
    dense.height = reference.height;
    dense.u.assign(static_cast<std::size_t>(dense.width * dense.height), 0.f);
    dense.v.assign(static_cast<std::size_t>(dense.width * dense.height), 0.f);
    std::vector<float> weight(dense.u.size(), 0.f);

    for (int py = 0; py < patchesY; ++py)
    {
        for (int px = 0; px < patchesX; ++px)
        {
            float const nx = patchesX == 1 ? 0.f : static_cast<float>(px) / static_cast<float>(patchesX - 1);
            float const ny = patchesY == 1 ? 0.f : static_cast<float>(py) / static_cast<float>(patchesY - 1);
            float initialU = 0.f;
            float initialV = 0.f;
            if (previous != nullptr && previous->width > 0)
            {
                float const scaleX = static_cast<float>(reference.width) / static_cast<float>(previous->width);
                float const scaleY = static_cast<float>(reference.height) / static_cast<float>(previous->height);
                initialU = sampleFlow(*previous, nx, ny, false) * scaleX;
                initialV = sampleFlow(*previous, nx, ny, true) * scaleY;
            }
            auto const found = searchPatch(reference, search, op.patchSize, op.searchIterations, nx, ny, initialU, initialV);
            float const cx = nx * static_cast<float>(reference.width);
            float const cy = ny * static_cast<float>(reference.height);
            float const half = 0.75f * static_cast<float>(op.patchSize);
            int const x0 = std::max(0, static_cast<int>(std::floor(cx - half)));
            int const x1 = std::min(reference.width - 1, static_cast<int>(std::ceil(cx + half)));
            int const y0 = std::max(0, static_cast<int>(std::floor(cy - half)));
            int const y1 = std::min(reference.height - 1, static_cast<int>(std::ceil(cy + half)));
            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    float const i0 = reference.at(x, y);
                    float const i1 = sampleGray(search, static_cast<float>(x) + found.u, static_cast<float>(y) + found.v);
                    float diff = i0 - i1 - found.meanDiff;
                    float const w = 1.f / std::max(std::fabs(diff), 2.f / 255.f);
                    std::size_t const i = static_cast<std::size_t>(y * reference.width + x);
                    dense.u[i] += found.u * w;
                    dense.v[i] += found.v * w;
                    weight[i] += w;
                }
            }
        }
    }
    for (std::size_t i = 0; i < weight.size(); ++i)
    {
        if (weight[i] > 0.f)
        {
            dense.u[i] /= weight[i];
            dense.v[i] /= weight[i];
        }
    }
    return dense;
}

float deriv(float m2, float m1, float p1, float p2)
{
    return (p1 - m1) * (2.f / 3.f) + (m2 - p2) * (1.f / 12.f);
}

float planeAt(std::vector<float> const& plane, int width, int height, int x, int y)
{
    x = clampi(x, 0, width - 1);
    y = clampi(y, 0, height - 1);
    return plane[static_cast<std::size_t>(y * width + x)];
}

void variationalRefine(GrayImage const& reference, GrayImage const& search, FlowField& flow, int level)
{
    int const w = flow.width;
    int const h = flow.height;
    std::size_t const n = static_cast<std::size_t>(w * h);
    std::vector<float> mean(n, 0.f);
    std::vector<float> it(n, 0.f);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            std::size_t const i = static_cast<std::size_t>(y * w + x);
            float const i0 = reference.at(x, y);
            float const iw = sampleGray(search, static_cast<float>(x) + flow.u[i], static_cast<float>(y) + flow.v[i]);
            mean[i] = 0.5f * (i0 + iw);
            it[i] = iw - i0;
        }
    }
    std::vector<float> ix(n, 0.f);
    std::vector<float> iy(n, 0.f);
    std::vector<float> beta0(n, 0.f);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            auto m = [&](int dx, int dy) { return planeAt(mean, w, h, x + dx, y + dy); };
            std::size_t const i = static_cast<std::size_t>(y * w + x);
            ix[i] = deriv(m(-2, 0), m(-1, 0), m(1, 0), m(2, 0));
            iy[i] = deriv(m(0, -2), m(0, -1), m(0, 1), m(0, 2));
            beta0[i] = 1.f / (ix[i] * ix[i] + iy[i] * iy[i] + 1e-7f);
        }
    }

    std::vector<float> du(n, 0.f);
    std::vector<float> dv(n, 0.f);
    int const outers = level + 1;
    for (int outer = 0; outer < outers; ++outer)
    {
        bool const zeroDiff = outer == 0;
        std::vector<float> diff(n, 0.f);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                auto flowAt = [&](int px, int py) {
                    px = clampi(px, 0, w - 1);
                    py = clampi(py, 0, h - 1);
                    std::size_t const j = static_cast<std::size_t>(py * w + px);
                    float uu = flow.u[j];
                    float vv = flow.v[j];
                    if (!zeroDiff)
                    {
                        uu += du[j];
                        vv += dv[j];
                    }
                    return std::pair<float, float>{uu, vv};
                };
                auto right = flowAt(x + 1, y);
                auto left = flowAt(x - 1, y);
                auto up = flowAt(x, y + 1);
                auto down = flowAt(x, y - 1);
                float const ux = right.first - left.first;
                float const vx = right.second - left.second;
                float const uy = up.first - down.first;
                float const vy = up.second - down.second;
                std::size_t const i = static_cast<std::size_t>(y * w + x);
                diff[i] = kVrAlpha / std::sqrt(ux * ux + uy * uy + vx * vx + vy * vy + 0.001f * 0.001f);
            }
        }
        auto smoothBetween = [&](int x0, int y0, int x1, int y1) {
            if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0 || x0 >= w || y0 >= h || x1 >= w || y1 >= h)
            {
                return 0.f;
            }
            std::size_t const a = static_cast<std::size_t>(y0 * w + x0);
            std::size_t const b = static_cast<std::size_t>(y1 * w + x1);
            return 0.5f * (diff[a] + diff[b]);
        };

        struct Eq
        {
            float invA11 = 0.f;
            float a12 = 0.f;
            float invA22 = 0.f;
            float b1 = 0.f;
            float b2 = 0.f;
            float smoothL = 0.f;
            float smoothR = 0.f;
            float smoothD = 0.f;
            float smoothU = 0.f;
        };
        std::vector<Eq> eqs(n);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                std::size_t const i = static_cast<std::size_t>(y * w + x);
                float const du0 = zeroDiff ? 0.f : du[i];
                float const dv0 = zeroDiff ? 0.f : dv[i];
                float const Ix = ix[i];
                float const Iy = iy[i];
                float const It = it[i];
                float const b0 = beta0[i];
                float const data = Ix * du0 + Iy * dv0 + It;
                float const k1 = kVrDelta * b0 / std::sqrt(b0 * data * data + 1e-6f);
                float a11 = k1 * Ix * Ix;
                float a12 = k1 * Ix * Iy;
                float a22 = k1 * Iy * Iy;
                float b1 = -k1 * It * Ix;
                float b2 = -k1 * It * Iy;

                auto ixy = [&](int dx, int dy) {
                    return std::pair<float, float>{planeAt(ix, w, h, x + dx, y + dy), planeAt(iy, w, h, x + dx, y + dy)};
                };
                auto xm2 = ixy(-2, 0);
                auto xm1 = ixy(-1, 0);
                auto xp1 = ixy(1, 0);
                auto xp2 = ixy(2, 0);
                float const ixx = deriv(xm2.first, xm1.first, xp1.first, xp2.first);
                float const ixyv = deriv(xm2.second, xm1.second, xp1.second, xp2.second);
                float const iyy = deriv(planeAt(iy, w, h, x, y - 2), planeAt(iy, w, h, x, y - 1), planeAt(iy, w, h, x, y + 1), planeAt(iy, w, h, x, y + 2));
                float const ixt = deriv(planeAt(it, w, h, x - 2, y), planeAt(it, w, h, x - 1, y), planeAt(it, w, h, x + 1, y), planeAt(it, w, h, x + 2, y));
                float const iyt = 0.f;
                float const betaX = 1.f / (ixx * ixx + ixyv * ixyv + 1e-7f);
                float const betaY = 1.f / (ixyv * ixyv + iyy * iyy + 1e-7f);
                float const gradRes = betaX * (ixx * du0 + ixyv * dv0 + ixt) * (ixx * du0 + ixyv * dv0 + ixt) +
                                       betaY * (ixyv * du0 + iyy * dv0 + iyt) * (ixyv * du0 + iyy * dv0 + iyt);
                float const k2 = kVrGamma / std::sqrt(gradRes + 1e-6f);
                float const kx = k2 * betaX;
                float const ky = k2 * betaY;
                a11 += kx * ixx * ixx + ky * ixyv * ixyv;
                a12 += kx * ixx * ixyv + ky * ixyv * iyy;
                a22 += kx * ixyv * ixyv + ky * iyy * iyy;
                b1 -= kx * ixx * ixt + ky * ixyv * iyt;
                b2 -= kx * ixyv * ixt + ky * iyy * iyt;

                float const sL = smoothBetween(x - 1, y, x, y);
                float const sR = smoothBetween(x, y, x + 1, y);
                float const sD = smoothBetween(x, y - 1, x, y);
                float const sU = smoothBetween(x, y, x, y + 1);
                a11 += sL + sR + sD + sU;
                a22 += sL + sR + sD + sU;
                auto base = [&](int px, int py) {
                    px = clampi(px, 0, w - 1);
                    py = clampi(py, 0, h - 1);
                    std::size_t const j = static_cast<std::size_t>(py * w + px);
                    return std::pair<float, float>{flow.u[j], flow.v[j]};
                };
                auto c = base(x, y);
                auto l = base(x - 1, y);
                auto r = base(x + 1, y);
                auto dwn = base(x, y - 1);
                auto up = base(x, y + 1);
                b1 += sL * l.first + sR * r.first + sD * dwn.first + sU * up.first - (sL + sR + sD + sU) * c.first;
                b2 += sL * l.second + sR * r.second + sD * dwn.second + sU * up.second - (sL + sR + sD + sU) * c.second;

                Eq eq;
                eq.invA11 = 1.f / std::max(a11, 1e-8f);
                eq.a12 = a12;
                eq.invA22 = 1.f / std::max(a22, 1e-8f);
                eq.b1 = b1;
                eq.b2 = b2;
                eq.smoothL = sL;
                eq.smoothR = sR;
                eq.smoothD = sD;
                eq.smoothU = sU;
                eqs[i] = eq;
            }
        }

        for (int iter = 0; iter < kSorIterations; ++iter)
        {
            for (int phase = 0; phase < 2; ++phase)
            {
                for (int y = 0; y < h; ++y)
                {
                    for (int x = 0; x < w; ++x)
                    {
                        if (((x + y) & 1) != phase)
                        {
                            continue;
                        }
                        std::size_t const i = static_cast<std::size_t>(y * w + x);
                        Eq const& eq = eqs[i];
                        int const nonzero = zeroDiff ? (iter == 0 ? phase : 2) : 2;
                        float b1 = eq.b1;
                        float b2 = eq.b2;
                        if (nonzero == 0)
                        {
                            du[i] = kOmega * b1 * eq.invA11;
                            dv[i] = kOmega * b2 * eq.invA22;
                            continue;
                        }
                        auto neigh = [&](int px, int py) {
                            if (px < 0 || py < 0 || px >= w || py >= h)
                            {
                                return std::pair<float, float>{0.f, 0.f};
                            }
                            std::size_t const j = static_cast<std::size_t>(py * w + px);
                            return std::pair<float, float>{du[j], dv[j]};
                        };
                        auto l = neigh(x - 1, y);
                        auto r = neigh(x + 1, y);
                        auto dwn = neigh(x, y - 1);
                        auto up = neigh(x, y + 1);
                        b1 += eq.smoothL * l.first + eq.smoothR * r.first + eq.smoothD * dwn.first + eq.smoothU * up.first;
                        b2 += eq.smoothL * l.second + eq.smoothR * r.second + eq.smoothD * dwn.second + eq.smoothU * up.second;
                        if (nonzero == 1)
                        {
                            du[i] = 0.f;
                            dv[i] = 0.f;
                        }
                        float const sigmaU = eq.a12 * dv[i];
                        du[i] += kOmega * ((b1 - sigmaU) * eq.invA11 - du[i]);
                        float const sigmaV = eq.a12 * du[i];
                        dv[i] += kOmega * ((b2 - sigmaV) * eq.invA22 - dv[i]);
                    }
                }
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        flow.u[i] += du[i];
        flow.v[i] += dv[i];
    }
}

FlowField computeOne(GrayImage const& from, GrayImage const& to, OperatingPoint const& op)
{
    int const levels = std::max(op.coarsestLevel, op.finestLevel);
    auto const pyrFrom = buildPyramid(from, levels);
    auto const pyrTo = buildPyramid(to, levels);
    FlowField previous;
    for (int level = op.coarsestLevel; level >= op.finestLevel; --level)
    {
        int const index = std::min(level, static_cast<int>(pyrFrom.size()) - 1);
        GrayImage const& a = pyrFrom[static_cast<std::size_t>(index)];
        GrayImage const& b = pyrTo[static_cast<std::size_t>(index)];
        FlowField flow = motionSearch(a, b, op, previous.empty() ? nullptr : &previous);
        if (op.variational)
        {
            variationalRefine(a, b, flow, level);
        }
        previous = std::move(flow);
    }
    return previous;
}

float sampleFlowPx(FlowField const& flow, float x, float y, bool vertical)
{
    if (flow.empty())
    {
        return 0.f;
    }
    auto const& plane = vertical ? flow.v : flow.u;
    return samplePlane(plane, flow.width, flow.height, x, y);
}

FlowField splatFlow(GrayImage const& from, GrayImage const& to, FlowField const& forward, FlowField const& backward, float phase, float splatSize)
{
    int const w = forward.width;
    int const h = forward.height;
    FlowField out;
    out.width = w;
    out.height = h;
    out.u.assign(static_cast<std::size_t>(w * h), 1000.f);
    out.v.assign(static_cast<std::size_t>(w * h), 1000.f);
    std::vector<float> depth(out.u.size(), 1.f);
    auto paint = [&](FlowField const& src, float alpha, bool reverse) {
        int const radius = std::max(0, static_cast<int>(std::ceil(splatSize * 0.5f)));
        for (int y = 0; y < src.height; ++y)
        {
            for (int x = 0; x < src.width; ++x)
            {
                std::size_t const i = static_cast<std::size_t>(y * src.width + x);
                float fu = src.u[i];
                float fv = src.v[i];
                if (reverse)
                {
                    fu = -fu;
                    fv = -fv;
                }
                float const nx = (static_cast<float>(x) + 0.5f + fu * alpha) * static_cast<float>(w) / static_cast<float>(src.width) - 0.5f;
                float const ny = (static_cast<float>(y) + 0.5f + fv * alpha) * static_cast<float>(h) / static_cast<float>(src.height) - 0.5f;
                float const i0 = sampleGray(from, static_cast<float>(x) * static_cast<float>(from.width) / static_cast<float>(src.width),
                    static_cast<float>(y) * static_cast<float>(from.height) / static_cast<float>(src.height));
                float const i1 = sampleGray(to, static_cast<float>(x) * static_cast<float>(to.width) / static_cast<float>(src.width) + fu,
                    static_cast<float>(y) * static_cast<float>(to.height) / static_cast<float>(src.height) + fv);
                float const d = 0.125f * std::fabs(i1 - i0);
                float const nu = fu / static_cast<float>(src.width);
                float const nv = fv / static_cast<float>(src.height);
                for (int oy = -radius; oy <= radius; ++oy)
                {
                    for (int ox = -radius; ox <= radius; ++ox)
                    {
                        int const dx = static_cast<int>(std::round(nx)) + ox;
                        int const dy = static_cast<int>(std::round(ny)) + oy;
                        if (dx < 0 || dy < 0 || dx >= w || dy >= h)
                        {
                            continue;
                        }
                        std::size_t const j = static_cast<std::size_t>(dy * w + dx);
                        if (d < depth[j])
                        {
                            depth[j] = d;
                            out.u[j] = nu * static_cast<float>(w);
                            out.v[j] = nv * static_cast<float>(h);
                        }
                    }
                }
            }
        }
    };
    paint(forward, phase, false);
    if (!backward.empty())
    {
        paint(backward, 1.f - phase, true);
    }
    auto fillDirection = [&](int stepX, int stepY) {
        FlowField filled = out;
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                int const sx = stepX > 0 ? x : w - 1 - x;
                int const sy = stepY > 0 ? y : h - 1 - y;
                std::size_t const i = static_cast<std::size_t>(sy * w + sx);
                if (out.u[i] < 100.f)
                {
                    continue;
                }
                int const px = sx - stepX;
                int const py = sy - stepY;
                if (px < 0 || py < 0 || px >= w || py >= h)
                {
                    continue;
                }
                std::size_t const p = static_cast<std::size_t>(py * w + px);
                if (filled.u[p] < 100.f)
                {
                    filled.u[i] = filled.u[p];
                    filled.v[i] = filled.v[p];
                }
            }
        }
        return filled;
    };
    FlowField const left = fillDirection(1, 0);
    FlowField const right = fillDirection(-1, 0);
    FlowField const up = fillDirection(0, 1);
    FlowField const down = fillDirection(0, -1);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            std::size_t const i = static_cast<std::size_t>(y * w + x);
            if (out.u[i] < 100.f)
            {
                continue;
            }
            float su = 0.f;
            float sv = 0.f;
            int n = 0;
            auto acc = [&](FlowField const& src) {
                if (src.u[i] < 100.f)
                {
                    su += src.u[i];
                    sv += src.v[i];
                    ++n;
                }
            };
            acc(left);
            acc(right);
            acc(up);
            acc(down);
            if (n == 0)
            {
                out.u[i] = 0.f;
                out.v[i] = 0.f;
            }
            else
            {
                out.u[i] = su / static_cast<float>(n);
                out.v[i] = sv / static_cast<float>(n);
            }
        }
    }
    return out;
}

float sampleChroma(Yuv422 const& frame, float x, float y, bool cr)
{
    int const cw = std::max(1, frame.width / 2);
    auto const& plane = cr ? frame.cr : frame.cb;
    return samplePlane(plane, cw, frame.height, x * 0.5f, y);
}
} // namespace

OperatingPoint operatingPoint(InterpPreset preset)
{
    OperatingPoint op;
    if (preset == InterpPreset::Fast)
    {
        op = OperatingPoint{5, 3, 8, 8, 0.30f, false, 1.f};
    }
    else if (preset == InterpPreset::Quality)
    {
        op = OperatingPoint{5, 0, 128, 12, 0.75f, true, 8.f};
    }
    else
    {
        op = OperatingPoint{5, 1, 8, 12, 0.75f, true, 4.f};
    }
    return op;
}

char const* presetName(InterpPreset preset)
{
    switch (preset)
    {
    case InterpPreset::Fast:
        return "fast";
    case InterpPreset::Quality:
        return "quality";
    case InterpPreset::Balanced:
        return "balanced";
    }
    return "balanced";
}

InterpPreset parsePreset(std::string const& text, bool* ok)
{
    if (ok != nullptr)
    {
        *ok = true;
    }
    if (text == "fast")
    {
        return InterpPreset::Fast;
    }
    if (text == "quality")
    {
        return InterpPreset::Quality;
    }
    if (text == "balanced")
    {
        return InterpPreset::Balanced;
    }
    if (ok != nullptr)
    {
        *ok = false;
    }
    return InterpPreset::Balanced;
}

GrayImage grayFromLuma(Yuv422 const& frame)
{
    GrayImage gray;
    gray.width = frame.width;
    gray.height = frame.height;
    gray.samples = frame.y;
    return gray;
}

FlowField computeFlow(GrayImage const& from, GrayImage const& to, OperatingPoint const& op)
{
    if (from.width != to.width || from.height != to.height || from.width <= 0)
    {
        return {};
    }
    return computeOne(from, to, op);
}

FlowPair computeFlowPair(GrayImage const& from, GrayImage const& to, OperatingPoint const& op)
{
    FlowPair pair;
    pair.forward = computeFlow(from, to, op);
    pair.backward = computeFlow(to, from, op);
    return pair;
}

FlowField scaleFlow(FlowField const& flow, int width, int height)
{
    FlowField out;
    out.width = width;
    out.height = height;
    if (flow.empty() || width <= 0 || height <= 0)
    {
        return out;
    }
    out.u.resize(static_cast<std::size_t>(width * height));
    out.v.resize(static_cast<std::size_t>(width * height));
    float const sx = static_cast<float>(width) / static_cast<float>(flow.width);
    float const sy = static_cast<float>(height) / static_cast<float>(flow.height);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float const fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(flow.width) / static_cast<float>(width) - 0.5f;
            float const fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(flow.height) / static_cast<float>(height) - 0.5f;
            std::size_t const i = static_cast<std::size_t>(y * width + x);
            out.u[i] = sampleFlowPx(flow, fx, fy, false) * sx;
            out.v[i] = sampleFlowPx(flow, fx, fy, true) * sy;
        }
    }
    return out;
}

Yuv422 interpolateFrames(Yuv422 const& from, Yuv422 const& to, FlowField const& forward, FlowField const& backward, float phase, OperatingPoint const& op)
{
    Yuv422 out;
    out.width = from.width;
    out.height = from.height;
    if (from.width <= 0 || to.width != from.width || to.height != from.height)
    {
        return out;
    }
    phase = clampf(phase, 0.f, 1.f);
    GrayImage const ga = grayFromLuma(from);
    GrayImage const gb = grayFromLuma(to);
    FlowField flow = forward;
    if (!backward.empty() && op.splatSize > 0.f)
    {
        flow = splatFlow(ga, gb, forward, backward, phase, op.splatSize);
    }
    std::size_t const n = static_cast<std::size_t>(out.width * out.height);
    out.y.resize(n);
    out.cb.resize(static_cast<std::size_t>((out.width / 2) * out.height));
    out.cr.resize(out.cb.size());
    int const cw = std::max(1, out.width / 2);
    for (int y = 0; y < out.height; ++y)
    {
        for (int x = 0; x < out.width; ++x)
        {
            float const nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(out.width);
            float const ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(out.height);
            float const fu = flow.empty() ? 0.f : sampleFlowPx(flow, nx * static_cast<float>(flow.width) - 0.5f, ny * static_cast<float>(flow.height) - 0.5f, false) /
                                                       static_cast<float>(flow.width);
            float const fv = flow.empty() ? 0.f : sampleFlowPx(flow, nx * static_cast<float>(flow.width) - 0.5f, ny * static_cast<float>(flow.height) - 0.5f, true) /
                                                       static_cast<float>(flow.height);
            auto sampleY = [&](Yuv422 const& image, float ox, float oy) {
                float const px = ox * static_cast<float>(image.width) - 0.5f;
                float const py = oy * static_cast<float>(image.height) - 0.5f;
                return samplePlane(image.y, image.width, image.height, px, py);
            };
            float const y0 = sampleY(from, nx - phase * fu, ny - phase * fv);
            float const y1 = sampleY(to, nx + (1.f - phase) * fu, ny + (1.f - phase) * fv);
            auto flowAt = [&](float ox, float oy) {
                if (flow.empty())
                {
                    return std::pair<float, float>{0.f, 0.f};
                }
                float const uu = sampleFlowPx(flow, ox * static_cast<float>(flow.width) - 0.5f, oy * static_cast<float>(flow.height) - 0.5f, false) /
                                 static_cast<float>(flow.width);
                float const vv = sampleFlowPx(flow, ox * static_cast<float>(flow.width) - 0.5f, oy * static_cast<float>(flow.height) - 0.5f, true) /
                                 static_cast<float>(flow.height);
                return std::pair<float, float>{uu, vv};
            };
            auto f0 = flowAt(nx - phase * fu, ny - phase * fv);
            auto f1 = flowAt(nx + (1.f - phase) * fu, ny + (1.f - phase) * fv);
            float const dx0 = (f0.first - fu) * static_cast<float>(out.width);
            float const dy0 = (f0.second - fv) * static_cast<float>(out.height);
            float const dx1 = (f1.first - fu) * static_cast<float>(out.width);
            float const dy1 = (f1.second - fv) * static_cast<float>(out.height);
            float const d0 = phase * std::sqrt(dx0 * dx0 + dy0 * dy0);
            float const d1 = (1.f - phase) * std::sqrt(dx1 * dx1 + dy1 * dy1);
            float yv = 0.f;
            if (std::max(d0, d1) < 3.f)
            {
                yv = y0 + phase * (y1 - y0);
            }
            else if (d0 < d1)
            {
                yv = y0;
            }
            else
            {
                yv = y1;
            }
            out.y[static_cast<std::size_t>(y * out.width + x)] = yv;
            if ((x & 1) == 0)
            {
                int const cxi = x / 2;
                auto sampleC = [&](Yuv422 const& image, float ox, float oy, bool cr) {
                    float const px = ox * static_cast<float>(image.width) - 0.5f;
                    float const py = oy * static_cast<float>(image.height) - 0.5f;
                    return sampleChroma(image, px, py, cr);
                };
                float const cb0 = sampleC(from, nx - phase * fu, ny - phase * fv, false);
                float const cb1 = sampleC(to, nx + (1.f - phase) * fu, ny + (1.f - phase) * fv, false);
                float const cr0 = sampleC(from, nx - phase * fu, ny - phase * fv, true);
                float const cr1 = sampleC(to, nx + (1.f - phase) * fu, ny + (1.f - phase) * fv, true);
                float cb = cb0;
                float cr = cr0;
                if (std::max(d0, d1) < 3.f)
                {
                    cb = cb0 + phase * (cb1 - cb0);
                    cr = cr0 + phase * (cr1 - cr0);
                }
                else if (!(d0 < d1))
                {
                    cb = cb1;
                    cr = cr1;
                }
                std::size_t const ci = static_cast<std::size_t>(y * cw + cxi);
                out.cb[ci] = cb;
                out.cr[ci] = cr;
            }
        }
    }
    return out;
}

double psnrLuma(Yuv422 const& a, Yuv422 const& b)
{
    if (a.width != b.width || a.height != b.height || a.y.empty())
    {
        return 0.0;
    }
    double mse = 0.0;
    for (std::size_t i = 0; i < a.y.size(); ++i)
    {
        double const d = (static_cast<double>(a.y[i]) - static_cast<double>(b.y[i])) * 255.0;
        mse += d * d;
    }
    mse /= static_cast<double>(a.y.size());
    if (mse <= 1e-12)
    {
        return 99.0;
    }
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double ssimLuma(Yuv422 const& a, Yuv422 const& b)
{
    if (a.width != b.width || a.height != b.height || a.width < 8 || a.height < 8)
    {
        return 0.0;
    }
    constexpr int kWindow = 8;
    double sum = 0.0;
    int windows = 0;
    for (int y = 0; y + kWindow <= a.height; y += kWindow)
    {
        for (int x = 0; x + kWindow <= a.width; x += kWindow)
        {
            double muA = 0.0;
            double muB = 0.0;
            int const count = kWindow * kWindow;
            for (int yy = 0; yy < kWindow; ++yy)
            {
                for (int xx = 0; xx < kWindow; ++xx)
                {
                    muA += a.y[static_cast<std::size_t>((y + yy) * a.width + x + xx)];
                    muB += b.y[static_cast<std::size_t>((y + yy) * b.width + x + xx)];
                }
            }
            muA /= count;
            muB /= count;
            double varA = 0.0;
            double varB = 0.0;
            double cov = 0.0;
            for (int yy = 0; yy < kWindow; ++yy)
            {
                for (int xx = 0; xx < kWindow; ++xx)
                {
                    double const da = a.y[static_cast<std::size_t>((y + yy) * a.width + x + xx)] - muA;
                    double const db = b.y[static_cast<std::size_t>((y + yy) * b.width + x + xx)] - muB;
                    varA += da * da;
                    varB += db * db;
                    cov += da * db;
                }
            }
            varA /= count;
            varB /= count;
            cov /= count;
            constexpr double c1 = (0.01 * 0.01);
            constexpr double c2 = (0.03 * 0.03);
            sum += ((2.0 * muA * muB + c1) * (2.0 * cov + c2)) / ((muA * muA + muB * muB + c1) * (varA + varB + c2));
            ++windows;
        }
    }
    return windows > 0 ? sum / static_cast<double>(windows) : 0.0;
}
} // namespace replay
