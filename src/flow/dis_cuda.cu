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

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
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

// Device or page-locked host memory that is kept and only grows.
struct Buffer
{
    void* ptr = nullptr;
    std::size_t cap = 0;
    bool host = false;

    Buffer() = default;
    Buffer(Buffer const&) = delete;
    Buffer& operator=(Buffer const&) = delete;

    ~Buffer()
    {
        release();
    }

    void* ensure(std::size_t bytes, bool pinnedHost)
    {
        if (ptr != nullptr && bytes <= cap && host == pinnedHost)
        {
            return ptr;
        }
        release();
        if ((pinnedHost ? cudaMallocHost(&ptr, bytes) : cudaMalloc(&ptr, bytes)) != cudaSuccess)
        {
            ptr = nullptr;
            return nullptr;
        }
        cap = bytes;
        host = pinnedHost;
        return ptr;
    }

    void release()
    {
        if (ptr != nullptr)
        {
            host ? cudaFreeHost(ptr) : cudaFree(ptr);
        }
        ptr = nullptr;
        cap = 0;
    }
};

void releasePlanes(DevPlanes& planes)
{
    cudaFree(planes.y);
    cudaFree(planes.cb);
    cudaFree(planes.cr);
    planes = {};
}

// One per thread (recorder input, playout channel): its own nvJPEG states, stream and
// buffers, so cameras and channels run in parallel instead of behind one mutex.
struct Nv
{
    nvjpegHandle_t handle{};
    nvjpegJpegState_t decoder{};
    nvjpegEncoderState_t encoder{};
    nvjpegEncoderParams_t params{};
    cudaStream_t stream{};
    bool ready = false;
    DevPlanes slots[3];
    // Which source frame slots[0] and slots[1] hold: consecutive output frames reuse
    // the same pair (slow motion) or one of it, so it is not decoded again.
    std::string slotKey[2];
    // The picture this thread last unpacked, decoded or rendered (one of slots): the preview mosaic
    // draws its tile from it.
    DevPlanes const* last = nullptr;
    Buffer packed;
    Buffer pinned;
    std::map<std::string, DevFlow> flows;

    Nv() = default;
    Nv(Nv const&) = delete;
    Nv& operator=(Nv const&) = delete;

    ~Nv()
    {
        if (!ready)
        {
            return;
        }
        cudaStreamSynchronize(stream);
        for (auto& entry : flows)
        {
            cudaFree(entry.second.u);
            cudaFree(entry.second.v);
        }
        for (auto& slot : slots)
        {
            releasePlanes(slot);
        }
        nvjpegEncoderParamsDestroy(params);
        nvjpegEncoderStateDestroy(encoder);
        nvjpegJpegStateDestroy(decoder);
        nvjpegDestroy(handle);
        cudaStreamDestroy(stream);
    }
};

Nv& nv()
{
    thread_local Nv state;
    return state;
}

// Stream-ordered allocations from the device's caching pool. cudaFree waits for the
// whole device, so per-frame cudaMalloc/cudaFree serialised every channel and camera.
template <typename T>
bool devAlloc(T*& ptr, std::size_t bytes)
{
    void* raw = nullptr;
    if (cudaMallocAsync(&raw, bytes, nv().stream) != cudaSuccess)
    {
        ptr = nullptr;
        return false;
    }
    ptr = static_cast<T*>(raw);
    return true;
}

void devFree(void* ptr)
{
    if (ptr != nullptr)
    {
        cudaFreeAsync(ptr, nv().stream);
    }
}

// MXL grain memory this thread page-locked: the upload is then a direct DMA. Each
// grain is its own mapping that lives as long as the reader, so it is locked once.
struct HostMemory
{
    std::map<void const*, std::size_t> locked;
    std::map<void const*, bool> other;

    HostMemory() = default;
    HostMemory(HostMemory const&) = delete;
    HostMemory& operator=(HostMemory const&) = delete;

    ~HostMemory()
    {
        release();
    }

