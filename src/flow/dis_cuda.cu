// SPDX-License-Identifier: GPL-3.0-or-later
// CUDA / nvJPEG path for mxl-replay.
// Copyright (C) 2018 Steinar H. Gunderson (Futatabi / Nageru algorithm)
// Copyright (C) 2026 the mxl-replay authors
//
// Host traffic is the compressed JPEG (nvJPEG copies the bitstream) and the
// finished v210 grain. Search, densify, variational refinement, blend and
// v210 pack stay in device memory. Flow pairs are cached on the device.

#include "flow/cuda_flow.hpp"

#include "media/v210.hpp"

#include <nvjpeg.h>

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace replay
{
namespace
{
constexpr int kMaxPatch = 16;

struct DevPlanes
{
    std::uint8_t* y = nullptr;
    std::uint8_t* cb = nullptr;
    std::uint8_t* cr = nullptr;
    int width = 0;
    int height = 0;
    int pitchY = 0;
    int pitchC = 0;
};

struct DevFlow
{
    float* u = nullptr;
    float* v = nullptr;
    int width = 0;
    int height = 0;
};

struct Nv
{
    nvjpegHandle_t handle{};
    nvjpegJpegState_t decoder{};
    nvjpegEncoderState_t encoder{};
    nvjpegEncoderParams_t params{};
    cudaStream_t stream{};
    bool ready = false;
    DevPlanes slots[3];
    std::mutex mutex;
    std::map<std::string, DevFlow> flows;
};

Nv& nv()
{
    static Nv state;
    return state;
}

void releasePlanes(DevPlanes& planes)
{
    cudaFree(planes.y);
    cudaFree(planes.cb);
    cudaFree(planes.cr);
    planes = {};
}

bool ensureSlot(DevPlanes& planes, int width, int height)
{
    int const pitchY = (width + 31) & ~31;
    int const pitchC = ((width / 2) + 31) & ~31;
    if (planes.y != nullptr && planes.width == width && planes.height == height && planes.pitchY == pitchY)
    {
        return true;
    }
    releasePlanes(planes);
    planes.width = width;
    planes.height = height;
    planes.pitchY = pitchY;
    planes.pitchC = pitchC;
    if (cudaMalloc(&planes.y, static_cast<std::size_t>(pitchY) * static_cast<std::size_t>(height)) != cudaSuccess)
    {
        return false;
    }
    if (cudaMalloc(&planes.cb, static_cast<std::size_t>(pitchC) * static_cast<std::size_t>(height)) != cudaSuccess)
    {
        return false;
    }
    if (cudaMalloc(&planes.cr, static_cast<std::size_t>(pitchC) * static_cast<std::size_t>(height)) != cudaSuccess)
    {
        return false;
    }
    return true;
}

bool prepareNv()
{
    auto& state = nv();
    if (state.ready)
    {
        return true;
    }
    if (cudaFlowDeviceCount() <= 0)
    {
        return false;
    }
    if (nvjpegCreateSimple(&state.handle) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    if (nvjpegJpegStateCreate(state.handle, &state.decoder) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    if (cudaStreamCreate(&state.stream) != cudaSuccess)
    {
        return false;
    }
    if (nvjpegEncoderStateCreate(state.handle, &state.encoder, state.stream) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    if (nvjpegEncoderParamsCreate(state.handle, &state.params, state.stream) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    state.ready = true;
    return true;
}

__device__ float sampleU8(std::uint8_t const* image, int pitch, int width, int height, float x, float y)
{
    x = fminf(fmaxf(x, 0.f), static_cast<float>(width - 1));
    y = fminf(fmaxf(y, 0.f), static_cast<float>(height - 1));
    int const x0 = static_cast<int>(floorf(x));
    int const y0 = static_cast<int>(floorf(y));
    int const x1 = min(x0 + 1, width - 1);
    int const y1 = min(y0 + 1, height - 1);
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    float const a = image[y0 * pitch + x0];
    float const b = image[y0 * pitch + x1];
    float const c = image[y1 * pitch + x0];
    float const d = image[y1 * pitch + x1];
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + d * fx) * fy;
}

__device__ float sampleF(float const* image, int width, int height, float x, float y)
{
    x = fminf(fmaxf(x, 0.f), static_cast<float>(width - 1));
    y = fminf(fmaxf(y, 0.f), static_cast<float>(height - 1));
    int const x0 = static_cast<int>(floorf(x));
    int const y0 = static_cast<int>(floorf(y));
    int const x1 = min(x0 + 1, width - 1);
    int const y1 = min(y0 + 1, height - 1);
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    auto at = [&](int px, int py) { return image[py * width + px]; };
    return (at(x0, y0) * (1.f - fx) + at(x1, y0) * fx) * (1.f - fy) + (at(x0, y1) * (1.f - fx) + at(x1, y1) * fx) * fy;
}

__device__ void sobelAt(float const* image, int width, int height, int x, int y, float& gx, float& gy, float& value)
{
    auto at = [&](int px, int py) {
        px = max(0, min(width - 1, px));
        py = max(0, min(height - 1, py));
        return image[py * width + px];
    };
    if (x < 0 || y < 0 || x >= width || y >= height)
    {
        gx = 0.f;
        gy = 0.f;
        value = at(x, y);
        return;
    }
    float const tl = at(x - 1, y - 1);
    float const left = at(x - 1, y);
    float const bl = at(x - 1, y + 1);
    float const top = at(x, y - 1);
    float const bottom = at(x, y + 1);
    float const tr = at(x + 1, y - 1);
    float const right = at(x + 1, y);
    float const br = at(x + 1, y + 1);
    gx = fminf(0.5f, fmaxf(-0.5f, ((tr + 2.f * right + br) - (tl + 2.f * left + bl)) * 0.125f));
    gy = fminf(0.5f, fmaxf(-0.5f, ((bl + 2.f * bottom + br) - (tl + 2.f * top + tr)) * 0.125f));
    value = image[y * width + x];
}

__global__ void yToGrayKernel(std::uint8_t const* y, int pitch, int width, int height, float* gray)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || row >= height)
    {
        return;
    }
    gray[row * width + x] = static_cast<float>(y[row * pitch + x]) / 255.f;
}

__global__ void downsampleKernel(float const* src, int sw, int sh, float* dst, int dw, int dh)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh)
    {
        return;
    }
    float sum = 0.f;
    int n = 0;
    for (int dy = 0; dy < 2; ++dy)
    {
        for (int dx = 0; dx < 2; ++dx)
        {
            int const xx = x * 2 + dx;
            int const yy = y * 2 + dy;
            if (xx < sw && yy < sh)
            {
                sum += src[yy * sw + xx];
                ++n;
            }
        }
    }
    dst[y * dw + x] = n > 0 ? sum / static_cast<float>(n) : 0.f;
}

__global__ void motionDensifyKernel(float const* reference, float const* search, int width, int height, float const* prevU, float const* prevV, int prevW,
    int prevH, int patch, int iterations, int patchesX, int patchesY, float* denseU, float* denseV, float* weight)
{
    int const px = blockIdx.x * blockDim.x + threadIdx.x;
    int const py = blockIdx.y * blockDim.y + threadIdx.y;
    if (px >= patchesX || py >= patchesY || patch > kMaxPatch)
    {
        return;
    }
    float const nx = patchesX == 1 ? 0.f : static_cast<float>(px) / static_cast<float>(patchesX - 1);
    float const ny = patchesY == 1 ? 0.f : static_cast<float>(py) / static_cast<float>(patchesY - 1);
    float initialU = 0.f;
    float initialV = 0.f;
    if (prevU != nullptr && prevW > 0)
    {
        initialU = sampleF(prevU, prevW, prevH, nx * static_cast<float>(prevW - 1), ny * static_cast<float>(prevH - 1)) * static_cast<float>(width) /
                   static_cast<float>(prevW);
        initialV = sampleF(prevV, prevW, prevH, nx * static_cast<float>(prevW - 1), ny * static_cast<float>(prevH - 1)) * static_cast<float>(height) /
                   static_cast<float>(prevH);
    }
    int const lockedX = static_cast<int>(rintf(nx * static_cast<float>(width)));
    int const lockedY = static_cast<int>(rintf(ny * static_cast<float>(height)));
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
            float gx = 0.f;
            float gy = 0.f;
            float value = 0.f;
            sobelAt(reference, width, height, firstX + x, firstY + y, gx, gy, value);
            h00 += gx * gx;
            h11 += gy * gy;
            h01 += gx * gy;
            templateSum += value;
            gradSumX += gx;
            gradSumY += gy;
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
                float gx = 0.f;
                float gy = 0.f;
                float value = 0.f;
                sobelAt(reference, width, height, firstX + x, firstY + y, gx, gy, value);
                float const warped = sampleF(search, width, height, static_cast<float>(firstX + x) + u, static_cast<float>(firstY + y) + v);
                float const residual = warped - value;
                du += gx * residual;
                dv += gy * residual;
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
        float const stepU = inv00 * du + inv01 * dv;
        float const stepV = inv01 * du + inv11 * dv;
        u -= stepU;
        v -= stepV;
    }
    float const moved = hypotf(u - initialU, v - initialV);
    float const margin = static_cast<float>(patch) * 0.5f;
    float const cx = static_cast<float>(lockedX) + u;
    float const cy = static_cast<float>(lockedY) + v;
    bool const outside = cx < -margin || static_cast<float>(width) - cx < -margin || cy < -margin || static_cast<float>(height) - cy < -margin;
    if (moved > margin || outside)
    {
        u = initialU;
        v = initialV;
        meanDiff = firstMean;
    }
    float const half = 0.75f * static_cast<float>(patch);
    float const centerX = nx * static_cast<float>(width);
    float const centerY = ny * static_cast<float>(height);
    int const x0 = max(0, static_cast<int>(floorf(centerX - half)));
    int const x1 = min(width - 1, static_cast<int>(ceilf(centerX + half)));
    int const y0 = max(0, static_cast<int>(floorf(centerY - half)));
    int const y1 = min(height - 1, static_cast<int>(ceilf(centerY + half)));
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            float const i0 = reference[y * width + x];
            float const i1 = sampleF(search, width, height, static_cast<float>(x) + u, static_cast<float>(y) + v);
            float const diff = fabsf(i0 - i1 - meanDiff);
            float const w = 1.f / fmaxf(diff, 2.f / 255.f);
            int const index = y * width + x;
            atomicAdd(denseU + index, u * w);
            atomicAdd(denseV + index, v * w);
            atomicAdd(weight + index, w);
        }
    }
}

