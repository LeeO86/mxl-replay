#pragma once

#include <cstdint>
#include <string>

namespace replay
{
enum class MotionMode
{
    Repeat,
    Blend,
    Interpolate
};

struct SourcePick
{
    // Exact means the wanted time lands on a real source frame (100% on a house-rate camera).
    enum class Kind
    {
        Exact,
        Repeat,
        Blend,
        Interpolate
    };
    Kind kind = Kind::Exact;
    std::int64_t frameA = 0;
    std::int64_t frameB = 0;
    double phase = 0;
    bool snapped = false;
};

struct Scheduler
{
    double speed = 1.0;
    double targetSpeed = 1.0;
    double speedFrom = 1.0;
    int rampFrames = 3;
    int rampLeft = 0;
    std::int64_t positionNs = 0;
    std::int64_t framePeriodNs = 20000000;
    bool playing = false;
    int camera = 1;

    void setTargetSpeed(double speed, int ramp);
    // One output grain. Returns the instantaneous speed applied to this grain.
    double step();
    void scrubFrames(int frames);
    void scrubSeconds(double seconds);
};

// hfrFactor is flow_rate / house_rate (1 for a house-rate camera).
// hfrSnap is in source frames (default 0.1).
[[nodiscard]] SourcePick pickSource(std::int64_t positionNs, std::int64_t originNs, std::int64_t housePeriodNs, double hfrFactor, double hfrSnap,
    MotionMode requested);

[[nodiscard]] char const* motionName(MotionMode mode);
[[nodiscard]] MotionMode parseMotion(std::string const& text, bool* ok = nullptr);
} // namespace replay