    // True when `ptr` is locked (by this thread or another); false when it cannot be.
    bool ensure(void const* ptr, std::size_t bytes)
    {
        auto const it = locked.find(ptr);
        if (it != locked.end() && it->second >= bytes)
        {
            return true;
        }
        auto const seen = other.find(ptr);
        if (seen != other.end())
        {
            return seen->second;
        }
        if (it != locked.end())
        {
            cudaHostUnregister(const_cast<void*>(ptr));
            locked.erase(it);
        }
        static int const readOnly = [] {
            int value = 0;
            if (cudaDeviceGetAttribute(&value, cudaDevAttrHostRegisterReadOnlySupported, 0) != cudaSuccess)
            {
                cudaGetLastError();
                return 0;
            }
            return value;
        }();
        unsigned const flags = cudaHostRegisterPortable | (readOnly != 0 ? cudaHostRegisterReadOnly : 0u);
        cudaError_t const err = cudaHostRegister(const_cast<void*>(ptr), bytes, flags);
        if (err == cudaSuccess)
        {
            locked.emplace(ptr, bytes);
            return true;
        }
        cudaGetLastError();
        other.emplace(ptr, err == cudaErrorHostMemoryAlreadyRegistered);
        return err == cudaErrorHostMemoryAlreadyRegistered;
    }

    void release()
    {
        for (auto const& entry : locked)
        {
            cudaHostUnregister(const_cast<void*>(entry.first));
        }
        locked.clear();
        other.clear();
    }
};