__global__ void normalizeFlowKernel(float* u, float* v, float const* weight, int n)
{
    int const index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= n || weight[index] <= 0.f)
    {
        return;
    }
    u[index] /= weight[index];
    v[index] /= weight[index];
}

__device__ float deriv5(float m2, float m1, float p1, float p2)
{
    return (p1 - m1) * (2.f / 3.f) + (m2 - p2) * (1.f / 12.f);
}

__global__ void refineKernel(float const* reference, float const* search, float* u, float* v, int width, int height, int level)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height || ((x + y) & 1) != (level & 1))
    {
        return;
    }
    int const index = y * width + x;
    auto at = [&](float const* image, int px, int py) {
        px = max(0, min(width - 1, px));
        py = max(0, min(height - 1, py));
        return image[py * width + px];
    };
    float const i0 = reference[index];
    float const iw = sampleF(search, width, height, static_cast<float>(x) + u[index], static_cast<float>(y) + v[index]);
    float const ix = deriv5(at(reference, x - 2, y), at(reference, x - 1, y), at(reference, x + 1, y), at(reference, x + 2, y));
    float const iy = deriv5(at(reference, x, y - 2), at(reference, x, y - 1), at(reference, x, y + 1), at(reference, x, y + 2));
    float const it = iw - i0;
    float const beta = 1.f / (ix * ix + iy * iy + 1e-7f);
    float const data = ix * 0.f + iy * 0.f + it;
    constexpr float delta = 0.25f;
    constexpr float alpha = 1.f;
    float const k1 = delta * beta / sqrtf(beta * data * data + 1e-6f);
    float a11 = k1 * ix * ix;
    float a22 = k1 * iy * iy;
    float const a12 = k1 * ix * iy;
    float b1 = -k1 * it * ix;
    float b2 = -k1 * it * iy;
    auto smooth = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= width || py >= height)
        {
            return 0.f;
        }
        float const ux = at(u, px + 1, py) - at(u, px - 1, py);
        float const uy = at(u, px, py + 1) - at(u, px, py - 1);
        float const vx = at(v, px + 1, py) - at(v, px - 1, py);
        float const vy = at(v, px, py + 1) - at(v, px, py - 1);
        return alpha / sqrtf(ux * ux + uy * uy + vx * vx + vy * vy + 1e-6f);
    };
    float const sL = x > 0 ? 0.5f * (smooth(x - 1, y) + smooth(x, y)) : 0.f;
    float const sR = x + 1 < width ? 0.5f * (smooth(x, y) + smooth(x + 1, y)) : 0.f;
    float const sD = y > 0 ? 0.5f * (smooth(x, y - 1) + smooth(x, y)) : 0.f;
    float const sU = y + 1 < height ? 0.5f * (smooth(x, y) + smooth(x, y + 1)) : 0.f;
    a11 += sL + sR + sD + sU;
    a22 += sL + sR + sD + sU;
    b1 += sL * at(u, x - 1, y) + sR * at(u, x + 1, y) + sD * at(u, x, y - 1) + sU * at(u, x, y + 1) - (sL + sR + sD + sU) * u[index];
    b2 += sL * at(v, x - 1, y) + sR * at(v, x + 1, y) + sD * at(v, x, y - 1) + sU * at(v, x, y + 1) - (sL + sR + sD + sU) * v[index];
    constexpr float omega = 1.8f;
    float const invA11 = 1.f / fmaxf(a11, 1e-8f);
    float const invA22 = 1.f / fmaxf(a22, 1e-8f);
    float du = 0.f;
    float dv = 0.f;
    du += omega * ((b1 - a12 * dv) * invA11 - du);
    dv += omega * ((b2 - a12 * du) * invA22 - dv);
    u[index] += du;
    v[index] += dv;
}

