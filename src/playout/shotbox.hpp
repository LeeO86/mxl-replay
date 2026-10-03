#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace replay
{
enum class EndAction
{
    Freeze,
    Black,
    Loop,
    Next,
    ReturnToLive
};

enum class ShotState
{
    Idle,
    Cued,
    Playing,
    Paused,
    Ended
};

[[nodiscard]] char const* endActionName(EndAction action);
[[nodiscard]] EndAction parseEndAction(std::string const& text, bool* ok = nullptr);
[[nodiscard]] char const* shotStateName(ShotState state);

struct ClipRef
{
    std::string id;
    std::string name;
    int camera = 1;
    std::uint64_t inNs = 0;
    std::uint64_t outNs = 0;
    double speed = 1;
    std::string motion = "interpolate";
    std::string audio = "mute";
    std::string colour = "#f5c518";
    std::string tags;
    EndAction end = EndAction::Freeze;
    std::string groupId;
    bool library = false;
};

struct PlaylistEntry
{
    std::string clipId;
    double speed = 1;
    EndAction end = EndAction::Next;
    bool autoAdvance = true;
};

struct Shotbox
{
    ShotState state = ShotState::Idle;
    bool playOnFirstClick = false;
    std::string clipId;
    EndAction end = EndAction::Freeze;

    // Returns the state after the click.
    ShotState click();
    ShotState finished();
};

// Advance a playlist. `index` is the entry that just ended.
// Returns the next index, or -1 when the playlist is finished.
[[nodiscard]] int playlistNext(std::vector<PlaylistEntry> const& entries, int index);
} // namespace replay
