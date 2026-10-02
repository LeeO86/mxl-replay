#pragma once

#include "flow/dis.hpp"

namespace replay
{
// True when a CUDA device is visible to this process.
[[nodiscard]] int cudaFlowDeviceCount();
[[nodiscard]] bool cudaFlowAvailable();
[[nodiscard]] bool ofaProbe();

// GPU optical-flow interpolation. Returns false when the device path is not
// usable; the caller then runs the CPU port in dis.cpp.
bool cudaInterpolate(Yuv422 const& from, Yuv422 const& to, FlowField const& forward, FlowField const& backward, float phase, OperatingPoint const& op,
    Yuv422& out);

[[nodiscard]] bool ofaAvailable();
[[nodiscard]] char const* activeFlowModuleName();
} // namespace replay