dim3 block2()
{
    return dim3(16, 16);
}

dim3 grid2(int width, int height)
{
    return dim3((width + 15) / 16, (height + 15) / 16);
}

bool launchGray(DevPlanes const& planes, float*& gray)
{
    if (cudaMalloc(&gray, static_cast<std::size_t>(planes.width) * static_cast<std::size_t>(planes.height) * sizeof(float)) != cudaSuccess)
    {
        gray = nullptr;
        return false;
    }
    yToGrayKernel<<<grid2(planes.width, planes.height), block2(), 0, nv().stream>>>(planes.y, planes.pitchY, planes.width, planes.height, gray);
    return cudaGetLastError() == cudaSuccess;
}

DevFlow computeFlowDevice(float* reference, float* search, int width, int height, OperatingPoint const& op)
{
    DevFlow flow;
    int const levels = std::max(op.coarsestLevel, op.finestLevel);
    std::vector<float*> pyrA(static_cast<std::size_t>(levels + 1), nullptr);
    std::vector<float*> pyrB(static_cast<std::size_t>(levels + 1), nullptr);
    std::vector<int> ws(static_cast<std::size_t>(levels + 1));
    std::vector<int> hs(static_cast<std::size_t>(levels + 1));
    pyrA[0] = reference;
    pyrB[0] = search;
    ws[0] = width;
    hs[0] = height;
    for (int level = 1; level <= levels; ++level)
    {
        ws[static_cast<std::size_t>(level)] = std::max(1, ws[static_cast<std::size_t>(level - 1)] >> 1);
        hs[static_cast<std::size_t>(level)] = std::max(1, hs[static_cast<std::size_t>(level - 1)] >> 1);
        int const dw = ws[static_cast<std::size_t>(level)];
        int const dh = hs[static_cast<std::size_t>(level)];
        cudaMalloc(&pyrA[static_cast<std::size_t>(level)], static_cast<std::size_t>(dw * dh) * sizeof(float));
        cudaMalloc(&pyrB[static_cast<std::size_t>(level)], static_cast<std::size_t>(dw * dh) * sizeof(float));
        downsampleKernel<<<grid2(dw, dh), block2(), 0, nv().stream>>>(pyrA[static_cast<std::size_t>(level - 1)], ws[static_cast<std::size_t>(level - 1)],
            hs[static_cast<std::size_t>(level - 1)], pyrA[static_cast<std::size_t>(level)], dw, dh);
        downsampleKernel<<<grid2(dw, dh), block2(), 0, nv().stream>>>(pyrB[static_cast<std::size_t>(level - 1)], ws[static_cast<std::size_t>(level - 1)],
            hs[static_cast<std::size_t>(level - 1)], pyrB[static_cast<std::size_t>(level)], dw, dh);
    }
    float* prevU = nullptr;
    float* prevV = nullptr;
    int prevW = 0;
    int prevH = 0;
    float const spacing = static_cast<float>(op.patchSize) * (1.f - op.overlap);
    for (int level = op.coarsestLevel; level >= op.finestLevel; --level)
    {
        int const index = std::min(level, levels);
        int const lw = ws[static_cast<std::size_t>(index)];
        int const lh = hs[static_cast<std::size_t>(index)];
        float* denseU = nullptr;
        float* denseV = nullptr;
        float* weight = nullptr;
        cudaMalloc(&denseU, static_cast<std::size_t>(lw * lh) * sizeof(float));
        cudaMalloc(&denseV, static_cast<std::size_t>(lw * lh) * sizeof(float));
        cudaMalloc(&weight, static_cast<std::size_t>(lw * lh) * sizeof(float));
        cudaMemsetAsync(denseU, 0, static_cast<std::size_t>(lw * lh) * sizeof(float), nv().stream);
        cudaMemsetAsync(denseV, 0, static_cast<std::size_t>(lw * lh) * sizeof(float), nv().stream);
        cudaMemsetAsync(weight, 0, static_cast<std::size_t>(lw * lh) * sizeof(float), nv().stream);
        int const patchesX = 1 + static_cast<int>(std::ceil(static_cast<float>(lw) / std::max(spacing, 1.f)));
        int const patchesY = 1 + static_cast<int>(std::ceil(static_cast<float>(lh) / std::max(spacing, 1.f)));
        motionDensifyKernel<<<grid2(patchesX, patchesY), block2(), 0, nv().stream>>>(pyrA[static_cast<std::size_t>(index)], pyrB[static_cast<std::size_t>(index)], lw, lh, prevU,
            prevV, prevW, prevH, op.patchSize, op.searchIterations, patchesX, patchesY, denseU, denseV, weight);
        normalizeFlowKernel<<<(lw * lh + 255) / 256, 256, 0, nv().stream>>>(denseU, denseV, weight, lw * lh);
        cudaFree(weight);
        if (op.variational)
        {
            int const sweeps = std::max(1, (level + 1) * 5);
            for (int sweep = 0; sweep < sweeps; ++sweep)
            {
                refineKernel<<<grid2(lw, lh), block2(), 0, nv().stream>>>(pyrA[static_cast<std::size_t>(index)], pyrB[static_cast<std::size_t>(index)], denseU, denseV, lw, lh, sweep);
            }
        }
        cudaFree(prevU);
        cudaFree(prevV);
        prevU = denseU;
        prevV = denseV;
        prevW = lw;
        prevH = lh;
    }
    for (int level = 1; level <= levels; ++level)
    {
        cudaFree(pyrA[static_cast<std::size_t>(level)]);
        cudaFree(pyrB[static_cast<std::size_t>(level)]);
    }
    flow.u = prevU;
    flow.v = prevV;
    flow.width = prevW;
    flow.height = prevH;
    return flow;
}