HostMemory& hostMemory()
{
    thread_local HostMemory value;
    return value;
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

// The device flags apply only when set before the primary context exists: every first CUDA use of a
// thread goes through here.
bool primeDevice()
{
    if (cudaFlowDeviceCount() <= 0)
    {
        return false;
    }
    static bool const poolKept = [] {
        // Sleep in a blocking sync while the GPU works. CUDA's default spins while there are
        // spare cores: every channel thread's cudaStreamSynchronize held a core (4 channels
        // took 6.5 of 7.4 cores inside libcuda). Set before the context exists.
        cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
        // Keep freed stream-ordered memory in the pool instead of returning it each sync.
        cudaMemPool_t pool{};
        std::uint64_t keep = UINT64_MAX;
        return cudaDeviceGetDefaultMemPool(&pool, 0) == cudaSuccess && cudaMemPoolSetAttribute(pool, cudaMemPoolAttrReleaseThreshold, &keep) == cudaSuccess;
    }();
    (void)poolKept;
    return true;
}

bool prepareNv()
{
    auto& state = nv();
    if (state.ready)
    {
        return true;
    }
    if (!primeDevice())
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
    if (cudaStreamCreateWithFlags(&state.stream, cudaStreamNonBlocking) != cudaSuccess)
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
    if (!devAlloc(gray, static_cast<std::size_t>(planes.width) * static_cast<std::size_t>(planes.height) * sizeof(float)))
    {
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
        devAlloc(pyrA[static_cast<std::size_t>(level)], static_cast<std::size_t>(dw * dh) * sizeof(float));
        devAlloc(pyrB[static_cast<std::size_t>(level)], static_cast<std::size_t>(dw * dh) * sizeof(float));
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
        devAlloc(denseU, static_cast<std::size_t>(lw * lh) * sizeof(float));
        devAlloc(denseV, static_cast<std::size_t>(lw * lh) * sizeof(float));
        devAlloc(weight, static_cast<std::size_t>(lw * lh) * sizeof(float));
        cudaMemsetAsync(denseU, 0, static_cast<std::size_t>(lw * lh) * sizeof(float), nv().stream);
        cudaMemsetAsync(denseV, 0, static_cast<std::size_t>(lw * lh) * sizeof(float), nv().stream);
        cudaMemsetAsync(weight, 0, static_cast<std::size_t>(lw * lh) * sizeof(float), nv().stream);
        int const patchesX = 1 + static_cast<int>(std::ceil(static_cast<float>(lw) / std::max(spacing, 1.f)));
        int const patchesY = 1 + static_cast<int>(std::ceil(static_cast<float>(lh) / std::max(spacing, 1.f)));
        motionDensifyKernel<<<grid2(patchesX, patchesY), block2(), 0, nv().stream>>>(pyrA[static_cast<std::size_t>(index)], pyrB[static_cast<std::size_t>(index)], lw, lh, prevU,
            prevV, prevW, prevH, op.patchSize, op.searchIterations, patchesX, patchesY, denseU, denseV, weight);
        normalizeFlowKernel<<<(lw * lh + 255) / 256, 256, 0, nv().stream>>>(denseU, denseV, weight, lw * lh);
        devFree(weight);
        if (op.variational)
        {
            int const sweeps = std::max(1, (level + 1) * 5);
            for (int sweep = 0; sweep < sweeps; ++sweep)
            {
                refineKernel<<<grid2(lw, lh), block2(), 0, nv().stream>>>(pyrA[static_cast<std::size_t>(index)], pyrB[static_cast<std::size_t>(index)], denseU, denseV, lw, lh, sweep);
            }
        }
        devFree(prevU);
        devFree(prevV);
        prevU = denseU;
        prevV = denseV;
        prevW = lw;
        prevH = lh;
    }
    for (int level = 1; level <= levels; ++level)
    {
        devFree(pyrA[static_cast<std::size_t>(level)]);
        devFree(pyrB[static_cast<std::size_t>(level)]);
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

// v210 kernels: one thread per 6-pixel group (launch with v210Grid). One thread per
// row, as before, kept the GPU nearly idle and took milliseconds per frame.
dim3 v210Grid(int width, int height)
{
    int const groups = (width + 5) / 6;
    return dim3((groups + 127) / 128, height);
}

__global__ void packV210Kernel(std::uint8_t const* y, std::uint8_t const* cb, std::uint8_t const* cr, int pitchY, int pitchC, int width, int height,
    std::uint8_t* dst, int rowBytes)
{
    int const group = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y;
    int const cw = width / 2;
    int const groups = (width + 5) / 6;
    if (row >= height || group >= groups)
    {
        return;
    }
    std::uint8_t* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes);
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
    int const group = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y;
    int const cw = width / 2;
    int const groups = (width + 5) / 6;
    if (row >= height || group >= groups)
    {
        return;
    }
    std::uint8_t const* line = src + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes);
    int x = group * 6;
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

// Mean of up to 8×8 samples spread over the source rectangle [x0, x1) × [y0, y1).
__device__ float boxMean(std::uint8_t const* plane, int pitch, int x0, int x1, int y0, int y1)
{
    int const stepX = max(1, (x1 - x0 + 7) / 8);
    int const stepY = max(1, (y1 - y0 + 7) / 8);
    float sum = 0.f;
    int count = 0;
    for (int y = y0; y < y1; y += stepY)
    {
        for (int x = x0; x < x1; x += stepX)
        {
            sum += plane[y * pitch + x];
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : 0.f;
}

// One preview mosaic tile from 8-bit 4:2:2 planes: one thread per 2×2 output pixels (one NV12 CbCr pair).
__global__ void mosaicTileKernel(std::uint8_t const* y, std::uint8_t const* cb, std::uint8_t const* cr, int pitchY, int pitchC, int srcW, int srcH,
    std::uint8_t* canvas, int pitch, int canvasH, int tx, int ty, int tw, int th)
{
    int const bx = blockIdx.x * blockDim.x + threadIdx.x;
    int const by = blockIdx.y * blockDim.y + threadIdx.y;
    if (2 * bx >= tw || 2 * by >= th)
    {
        return;
    }
    for (int dy = 0; dy < 2; ++dy)
    {
        int const py = 2 * by + dy;
        int const y0 = py * srcH / th;
        int const y1 = max(y0 + 1, (py + 1) * srcH / th);
        for (int dx = 0; dx < 2; ++dx)
        {
            int const px = 2 * bx + dx;
            int const x0 = px * srcW / tw;
            int const x1 = max(x0 + 1, (px + 1) * srcW / tw);
            canvas[(ty + py) * pitch + tx + px] = static_cast<std::uint8_t>(boxMean(y, pitchY, x0, x1, y0, y1) + 0.5f);
        }
    }
    int const cx0 = 2 * bx * srcW / tw / 2;
    int const cx1 = max(cx0 + 1, (2 * bx + 2) * srcW / tw / 2);
    int const cy0 = 2 * by * srcH / th;
    int const cy1 = max(cy0 + 1, (2 * by + 2) * srcH / th);
    std::uint8_t* pair = canvas + (canvasH + ty / 2 + by) * pitch + tx + 2 * bx;
    pair[0] = static_cast<std::uint8_t>(boxMean(cb, pitchC, cx0, cx1, cy0, cy1) + 0.5f);
    pair[1] = static_cast<std::uint8_t>(boxMean(cr, pitchC, cx0, cx1, cy0, cy1) + 0.5f);
}

// The stream of this thread's canvas copies (the encoder thread has no nvJPEG state).
cudaStream_t copyStream()
{
    struct Holder
    {
        cudaStream_t stream{};
        ~Holder()
        {
            if (stream != nullptr)
            {
                cudaStreamDestroy(stream);
            }
        }
    };
    thread_local Holder holder;
    if (holder.stream == nullptr && cudaStreamCreateWithFlags(&holder.stream, cudaStreamNonBlocking) != cudaSuccess)
    {
        holder.stream = nullptr;
    }
    return holder.stream;
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
    auto& state = nv();
    auto* device = static_cast<std::uint8_t*>(state.packed.ensure(bytes, false));
    auto* pinned = static_cast<std::uint8_t*>(state.pinned.ensure(bytes, true));
    if (device == nullptr || pinned == nullptr)
    {
        return {};
    }
    packV210Kernel<<<v210Grid(planes.width, planes.height), 128, 0, state.stream>>>(planes.y, planes.cb, planes.cr, planes.pitchY, planes.pitchC, planes.width,
        planes.height, device, rowBytes);
    // Page-locked target: a full-speed DMA instead of the driver's pageable staging.
    cudaMemcpyAsync(pinned, device, bytes, cudaMemcpyDeviceToHost, state.stream);
    if (cudaStreamSynchronize(state.stream) != cudaSuccess || cudaGetLastError() != cudaSuccess)
    {
        return {};
    }
    return std::vector<std::uint8_t>(pinned, pinned + bytes);
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
        devFree(oldest->second.u);
        devFree(oldest->second.v);
        state.flows.erase(oldest);
    }
    float* grayA = nullptr;
    float* grayB = nullptr;
    if (!launchGray(a, grayA) || !launchGray(b, grayB))
    {
        devFree(grayA);
        devFree(grayB);
        return {};
    }
    DevFlow flow = computeFlowDevice(grayA, grayB, a.width, a.height, op);
    devFree(grayA);
    devFree(grayB);
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

bool gpuRenderFromJpeg(std::uint8_t const* jpegA, std::size_t sizeA, std::string const& keyA, std::uint8_t const* jpegB, std::size_t sizeB, std::string const& keyB,
    float phase, bool interpolate, OperatingPoint const& op, std::string const& flowKey, int width, int height, GpuPicture& out)
{
    if (jpegA == nullptr || sizeA == 0 || width < 2 || height < 1)
    {
        return false;
    }
    auto& state = nv();
    if (!prepareNv())
    {
        return false;
    }
    auto holding = [&](std::string const& key) {
        for (int i = 0; i < 2; ++i)
        {
            if (!key.empty() && state.slotKey[i] == key)
            {
                return i;
            }
        }
        return -1;
    };
    // Decode into slot `index` unless it already holds `key`.
    auto load = [&](int index, std::uint8_t const* jpeg, std::size_t size, std::string const& key) {
        if (!key.empty() && state.slotKey[index] == key)
        {
            return true;
        }
        state.slotKey[index].clear();
        if (!decodeToSlot(jpeg, size, state.slots[index]) || state.slots[index].width != width || state.slots[index].height != height)
        {
            return false;
        }
        state.slotKey[index] = key;
        return true;
    };
    bool const needB = jpegB != nullptr && sizeB > 0 && phase > 0.001f && phase < 0.999f;
    int a = holding(keyA);
    if (a < 0)
    {
        a = needB && holding(keyB) == 0 ? 1 : 0;
    }
    if (!load(a, jpegA, sizeA, keyA))
    {
        return false;
    }
    int const b = 1 - a;
    if (needB && !load(b, jpegB, sizeB, keyB))
    {
        return false;
    }
    auto const& planesA = state.slots[a];
    auto const& planesB = state.slots[b];
    DevPlanes* source = &state.slots[a];
    if (needB)
    {
        if (!ensureSlot(state.slots[2], planesA.width, planesA.height))
        {
            return false;
        }
        DevFlow flow;
        if (interpolate)
        {
            flow = cachedFlow(flowKey, planesA, planesB, op);
        }
        blendKernel<<<grid2(planesA.width, planesA.height), block2(), 0, nv().stream>>>(planesA.y, planesA.cb, planesA.cr, planesB.y, planesB.cb, planesB.cr,
            planesA.pitchY, planesA.pitchC, planesB.pitchY, planesB.pitchC, planesA.width, planesA.height, flow.u, flow.v, flow.width, flow.height, phase,
            state.slots[2].y, state.slots[2].cb, state.slots[2].cr, state.slots[2].pitchY, state.slots[2].pitchC);
        source = &state.slots[2];
    }
    if (cudaStreamSynchronize(state.stream) != cudaSuccess || cudaGetLastError() != cudaSuccess)
    {
        return false;
    }
    out.v210 = downloadV210(*source);
    if (out.v210.empty())
    {
        return false;
    }
    state.last = source;
    return true;
}

std::vector<std::uint8_t> gpuEncodeV210(std::uint8_t const* packed, int width, int height, int rowBytes, int quality)
{
    if (packed == nullptr || width < 2 || height < 1)
    {
        return {};
    }
    auto& state = nv();
    state.slotKey[0].clear();
    if (!prepareNv() || !ensureSlot(state.slots[0], width, height))
    {
        return {};
    }
    if (rowBytes <= 0)
    {
        rowBytes = static_cast<int>(v210RowBytes(width));
    }
    std::size_t const bytes = static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height);
    auto* device = static_cast<std::uint8_t*>(state.packed.ensure(bytes, false));
    if (device == nullptr)
    {
        return {};
    }
    // The MXL grain is page-locked on first use, so the copy is a direct DMA; when it
    // cannot be locked the driver stages the pageable copy as before.
    hostMemory().ensure(packed, bytes);
    cudaMemcpyAsync(device, packed, bytes, cudaMemcpyHostToDevice, state.stream);
    unpackV210Kernel<<<v210Grid(width, height), 128, 0, state.stream>>>(device, rowBytes, width, height, state.slots[0].y, state.slots[0].cb, state.slots[0].cr,
        state.slots[0].pitchY, state.slots[0].pitchC);
    state.last = &state.slots[0];
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
    state.slotKey[0].clear();
    if (!prepareNv() || !ensureSlot(state.slots[0], frame.width, frame.height))
    {
        return {};
    }
    std::uint16_t* y = nullptr;
    std::uint16_t* cb = nullptr;
    std::uint16_t* cr = nullptr;
    std::size_t const yBytes = frame.y.size() * sizeof(std::uint16_t);
    std::size_t const cBytes = frame.cb.size() * sizeof(std::uint16_t);
    if (!devAlloc(y, yBytes) || !devAlloc(cb, cBytes) || !devAlloc(cr, cBytes))
    {
        devFree(y);
        devFree(cb);
        devFree(cr);
        return {};
    }
    cudaMemcpyAsync(y, frame.y.data(), yBytes, cudaMemcpyHostToDevice, state.stream);
    cudaMemcpyAsync(cb, frame.cb.data(), cBytes, cudaMemcpyHostToDevice, state.stream);
    cudaMemcpyAsync(cr, frame.cr.data(), cBytes, cudaMemcpyHostToDevice, state.stream);
    planes10Kernel<<<grid2(frame.width, frame.height), block2(), 0, nv().stream>>>(y, cb, cr, frame.width, frame.height, state.slots[0].y, state.slots[0].cb, state.slots[0].cr,
        state.slots[0].pitchY, state.slots[0].pitchC);
    devFree(y);
    devFree(cb);
    devFree(cr);
    state.last = &state.slots[0];
    std::vector<std::uint8_t> jpeg;
    if (!encodePlanes(state.slots[0], quality, jpeg))
    {
        return {};
    }
    return jpeg;
}

void gpuReleaseHostMemory()
{
    // No queued copy may still read the memory when it is unlocked.
    auto& state = nv();
    if (state.ready)
    {
        cudaStreamSynchronize(state.stream);
    }
    hostMemory().release();
}

std::uint8_t* gpuCanvasCreate(int width, int height, int& pitch)
{
    if (width < 2 || height < 2 || !primeDevice())
    {
        return nullptr;
    }
    void* canvas = nullptr;
    std::size_t bytes = 0;
    if (cudaMallocPitch(&canvas, &bytes, static_cast<std::size_t>(width), static_cast<std::size_t>(height + height / 2)) != cudaSuccess)
    {
        cudaGetLastError();
        return nullptr;
    }
    auto* rows = static_cast<std::uint8_t*>(canvas);
    auto const stream = copyStream();
    bool const black = stream != nullptr &&
                       cudaMemset2DAsync(rows, bytes, 16, static_cast<std::size_t>(width), static_cast<std::size_t>(height), stream) == cudaSuccess &&
                       cudaMemset2DAsync(rows + bytes * static_cast<std::size_t>(height), bytes, 128, static_cast<std::size_t>(width),
                           static_cast<std::size_t>(height / 2), stream) == cudaSuccess &&
                       cudaStreamSynchronize(stream) == cudaSuccess;
    if (!black)
    {
        cudaFree(canvas);
        return nullptr;
    }
    pitch = static_cast<int>(bytes);
    return rows;
}

void gpuCanvasDestroy(std::uint8_t* canvas)
{
    if (canvas != nullptr)
    {
        cudaFree(canvas);
    }
}

bool gpuCanvasDrawLast(std::uint8_t* canvas, int pitch, int height, int x, int y, int w, int h)
{
    auto& state = nv();
    if (canvas == nullptr || !state.ready || state.last == nullptr || w < 2 || h < 2)
    {
        return false;
    }
    auto const& planes = *state.last;
    dim3 const block(16, 8);
    dim3 const grid((static_cast<unsigned>(w / 2) + 15) / 16, (static_cast<unsigned>(h / 2) + 7) / 8);
    // Queued behind the thread's own work on its stream, which also orders the next use of the planes after it.
    mosaicTileKernel<<<grid, block, 0, state.stream>>>(planes.y, planes.cb, planes.cr, planes.pitchY, planes.pitchC, planes.width, planes.height, canvas,
        pitch, height, x, y, w, h);
    return cudaGetLastError() == cudaSuccess;
}

bool gpuCopyRows(void* dst, std::size_t dstPitch, void const* src, std::size_t srcPitch, std::size_t bytes, std::size_t rows)
{
    auto const stream = copyStream();
    if (stream == nullptr || dst == nullptr || src == nullptr)
    {
        return false;
    }
    if (cudaMemcpy2DAsync(dst, dstPitch, src, srcPitch, bytes, rows, cudaMemcpyDefault, stream) != cudaSuccess || cudaStreamSynchronize(stream) != cudaSuccess)
    {
        cudaGetLastError();
        return false;
    }
    return true;
}

void* gpuContext()
{
    if (!primeDevice())
    {
        return nullptr;
    }
    // Any runtime call makes the primary context current on this thread.
    cudaFree(nullptr);
    void* entry = nullptr;
    cudaDriverEntryPointQueryResult found{};
    if (cudaGetDriverEntryPointByVersion("cuCtxGetCurrent", &entry, 12000, cudaEnableDefault, &found) != cudaSuccess || entry == nullptr)
    {
        cudaGetLastError();
        return nullptr;
    }
    CUcontext context = nullptr;
    return reinterpret_cast<CUresult (*)(CUcontext*)>(entry)(&context) == CUDA_SUCCESS ? context : nullptr;
}
} // namespace replay
