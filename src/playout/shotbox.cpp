#include "playout/shotbox.hpp"

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
} // namespace replay