__global__ void blendKernel(std::uint8_t const* ay, std::uint8_t const* acb, std::uint8_t const* acr, std::uint8_t const* by, std::uint8_t const* bcb,
    std::uint8_t const* bcr, int aPitchY, int aPitchC, int bPitchY, int bPitchC, int width, int height, float const* flowU, float const* flowV, int flowW,
    int flowH, float phase, std::uint8_t* oy, std::uint8_t* ocb, std::uint8_t* ocr, int oPitchY, int oPitchC)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
    {
        return;
    }
    float fu = 0.f;
    float fv = 0.f;
    if (flowU != nullptr && flowW > 0)
    {
        float const sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(flowW) / static_cast<float>(width) - 0.5f;
        float const sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(flowH) / static_cast<float>(height) - 0.5f;
        fu = sampleF(flowU, flowW, flowH, sx, sy) / static_cast<float>(flowW);
        fv = sampleF(flowV, flowW, flowH, sx, sy) / static_cast<float>(flowH);
    }
    float const nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
    float const ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
    auto sampleY = [&](std::uint8_t const* image, int pitch, float ox, float oy) {
        return sampleU8(image, pitch, width, height, ox * static_cast<float>(width) - 0.5f, oy * static_cast<float>(height) - 0.5f);
    };
    float const y0 = sampleY(ay, aPitchY, nx - phase * fu, ny - phase * fv);
    float const y1 = sampleY(by, bPitchY, nx + (1.f - phase) * fu, ny + (1.f - phase) * fv);
    float d0 = 0.f;
    float d1 = 0.f;
    if (flowU != nullptr && flowW > 1)
    {
        auto consistency = [&](float ox, float oy, float scale) {
            float const sx = ox * static_cast<float>(flowW) - 0.5f;
            float const sy = oy * static_cast<float>(flowH) - 0.5f;
            float const f0u = sampleF(flowU, flowW, flowH, sx, sy) / static_cast<float>(flowW);
            float const f0v = sampleF(flowV, flowW, flowH, sx, sy) / static_cast<float>(flowH);
            return scale * hypotf((f0u - fu) * static_cast<float>(width), (f0v - fv) * static_cast<float>(height));
        };
        d0 = consistency(nx - phase * fu, ny - phase * fv, phase);
        d1 = consistency(nx + (1.f - phase) * fu, ny + (1.f - phase) * fv, 1.f - phase);
    }
    bool const both = fmaxf(d0, d1) < 3.f;
    float const mixed = both ? y0 + phase * (y1 - y0) : (d0 < d1 ? y0 : y1);
    oy[y * oPitchY + x] = static_cast<std::uint8_t>(fminf(255.f, fmaxf(0.f, mixed)));
    if ((x & 1) == 0)
    {
        int const cw = width / 2;
        int const cx = x / 2;
        auto sampleC = [&](std::uint8_t const* image, int pitch, float ox, float oy) {
            return sampleU8(image, pitch, cw, height, ox * static_cast<float>(cw) - 0.5f, oy * static_cast<float>(height) - 0.5f);
        };
        float const cb0 = sampleC(acb, aPitchC, nx - phase * fu, ny - phase * fv);
        float const cb1 = sampleC(bcb, bPitchC, nx + (1.f - phase) * fu, ny + (1.f - phase) * fv);
        float const cr0 = sampleC(acr, aPitchC, nx - phase * fu, ny - phase * fv);
        float const cr1 = sampleC(bcr, bPitchC, nx + (1.f - phase) * fu, ny + (1.f - phase) * fv);
        float const cb = both ? cb0 + phase * (cb1 - cb0) : (d0 < d1 ? cb0 : cb1);
        float const cr = both ? cr0 + phase * (cr1 - cr0) : (d0 < d1 ? cr0 : cr1);
        ocb[y * oPitchC + cx] = static_cast<std::uint8_t>(fminf(255.f, fmaxf(0.f, cb)));
        ocr[y * oPitchC + cx] = static_cast<std::uint8_t>(fminf(255.f, fmaxf(0.f, cr)));
    }
}

