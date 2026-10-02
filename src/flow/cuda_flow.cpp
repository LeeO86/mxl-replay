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

#if !defined(REPLAY_WITH_CUDA)
bool cudaInterpolate(Yuv422 const&, Yuv422 const&, FlowField const&, FlowField const&, float, OperatingPoint const&, Yuv422&)
{
    return false;
}

int cudaFlowDeviceCount()
{
    return 0;
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
