#include "playout/shotbox.hpp"

#include "playout/transition.hpp"

#include <algorithm>

namespace replay
{
char const* endActionName(EndAction action)
{
    switch (action)
    {
    case EndAction::Freeze:
        return "freeze";
    case EndAction::Black:
        return "black";
    case EndAction::Loop:
        return "loop";
    case EndAction::Next:
        return "next";
    case EndAction::ReturnToLive:
        return "return-to-live";
    }
    return "freeze";
}

EndAction parseEndAction(std::string const& text, bool* ok)
{
    if (ok != nullptr)
    {
        *ok = true;
    }
    if (text == "freeze")
    {
        return EndAction::Freeze;
    }
    if (text == "black")
    {
        return EndAction::Black;
    }
    if (text == "loop")
    {
        return EndAction::Loop;
    }
    if (text == "next")
    {
        return EndAction::Next;
    }
    if (text == "return-to-live" || text == "live")
    {
        return EndAction::ReturnToLive;
    }
    if (ok != nullptr)
    {
        *ok = false;
    }
    return EndAction::Freeze;
}

char const* shotStateName(ShotState state)
{
    switch (state)
    {
    case ShotState::Idle:
        return "idle";
    case ShotState::Cued:
        return "cued";
    case ShotState::Playing:
        return "playing";
    case ShotState::Paused:
        return "paused";
    case ShotState::Ended:
        return "ended";
    }
    return "idle";
}

ShotState Shotbox::click()
{
    if (playOnFirstClick)
    {
        if (state == ShotState::Playing)
        {
            state = ShotState::Paused;
        }
        else
        {
            state = ShotState::Playing;
        }
        return state;
    }
    if (state == ShotState::Idle || state == ShotState::Ended)
    {
        state = ShotState::Cued;
    }
    else if (state == ShotState::Cued || state == ShotState::Paused)
    {
        state = ShotState::Playing;
    }
    else if (state == ShotState::Playing)
    {
        state = ShotState::Paused;
    }
    return state;
}

ShotState Shotbox::finished()
{
    if (end == EndAction::Loop)
    {
        state = ShotState::Playing;
        return state;
    }
    state = ShotState::Ended;
    return state;
}

int playlistNext(std::vector<PlaylistEntry> const& entries, int index)
{
    int const next = index + 1;
    if (next < 0 || next >= static_cast<int>(entries.size()))
    {
        return -1;
    }
    return next;
}

std::int64_t remainingNs(std::int64_t positionNs, std::uint64_t outNs, double speed)
{
    auto const out = static_cast<std::int64_t>(outNs);
    if (positionNs >= out)
    {
        return 0;
    }
    if (!(speed > 0.0))
    {
        return -1;
    }
    return static_cast<std::int64_t>(static_cast<double>(out - positionNs) / speed);
}

std::int64_t playlistRemainingNs(std::vector<PlaylistEntry> const& entries, std::vector<std::int64_t> const& lengthsNs, int index,
    std::int64_t clipRemainingNs, std::int64_t framePeriodNs)
{
    if (clipRemainingNs < 0 || index < 0 || index >= static_cast<int>(entries.size()))
    {
        return clipRemainingNs;
    }
    std::int64_t total = clipRemainingNs;
    for (int i = index; i < static_cast<int>(entries.size()); ++i)
    {
        auto const& entry = entries[static_cast<std::size_t>(i)];
        if (i > index)
        {
            auto const length = i < static_cast<int>(lengthsNs.size()) ? lengthsNs[static_cast<std::size_t>(i)] : -1;
            if (length < 0)
            {
                continue; // a deleted clip is skipped
            }
            total += length;
            // A transition into an auto-advanced entry overlaps the two entries (1.7.0).
            auto const& before = entries[static_cast<std::size_t>(i - 1)];
            if (framePeriodNs > 0 && before.autoAdvance)
            {
                total -= std::min<std::int64_t>(length, transitionPlan(entry).length() * framePeriodNs);
            }
        }
        if (entry.end == EndAction::Loop)
        {
            return -1;
        }
        if (entry.end != EndAction::Next)
        {
            return total;
        }
    }
    return total;
}

int idleNext(std::vector<bool> const& present, int index)
{
    int const size = static_cast<int>(present.size());
    for (int step = 1; step <= size; ++step)
    {
        int const i = ((index < 0 ? -1 : index) + step) % size;
        if (i >= 0 && present[static_cast<std::size_t>(i)])
        {
            return i;
        }
    }
    return -1;
}
} // namespace replay