__global__ void copyPlanesKernel(std::uint8_t const* y, std::uint8_t const* cb, std::uint8_t const* cr, int inPitchY, int inPitchC, int width, int height,
    std::uint8_t* oy, std::uint8_t* ocb, std::uint8_t* ocr, int outPitchY, int outPitchC)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || row >= height)
    {
        return;
    }
    oy[row * outPitchY + x] = y[row * inPitchY + x];
    if ((x & 1) == 0 && x / 2 < width / 2)
    {
        ocb[row * outPitchC + x / 2] = cb[row * inPitchC + x / 2];
        ocr[row * outPitchC + x / 2] = cr[row * inPitchC + x / 2];
    }
}

__global__ void packV210Kernel(std::uint8_t const* y, std::uint8_t const* cb, std::uint8_t const* cr, int pitchY, int pitchC, int width, int height,
    std::uint8_t* dst, int rowBytes)
{
    int const row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= height)
    {
        return;
    }
    int const cw = width / 2;
    int const groups = (width + 5) / 6;
    std::uint8_t* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes);
    for (int group = 0; group < groups; ++group)
    {
        std::uint16_t yv[6] = {};
        std::uint16_t cbv[3] = {};
        std::uint16_t crv[3] = {};
        for (int i = 0; i < 6; ++i)
        {
            int const x = group * 6 + i;
            if (x < width)
            {
                yv[i] = static_cast<std::uint16_t>(y[row * pitchY + x]) << 2;
                if ((i % 2) == 0 && x / 2 < cw)
                {
                    cbv[i / 2] = static_cast<std::uint16_t>(cb[row * pitchC + x / 2]) << 2;
                    crv[i / 2] = static_cast<std::uint16_t>(cr[row * pitchC + x / 2]) << 2;
                }
            }
        }
        std::uint32_t words[4];
        words[0] = (cbv[0] & 0x3ffu) | (static_cast<std::uint32_t>(yv[0] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(crv[0] & 0x3ffu) << 20);
        words[1] = (yv[1] & 0x3ffu) | (static_cast<std::uint32_t>(cbv[1] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(yv[2] & 0x3ffu) << 20);
        words[2] = (crv[1] & 0x3ffu) | (static_cast<std::uint32_t>(yv[3] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(cbv[2] & 0x3ffu) << 20);
        words[3] = (yv[4] & 0x3ffu) | (static_cast<std::uint32_t>(crv[2] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(yv[5] & 0x3ffu) << 20);
        for (int byte = 0; byte < 16; ++byte)
        {
            line[group * 16 + byte] = reinterpret_cast<std::uint8_t const*>(words)[byte];
        }
    }
}

__global__ void unpackV210Kernel(std::uint8_t const* src, int rowBytes, int width, int height, std::uint8_t* y, std::uint8_t* cb, std::uint8_t* cr, int pitchY,
    int pitchC)
{
    int const row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= height)
    {
        return;
    }
    int const cw = width / 2;
    int const groups = (width + 5) / 6;
    std::uint8_t const* line = src + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes);
    int x = 0;
    for (int group = 0; group < groups; ++group)
    {
        std::uint32_t words[4];
        for (int byte = 0; byte < 16; ++byte)
        {
            reinterpret_cast<std::uint8_t*>(words)[byte] = line[group * 16 + byte];
        }
        std::uint16_t yv[6] = {static_cast<std::uint16_t>((words[0] >> 10) & 0x3ffu), static_cast<std::uint16_t>(words[1] & 0x3ffu),
            static_cast<std::uint16_t>((words[1] >> 20) & 0x3ffu), static_cast<std::uint16_t>((words[2] >> 10) & 0x3ffu), static_cast<std::uint16_t>(words[3] & 0x3ffu),
            static_cast<std::uint16_t>((words[3] >> 20) & 0x3ffu)};
        std::uint16_t cbv[3] = {static_cast<std::uint16_t>(words[0] & 0x3ffu), static_cast<std::uint16_t>((words[1] >> 10) & 0x3ffu),
            static_cast<std::uint16_t>((words[2] >> 20) & 0x3ffu)};
        std::uint16_t crv[3] = {static_cast<std::uint16_t>((words[0] >> 20) & 0x3ffu), static_cast<std::uint16_t>(words[2] & 0x3ffu),
            static_cast<std::uint16_t>((words[3] >> 10) & 0x3ffu)};
        for (int i = 0; i < 6 && x < width; ++i, ++x)
        {
            y[row * pitchY + x] = static_cast<std::uint8_t>(yv[i] >> 2);
            if ((x & 1) == 0 && x / 2 < cw)
            {
                cb[row * pitchC + x / 2] = static_cast<std::uint8_t>(cbv[i / 2] >> 2);
                cr[row * pitchC + x / 2] = static_cast<std::uint8_t>(crv[i / 2] >> 2);
            }
        }
    }
}

__global__ void planes10Kernel(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, int height, std::uint8_t* oy, std::uint8_t* ocb,
    std::uint8_t* ocr, int pitchY, int pitchC)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || row >= height)
    {
        return;
    }
    oy[row * pitchY + x] = static_cast<std::uint8_t>(y[row * width + x] >> 2);
    if ((x & 1) == 0)
    {
        int const cx = x / 2;
        ocb[row * pitchC + cx] = static_cast<std::uint8_t>(cb[row * (width / 2) + cx] >> 2);
        ocr[row * pitchC + cx] = static_cast<std::uint8_t>(cr[row * (width / 2) + cx] >> 2);
    }
}

