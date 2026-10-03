#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace replay
{
enum class AudioMode
{
    Mute,
    Stretch,
    Follow
};

[[nodiscard]] char const* audioModeName(AudioMode mode);
[[nodiscard]] AudioMode parseAudioMode(std::string const& text, bool* ok = nullptr);

// Pitch-preserving stretch. Output length is round(input.size() / speed) per channel.
// Phase vocoder is tried first; WSOLA is the fallback when the block is shorter than one window.
struct StretchResult
{
    std::vector<float> samples;
    bool usedWsola = false;
};

[[nodiscard]] StretchResult timeStretch(std::vector<float> const& interleaved, int channels, double speed);

// Linear resample. Pitch follows the speed. `follow` uses this.
[[nodiscard]] std::vector<float> resampleLinear(std::vector<float> const& interleaved, int channels, double speed);

// Fade from full scale to silence across `fadeFrames` frames. `frameInFade` is 0-based.
void applyMuteFade(std::vector<float>& interleaved, int frameInFade, int fadeFrames);

[[nodiscard]] std::vector<std::uint8_t> writeWav(std::vector<float> const& interleaved, int channels, int sampleRate);
} // namespace replay
