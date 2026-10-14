#pragma once

// Playlist transitions and the aux track (SPECIFICATION.md §5.4 and §7, 1.7.0): the weights of a video mix, the
// gains of the equal-power audio crossfade, the aux track's duck, end fade and edges, and the JSON of the
// entry and aux fields (the API, the config export and import).

#include "playout/shotbox.hpp"
#include "util/json.hpp"

#include <cstdint>
#include <string>

namespace replay
{
// The lengths of a transition into an entry, in output frames: the video mix (0: a cut), the audio crossfade
// (0: a hard audio cut) and the overlap of the two entries, the longer of both.
struct TransitionPlan
{
    int videoFrames = 0;
    int audioFrames = 0;
    [[nodiscard]] int length() const { return videoFrames > audioFrames ? videoFrames : audioFrames; }
};
[[nodiscard]] TransitionPlan transitionPlan(PlaylistEntry const& incoming);

// The weight of the incoming picture in grain `k` (0-based) of an `n`-grain mix: (k+1)/(n+1), never a pure
// picture inside the mix; 1 from grain n on.
[[nodiscard]] double mixWeight(int k, int n);
// The CPU blend's weight in 1/1024 steps (as blendInto) and one blended 10-bit sample: the reference of the
// mix (a·(1024−w) + b·w) >> 10.
[[nodiscard]] std::uint32_t blendWeight1024(double weight);
[[nodiscard]] std::uint16_t blendSample(std::uint16_t a, std::uint16_t b, std::uint32_t weight1024);

// The equal-power crossfade at sample `n` (0-based) of `total`: out = cos(π/2·t), in = sin(π/2·t) with
// t = (n + 0.5) / total; exactly 1/0 before and 0/1 after the crossfade.
struct CrossfadeGains
{
    float out = 1;
    float in = 0;
};
[[nodiscard]] CrossfadeGains crossfadeGains(std::int64_t n, std::int64_t total);

[[nodiscard]] double dbToGain(double db);

// The aux track's duck: the gain in dB moves one sample at a time toward `depthDb` while ducked and back to
// 0 dB otherwise, down over `attackSamples`, up over `releaseSamples` (for the whole depth). Returns the gain.
struct DuckRamp
{
    double db = 0;
    double next(bool ducked, double depthDb, double attackSamples, double releaseSamples);
};

// The aux track's fade to the playlist's end: linear from 1 at `fadeNs` before the end to 0 at the end;
// 1 when the playlist does not end (`remainingNs` < 0) or without a fade.
[[nodiscard]] double endFadeGain(std::int64_t remainingNs, std::int64_t fadeNs);
// 5 ms fades at the aux source's start and end (its loop point): no click when it starts, ends or wraps.
constexpr std::int64_t kAuxEdgeSamples = 240;
[[nodiscard]] double edgeGain(std::int64_t position, std::int64_t length);

// The entry's fields as JSON members (no braces), after clip_id, speed, end and auto_advance.
[[nodiscard]] std::string entryTransitionJson(PlaylistEntry const& entry);
// Reads transition, transition_frames, audio_frames and own_audio of an entry object. False with `problem`.
bool readEntryTransition(Json const& item, PlaylistEntry& entry, std::string& problem);
[[nodiscard]] std::string auxJson(AuxTrack const& aux);
// Merges the members of an aux object into `aux` (a partial object keeps the other fields). False with `problem`.
bool readAux(Json const& item, AuxTrack& aux, std::string& problem);
} // namespace replay