nvjpegImage_t imageOf(DevPlanes const& planes)
{
    nvjpegImage_t image{};
    image.channel[0] = planes.y;
    image.channel[1] = planes.cb;
    image.channel[2] = planes.cr;
    image.pitch[0] = static_cast<unsigned>(planes.pitchY);
    image.pitch[1] = static_cast<unsigned>(planes.pitchC);
    image.pitch[2] = static_cast<unsigned>(planes.pitchC);
    return image;
}

std::vector<std::uint8_t> retrieveBitstream(Nv& state)
{
    std::size_t length = 0;
    if (nvjpegEncodeRetrieveBitstream(state.handle, state.encoder, nullptr, &length, state.stream) != NVJPEG_STATUS_SUCCESS)
    {
        return {};
    }
    cudaStreamSynchronize(state.stream);
    std::vector<std::uint8_t> bytes(length);
    if (nvjpegEncodeRetrieveBitstream(state.handle, state.encoder, bytes.data(), &length, state.stream) != NVJPEG_STATUS_SUCCESS)
    {
        return {};
    }
    cudaStreamSynchronize(state.stream);
    bytes.resize(length);
    return bytes;
}

bool encodePlanes(DevPlanes const& planes, int quality, std::vector<std::uint8_t>& bytes)
{
    auto& state = nv();
    if (nvjpegEncoderParamsSetQuality(state.params, quality, state.stream) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    if (nvjpegEncoderParamsSetSamplingFactors(state.params, NVJPEG_CSS_422, state.stream) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    auto image = imageOf(planes);
    if (nvjpegEncodeYUV(state.handle, state.encoder, state.params, &image, NVJPEG_CSS_422, planes.width, planes.height, state.stream) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    bytes = retrieveBitstream(state);
    return !bytes.empty();
}

bool decodeToSlot(std::uint8_t const* jpeg, std::size_t size, DevPlanes& slot)
{
    auto& state = nv();
    int widths[NVJPEG_MAX_COMPONENT] = {};
    int heights[NVJPEG_MAX_COMPONENT] = {};
    nvjpegChromaSubsampling_t subsampling = NVJPEG_CSS_UNKNOWN;
    int components = 0;
    if (nvjpegGetImageInfo(state.handle, jpeg, size, &components, &subsampling, widths, heights) != NVJPEG_STATUS_SUCCESS)
    {
        return false;
    }
    if (widths[0] < 2 || heights[0] < 1 || subsampling != NVJPEG_CSS_422)
    {
        return false;
    }
    if (!ensureSlot(slot, widths[0], heights[0]))
    {
        return false;
    }
    auto image = imageOf(slot);
    return nvjpegDecode(state.handle, state.decoder, jpeg, size, NVJPEG_OUTPUT_YUV, &image, state.stream) == NVJPEG_STATUS_SUCCESS;
}

std::vector<std::uint8_t> downloadV210(DevPlanes const& planes)
{
    int const rowBytes = static_cast<int>(v210RowBytes(planes.width));
    std::size_t const bytes = static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(planes.height);
    std::uint8_t* device = nullptr;
    if (cudaMalloc(&device, bytes) != cudaSuccess)
    {
        return {};
    }
    packV210Kernel<<<planes.height, 1, 0, nv().stream>>>(planes.y, planes.cb, planes.cr, planes.pitchY, planes.pitchC, planes.width, planes.height, device, rowBytes);
    std::vector<std::uint8_t> host(bytes);
    cudaMemcpyAsync(host.data(), device, bytes, cudaMemcpyDeviceToHost, nv().stream);
    cudaStreamSynchronize(nv().stream);
    cudaFree(device);
    if (cudaGetLastError() != cudaSuccess)
    {
        return {};
    }
    return host;
}

std::vector<std::uint8_t> previewOf(DevPlanes const& planes)
{
    DevPlanes small;
    if (!ensureSlot(small, 32, 16))
    {
        return {};
    }
    // Nearest sample into the preview slot, then a tiny nvJPEG download.
    copyPlanesKernel<<<grid2(std::min(planes.width, 32), std::min(planes.height, 16)), block2(), 0, nv().stream>>>(planes.y, planes.cb, planes.cr, planes.pitchY,
        planes.pitchC, std::min(planes.width, 32), std::min(planes.height, 16), small.y, small.cb, small.cr, small.pitchY, small.pitchC);
    std::vector<std::uint8_t> bytes;
    encodePlanes(small, 70, bytes);
    releasePlanes(small);
    return bytes;
}

DevFlow cachedFlow(std::string const& key, DevPlanes const& a, DevPlanes const& b, OperatingPoint const& op)
{
    auto& state = nv();
    auto const found = state.flows.find(key);
    if (found != state.flows.end())
    {
        return found->second;
    }
    if (state.flows.size() > 8)
    {
        auto const oldest = state.flows.begin();
        cudaFree(oldest->second.u);
        cudaFree(oldest->second.v);
        state.flows.erase(oldest);
    }
    float* grayA = nullptr;
    float* grayB = nullptr;
    if (!launchGray(a, grayA) || !launchGray(b, grayB))
    {
        cudaFree(grayA);
        cudaFree(grayB);
        return {};
    }
    DevFlow flow = computeFlowDevice(grayA, grayB, a.width, a.height, op);
    cudaFree(grayA);
    cudaFree(grayB);
    if (flow.u != nullptr)
    {
        state.flows.emplace(key, flow);
    }
    return flow;
}
} // namespace

int cudaFlowDeviceCount()
{
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count < 0)
    {
        return 0;
    }
    return count;
}

bool gpuRenderFromJpeg(std::uint8_t const* jpegA, std::size_t sizeA, std::uint8_t const* jpegB, std::size_t sizeB, float phase, bool interpolate,
    OperatingPoint const& op, std::string const& flowKey, int width, int height, GpuPicture& out)
{
    if (jpegA == nullptr || sizeA == 0 || width < 2 || height < 1)
    {
        return false;
    }
    auto& state = nv();
    std::lock_guard lock{state.mutex};
    if (!prepareNv())
    {
        return false;
    }
    if (!decodeToSlot(jpegA, sizeA, state.slots[0]) || state.slots[0].width != width || state.slots[0].height != height)
    {
        return false;
    }
    bool const needB = jpegB != nullptr && sizeB > 0 && phase > 0.001f && phase < 0.999f;
    if (needB && (!decodeToSlot(jpegB, sizeB, state.slots[1]) || state.slots[1].width != width || state.slots[1].height != height))
    {
        return false;
    }
    DevPlanes* source = &state.slots[0];
    if (needB)
    {
        if (!ensureSlot(state.slots[2], state.slots[0].width, state.slots[0].height))
        {
            return false;
        }
        DevFlow flow;
        if (interpolate)
        {
            flow = cachedFlow(flowKey, state.slots[0], state.slots[1], op);
        }
        blendKernel<<<grid2(state.slots[0].width, state.slots[0].height), block2(), 0, nv().stream>>>(state.slots[0].y, state.slots[0].cb, state.slots[0].cr, state.slots[1].y,
            state.slots[1].cb, state.slots[1].cr, state.slots[0].pitchY, state.slots[0].pitchC, state.slots[1].pitchY, state.slots[1].pitchC, state.slots[0].width,
            state.slots[0].height, flow.u, flow.v, flow.width, flow.height, phase, state.slots[2].y, state.slots[2].cb, state.slots[2].cr, state.slots[2].pitchY,
            state.slots[2].pitchC);
        source = &state.slots[2];
    }
    if (cudaStreamSynchronize(state.stream) != cudaSuccess || cudaGetLastError() != cudaSuccess)
    {
        return false;
    }
    out.v210 = downloadV210(*source);
    out.preview = previewOf(*source);
    return !out.v210.empty();
}

std::vector<std::uint8_t> gpuEncodeV210(std::uint8_t const* packed, int width, int height, int rowBytes, int quality)
{
    if (packed == nullptr || width < 2 || height < 1)
    {
        return {};
    }
    auto& state = nv();
    std::lock_guard lock{state.mutex};
    if (!prepareNv() || !ensureSlot(state.slots[0], width, height))
    {
        return {};
    }
    if (rowBytes <= 0)
    {
        rowBytes = static_cast<int>(v210RowBytes(width));
    }
    std::size_t const bytes = static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height);
    std::uint8_t* device = nullptr;
    if (cudaMalloc(&device, bytes) != cudaSuccess)
    {
        return {};
    }
    cudaMemcpyAsync(device, packed, bytes, cudaMemcpyHostToDevice, state.stream);
    unpackV210Kernel<<<height, 1, 0, nv().stream>>>(device, rowBytes, width, height, state.slots[0].y, state.slots[0].cb, state.slots[0].cr, state.slots[0].pitchY, state.slots[0].pitchC);
    cudaFree(device);
    std::vector<std::uint8_t> jpeg;
    if (!encodePlanes(state.slots[0], quality, jpeg))
    {
        return {};
    }
    return jpeg;
}

