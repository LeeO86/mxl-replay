#include "playout/scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace replay
{
void Scheduler::setTargetSpeed(double next, int ramp)
{
    next = std::clamp(next, -1.0, 2.0);
    speedFrom = speed;
    targetSpeed = next;
    rampFrames = std::max(0, ramp);
    rampLeft = rampFrames;
    if (rampFrames == 0)
    {
        speed = targetSpeed;
    }
}

double Scheduler::step()
{
    if (!playing)
    {
        return speed;
    }
    double applied = targetSpeed;
    if (rampLeft > 0 && rampFrames > 0)
    {
        applied = speedFrom + (targetSpeed - speedFrom) * static_cast<double>(rampFrames - rampLeft + 1) / static_cast<double>(rampFrames);
        --rampLeft;
    }
    speed = applied;
    auto const delta = static_cast<std::int64_t>(std::llround(applied * static_cast<double>(framePeriodNs)));
    positionNs += delta;
    return applied;
}

void Scheduler::scrubFrames(int frames)
{
    positionNs += static_cast<std::int64_t>(frames) * framePeriodNs;
}

void Scheduler::scrubSeconds(double seconds)
{
    positionNs += static_cast<std::int64_t>(std::llround(seconds * 1e9));
}

SourcePick pickSource(std::int64_t positionNs, std::int64_t originNs, std::int64_t housePeriodNs, double hfrFactor, double hfrSnap, MotionMode requested)
{
    SourcePick pick;
    if (housePeriodNs <= 0)
    {
        return pick;
    }
    if (hfrFactor < 1.0)
    {
        hfrFactor = 1.0;
    }
    double const sourcePeriod = static_cast<double>(housePeriodNs) / hfrFactor;
    double const delta = static_cast<double>(positionNs - originNs);
    double const k = delta / sourcePeriod;
    double const nearest = std::round(k);
    double const dist = std::fabs(k - nearest);
    if (dist <= hfrSnap)
    {
        pick.frameA = static_cast<std::int64_t>(nearest);
        pick.frameB = pick.frameA;
        pick.phase = 0;
        pick.snapped = true;
        pick.kind = (hfrFactor <= 1.0001 && dist < 1e-6) ? SourcePick::Kind::Exact : SourcePick::Kind::Repeat;
        return pick;
    }
    double const lower = std::floor(k);
    pick.frameA = static_cast<std::int64_t>(lower);
    pick.frameB = pick.frameA + 1;
    pick.phase = k - lower;
    pick.snapped = false;
    if (requested == MotionMode::Repeat)
    {
        if (pick.phase >= 0.5)
        {
            pick.frameA = pick.frameB;
        }
        pick.frameB = pick.frameA;
        pick.phase = 0;
        pick.kind = SourcePick::Kind::Repeat;
    }
    else if (requested == MotionMode::Blend)
    {
        pick.kind = SourcePick::Kind::Blend;
    }
    else
    {
        pick.kind = SourcePick::Kind::Interpolate;
    }
    return pick;
}

char const* motionName(MotionMode mode)
{
    switch (mode)
    {
    case MotionMode::Repeat:
        return "repeat";
    case MotionMode::Blend:
        return "blend";
    case MotionMode::Interpolate:
        return "interpolate";
    }
    return "interpolate";
}

std::uint64_t PlayoutGrid::take(std::uint64_t current, bool& resynced)
{
    resynced = next != 0 && (next + 2 < current || next > current + 1);
    if (resynced)
    {
        lastJump = static_cast<std::int64_t>(current) - static_cast<std::int64_t>(next);
        ++resyncs;
        ++unlogged;
    }
    if (next == 0 || resynced)
    {
        next = current;
    }
    return next++;
}

std::uint64_t PlayoutGrid::toLog(std::uint64_t nowNs)
{
    constexpr std::uint64_t kInterval = 10'000'000'000ull;
    if (unlogged == 0 || (loggedNs != 0 && nowNs < loggedNs + kInterval))
    {
        return 0;
    }
    loggedNs = nowNs;
    auto const count = unlogged;
    unlogged = 0;
    return count;
}

MotionMode parseMotion(std::string const& text, bool* ok)
{
    if (ok != nullptr)
    {
        *ok = true;
    }
    if (text == "repeat")
    {
        return MotionMode::Repeat;
    }
    if (text == "blend")
    {
        return MotionMode::Blend;
    }
    if (text == "interpolate")
    {
        return MotionMode::Interpolate;
    }
    if (ok != nullptr)
    {
        *ok = false;
    }
    return MotionMode::Interpolate;
}
} // namespace replay
