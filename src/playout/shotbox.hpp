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

// The transition into a playlist entry from the one before (SPECIFICATION.md §7, 1.6.0).
enum class Transition
{
    Cut,
    Mix
};

[[nodiscard]] char const* transitionName(Transition transition);
[[nodiscard]] Transition parseTransition(std::string const& text, bool* ok = nullptr);

// A mix and an audio crossfade are 1–250 frames (10 s at 25p).
constexpr int kMaxTransitionFrames = 250;
// A mix without a length.
constexpr int kDefaultMixFrames = 25;
// The audio crossfade of a cut without `audioFrames`: no click at the join.
constexpr int kDefaultCutAudioFrames = 2;

struct PlaylistEntry
{
    std::string clipId;
    double speed = 1;
    EndAction end = EndAction::Next;
    bool autoAdvance = true;
    // Into this entry: `cut` or a `mix` of `transitionFrames` output frames, and an audio crossfade of
    // `audioFrames` (-1: the mix's length, 2 on a cut; 0: a hard audio cut).
    Transition transition = Transition::Cut;
    int transitionFrames = 0;
    int audioFrames = -1;
    // The entry's own (clip) audio plays; off: only the playlist's aux track.
    bool ownAudio = true;
};

// A playlist's audio aux track (SPECIFICATION.md §5.4, 1.6.0): a clip's audio or an audio item, under the
// entries, ducked while an entry's own audio plays.
struct AuxTrack
{
    // A clip id or an audio item id ("audio-N"); empty: none.
    std::string source;
    double levelDb = 0;
    bool loop = true;
    // Linear fade to silence over the playlist's last frames (0: none).
    int fadeOutFrames = 50;
    // The duck depth (0: none) and its ramps.
    double duckDb = -12;
    int attackMs = 200;
    int releaseMs = 500;
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

// Shotbox countdowns (SPECIFICATION.md §8.1). The playing time from `positionNs` to `outNs` at `speed`: 0 at
// or after OUT, -1 when the speed never reaches it (0 or backwards).
[[nodiscard]] std::int64_t remainingNs(std::int64_t positionNs, std::uint64_t outNs, double speed);

// The playing time to the playlist's end from entry `index`, which has `clipRemainingNs` left (-1: never ends).
// `lengthsNs[i]` is entry i's playing time at its speed, -1 for a deleted clip (skipped). The playlist ends after
// the first entry whose end action is not `next` (freeze, black, return to live), else after the last entry (a
// looped playlist: the end of this pass); the time an entry waits cued (no auto-advance) is not counted. -1 when it
// never ends (a `loop` entry). With `framePeriodNs`, a transition into an auto-advanced entry takes its overlap off
// (1.6.0).
[[nodiscard]] std::int64_t playlistRemainingNs(std::vector<PlaylistEntry> const& entries, std::vector<std::int64_t> const& lengthsNs, int index,
    std::int64_t clipRemainingNs, std::int64_t framePeriodNs = 0);

// The next entry an idle playlist plays after `index` (SPECIFICATION.md §7): the next one whose clip exists
// (`present[i]`), from the first again after the last. -1 when no clip exists.
[[nodiscard]] int idleNext(std::vector<bool> const& present, int index);
} // namespace replay
