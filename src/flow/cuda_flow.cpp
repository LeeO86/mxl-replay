#include "flow/cuda_flow.hpp"

namespace replay
{
bool cudaFlowAvailable()
{
#if defined(REPLAY_WITH_CUDA)
    return cudaFlowDeviceCount() > 0;
#else
    return false;
#endif
}

char const* jpegRuntimeBackend()
{
#if defined(REPLAY_WITH_NVJPEG)
    if (cudaFlowAvailable())
    {
        return "nvjpeg";
    }
#endif
    return "libjpeg-turbo";
}

#if !defined(REPLAY_WITH_CUDA)
int cudaFlowDeviceCount()
{
    return 0;
}

bool gpuRenderFromJpeg(std::uint8_t const*, std::size_t, std::string const&, std::uint8_t const*, std::size_t, std::string const&, float, bool, OperatingPoint const&,
    std::string const&, int, int, GpuPicture&)
{
    return false;
}

std::vector<std::uint8_t> gpuEncodeV210(std::uint8_t const*, int, int, int, int)
{
    return {};
}

std::vector<std::uint8_t> gpuEncodeFrame10(Frame10 const&, int)
{
    return {};
}

void gpuReleaseHostMemory()
{
}

std::uint8_t* gpuCanvasCreate(int, int, int&)
{
    return nullptr;
}

void gpuCanvasDestroy(std::uint8_t*)
{
}

bool gpuCanvasDrawLast(std::uint8_t*, int, int, int, int, int, int)
{
    return false;
}

bool gpuCopyRows(void*, std::size_t, void const*, std::size_t, std::size_t, std::size_t)
{
    return false;
}

void* gpuContext()
{
    return nullptr;
}
#endif

bool ofaAvailable()
{
#if defined(REPLAY_WITH_VULKAN)
    return ofaProbe();
#else
    return false;
#endif
}

char const* activeFlowModuleName()
{
    if (ofaAvailable())
    {
        return "ofa";
    }
    if (cudaFlowAvailable())
    {
        return "dis-cuda";
    }
    return "dis-cpu";
}
} // namespace replay