std::vector<std::uint8_t> gpuEncodeFrame10(Frame10 const& frame, int quality)
{
    if (frame.width < 2 || frame.y.empty())
    {
        return {};
    }
    auto& state = nv();
    std::lock_guard lock{state.mutex};
    if (!prepareNv() || !ensureSlot(state.slots[0], frame.width, frame.height))
    {
        return {};
    }
    std::uint16_t* y = nullptr;
    std::uint16_t* cb = nullptr;
    std::uint16_t* cr = nullptr;
    std::size_t const yBytes = frame.y.size() * sizeof(std::uint16_t);
    std::size_t const cBytes = frame.cb.size() * sizeof(std::uint16_t);
    if (cudaMalloc(&y, yBytes) != cudaSuccess || cudaMalloc(&cb, cBytes) != cudaSuccess || cudaMalloc(&cr, cBytes) != cudaSuccess)
    {
        cudaFree(y);
        cudaFree(cb);
        cudaFree(cr);
        return {};
    }
    cudaMemcpyAsync(y, frame.y.data(), yBytes, cudaMemcpyHostToDevice, state.stream);
    cudaMemcpyAsync(cb, frame.cb.data(), cBytes, cudaMemcpyHostToDevice, state.stream);
    cudaMemcpyAsync(cr, frame.cr.data(), cBytes, cudaMemcpyHostToDevice, state.stream);
    planes10Kernel<<<grid2(frame.width, frame.height), block2(), 0, nv().stream>>>(y, cb, cr, frame.width, frame.height, state.slots[0].y, state.slots[0].cb, state.slots[0].cr,
        state.slots[0].pitchY, state.slots[0].pitchC);
    cudaFree(y);
    cudaFree(cb);
    cudaFree(cr);
    std::vector<std::uint8_t> jpeg;
    if (!encodePlanes(state.slots[0], quality, jpeg))
    {
        return {};
    }
    return jpeg;
}
} // namespace replay
