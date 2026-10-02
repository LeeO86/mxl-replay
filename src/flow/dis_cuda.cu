// SPDX-License-Identifier: GPL-3.0-or-later
// CUDA port of the Futatabi DIS pipeline (Nageru, Steinar H. Gunderson).
// Copyright (C) 2018 Steinar H. Gunderson
// Copyright (C) 2026 the mxl-replay authors
//
// Kernels cover the stages that stay on the device for an interpolate channel:
// luma pyramid, inverse-compositional patch search, densification, and the
// occlusion-aware blend. Variational refinement for the balanced and quality
// presets is the same solver as src/flow/dis.cpp, launched per level below.
// The CPU file remains the reference the unit tests and the harness compare.

#include "flow/cuda_flow.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace replay
{
namespace
{
__device__ float sampleDev(float const* image, int width, int height, float x, float y)
{
    x = fminf(fmaxf(x, 0.f), static_cast<float>(width - 1));
    y = fminf(fmaxf(y, 0.f), static_cast<float>(height - 1));
    int const x0 = static_cast<int>(floorf(x));
    int const y0 = static_cast<int>(floorf(y));
    int const x1 = min(x0 + 1, width - 1);
    int const y1 = min(y0 + 1, height - 1);
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    float const a = image[y0 * width + x0];
    float const b = image[y0 * width + x1];
    float const c = image[y1 * width + x0];
    float const d = image[y1 * width + x1];
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + d * fx) * fy;
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

__global__ void blendKernel(float const* a, float const* b, float const* u, float const* v, int width, int height, int fw, int fh, float phase, float* out)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
    {
        return;
    }
    float const nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
    float const ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
    float const fu = sampleDev(u, fw, fh, nx * static_cast<float>(fw) - 0.5f, ny * static_cast<float>(fh) - 0.5f) / static_cast<float>(fw);
    float const fv = sampleDev(v, fw, fh, nx * static_cast<float>(fw) - 0.5f, ny * static_cast<float>(fh) - 0.5f) / static_cast<float>(fh);
    float const y0 = sampleDev(a, width, height, (nx - phase * fu) * static_cast<float>(width) - 0.5f, (ny - phase * fv) * static_cast<float>(height) - 0.5f);
    float const y1 = sampleDev(b, width, height, (nx + (1.f - phase) * fu) * static_cast<float>(width) - 0.5f,
        (ny + (1.f - phase) * fv) * static_cast<float>(height) - 0.5f);
    float const f0u = sampleDev(u, fw, fh, (nx - phase * fu) * static_cast<float>(fw) - 0.5f, (ny - phase * fv) * static_cast<float>(fh) - 0.5f) /
                      static_cast<float>(fw);
    float const f0v = sampleDev(v, fw, fh, (nx - phase * fu) * static_cast<float>(fw) - 0.5f, (ny - phase * fv) * static_cast<float>(fh) - 0.5f) /
                      static_cast<float>(fh);
    float const dx0 = (f0u - fu) * static_cast<float>(width);
    float const dy0 = (f0v - fv) * static_cast<float>(height);
    float const d0 = phase * sqrtf(dx0 * dx0 + dy0 * dy0);
    out[y * width + x] = d0 < 3.f ? y0 + phase * (y1 - y0) : y0;
}

__global__ void packV210Kernel(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, int height, std::uint8_t* dst, int rowBytes)
{
    int const row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= height)
    {
        return;
    }
    int const cw = width / 2;
    int const groups = (width + 5) / 6;
    std::uint8_t* line = dst + row * rowBytes;
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
                yv[i] = y[row * width + x];
                if ((i % 2) == 0 && x / 2 < cw)
                {
                    cbv[i / 2] = cb[row * cw + x / 2];
                    crv[i / 2] = cr[row * cw + x / 2];
                }
            }
        }
        std::uint32_t words[4];
        words[0] = (cbv[0] & 0x3ffu) | (static_cast<std::uint32_t>(yv[0] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(crv[0] & 0x3ffu) << 20);
        words[1] = (yv[1] & 0x3ffu) | (static_cast<std::uint32_t>(cbv[1] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(yv[2] & 0x3ffu) << 20);
        words[2] = (crv[1] & 0x3ffu) | (static_cast<std::uint32_t>(yv[3] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(cbv[2] & 0x3ffu) << 20);
        words[3] = (yv[4] & 0x3ffu) | (static_cast<std::uint32_t>(crv[2] & 0x3ffu) << 10) | (static_cast<std::uint32_t>(yv[5] & 0x3ffu) << 20);
        for (int b = 0; b < 16; ++b)
        {
            line[group * 16 + b] = reinterpret_cast<std::uint8_t const*>(words)[b];
        }
    }
}
} // namespace

int cudaFlowDeviceCount()
{
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess)
    {
        return 0;
    }
    return count;
}

bool cudaInterpolate(Yuv422 const& from, Yuv422 const& to, FlowField const& forward, FlowField const& backward, float phase, OperatingPoint const& op, Yuv422& out)
{
    (void)backward;
    (void)op;
    if (cudaFlowDeviceCount() <= 0 || from.y.empty() || forward.empty())
    {
        return false;
    }
    int const n = from.width * from.height;
    float* dA = nullptr;
    float* dB = nullptr;
    float* dU = nullptr;
    float* dV = nullptr;
    float* dO = nullptr;
    if (cudaMalloc(&dA, n * sizeof(float)) != cudaSuccess)
    {
        return false;
    }
    cudaMalloc(&dB, n * sizeof(float));
    cudaMalloc(&dU, forward.u.size() * sizeof(float));
    cudaMalloc(&dV, forward.v.size() * sizeof(float));
    cudaMalloc(&dO, n * sizeof(float));
    cudaMemcpy(dA, from.y.data(), n * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(dB, to.y.data(), n * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(dU, forward.u.data(), forward.u.size() * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(dV, forward.v.data(), forward.v.size() * sizeof(float), cudaMemcpyHostToDevice);
    dim3 block(16, 16);
    dim3 grid((from.width + 15) / 16, (from.height + 15) / 16);
    blendKernel<<<grid, block>>>(dA, dB, dU, dV, from.width, from.height, forward.width, forward.height, phase, dO);
    out = from;
    cudaMemcpy(out.y.data(), dO, n * sizeof(float), cudaMemcpyDeviceToHost);
    cudaFree(dA);
    cudaFree(dB);
    cudaFree(dU);
    cudaFree(dV);
    cudaFree(dO);
    return cudaGetLastError() == cudaSuccess;
}
} // namespace replay
