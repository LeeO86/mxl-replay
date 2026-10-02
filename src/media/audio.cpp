#include "media/audio.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <string>

namespace replay
{
namespace
{
constexpr int kWindow = 512;
constexpr int kHop = 256;

std::vector<float> channel(std::vector<float> const& in, int channels, int channelIndex)
{
    std::vector<float> out;
    out.reserve(in.size() / static_cast<std::size_t>(std::max(channels, 1)));
    for (std::size_t i = static_cast<std::size_t>(channelIndex); i < in.size(); i += static_cast<std::size_t>(channels))
    {
        out.push_back(in[i]);
    }
    return out;
}

std::vector<float> interleave(std::vector<std::vector<float>> const& chans)
{
    if (chans.empty())
    {
        return {};
    }
    std::size_t const n = chans[0].size();
    std::vector<float> out(n * chans.size(), 0.f);
    for (std::size_t c = 0; c < chans.size(); ++c)
    {
        for (std::size_t i = 0; i < n && i < chans[c].size(); ++i)
        {
            out[i * chans.size() + c] = chans[c][i];
        }
    }
    return out;
}

void fft(std::vector<std::complex<float>>& a, bool inverse)
{
    std::size_t const n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        float const ang = (inverse ? 2.f : -2.f) * 3.1415926535f / static_cast<float>(len);
        std::complex<float> const wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<float> w(1.f, 0.f);
            for (std::size_t j = 0; j < len / 2; ++j)
            {
                auto const u = a[i + j];
                auto const v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
    {
        for (auto& v : a)
        {
            v /= static_cast<float>(n);
        }
    }
}

std::vector<float> hann(int n)
{
    std::vector<float> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        w[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(2.f * 3.1415926535f * static_cast<float>(i) / static_cast<float>(n));
    }
    return w;
}

std::size_t targetLength(std::size_t input, double speed)
{
    if (speed <= 0.0001)
    {
        speed = 0.0001;
    }
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(static_cast<double>(input) / speed)));
}

std::vector<float> phaseVocoder(std::vector<float> const& in, std::size_t target)
{
    if (in.size() < static_cast<std::size_t>(kWindow))
    {
        return {};
    }
    auto const window = hann(kWindow);
    double const ratio = static_cast<double>(in.size()) / static_cast<double>(target);
    int const analysisHop = std::max(1, static_cast<int>(std::lround(static_cast<double>(kHop) * ratio)));
    std::vector<float> out(target, 0.f);
    std::vector<float> weight(target, 0.f);
    std::vector<float> lastPhase(static_cast<std::size_t>(kWindow / 2 + 1), 0.f);
    std::vector<float> sumPhase(lastPhase.size(), 0.f);
    std::size_t inPos = 0;
    std::size_t outPos = 0;
    while (outPos + static_cast<std::size_t>(kWindow) < target && inPos + static_cast<std::size_t>(kWindow) <= in.size())
    {
        std::vector<std::complex<float>> spec(static_cast<std::size_t>(kWindow));
        for (int i = 0; i < kWindow; ++i)
        {
            spec[static_cast<std::size_t>(i)] = in[inPos + static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
        }
        fft(spec, false);
        for (std::size_t bin = 0; bin < sumPhase.size(); ++bin)
        {
            float const phase = std::arg(spec[bin]);
            float const delta = phase - lastPhase[bin];
            lastPhase[bin] = phase;
            float const expected = 2.f * 3.1415926535f * static_cast<float>(bin) * static_cast<float>(analysisHop) / static_cast<float>(kWindow);
            float deviation = delta - expected;
            deviation -= 2.f * 3.1415926535f * std::round(deviation / (2.f * 3.1415926535f));
            float const trueFreq = 2.f * 3.1415926535f * static_cast<float>(bin) / static_cast<float>(kWindow) + deviation / static_cast<float>(analysisHop);
            sumPhase[bin] += trueFreq * static_cast<float>(kHop);
            spec[bin] = std::polar(std::abs(spec[bin]), sumPhase[bin]);
            if (bin > 0 && bin < sumPhase.size() - 1)
            {
                spec[static_cast<std::size_t>(kWindow) - bin] = std::conj(spec[bin]);
            }
        }
        fft(spec, true);
        for (int i = 0; i < kWindow && outPos + static_cast<std::size_t>(i) < target; ++i)
        {
            out[outPos + static_cast<std::size_t>(i)] += spec[static_cast<std::size_t>(i)].real() * window[static_cast<std::size_t>(i)];
            weight[outPos + static_cast<std::size_t>(i)] += window[static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
        }
        inPos += static_cast<std::size_t>(analysisHop);
        outPos += static_cast<std::size_t>(kHop);
    }
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (weight[i] > 1e-4f)
        {
            out[i] /= weight[i];
        }
    }
    return out;
}

std::vector<float> wsolaStretch(std::vector<float> const& in, std::size_t target)
{
    std::vector<float> out(target, 0.f);
    if (in.empty())
    {
        return out;
    }
    int const win = static_cast<int>(std::min<std::size_t>(kWindow, in.size()));
    int const hop = std::max(1, win / 2);
    int const tolerance = std::max(1, hop / 2);
    auto const window = hann(win);
    double const step = static_cast<double>(in.size()) / static_cast<double>(std::max<std::size_t>(1, (target / static_cast<std::size_t>(hop))));
    double src = 0.0;
    std::size_t dst = 0;
    std::vector<float> previous(static_cast<std::size_t>(win), 0.f);
    bool havePrev = false;
    while (dst < target)
    {
        int base = static_cast<int>(std::llround(src));
        int best = base;
        if (havePrev)
        {
            float bestScore = -1e30f;
            int const lo = std::max(0, base - tolerance);
            int const hi = std::min(static_cast<int>(in.size()) - win, base + tolerance);
            for (int candidate = lo; candidate <= hi; ++candidate)
            {
                float score = 0.f;
                int const compare = std::min(hop, win);
                for (int i = 0; i < compare; ++i)
                {
                    score += previous[static_cast<std::size_t>(i)] * in[static_cast<std::size_t>(candidate + i)];
                }
                if (score > bestScore)
                {
                    bestScore = score;
                    best = candidate;
                }
            }
        }
        best = std::clamp(best, 0, std::max(0, static_cast<int>(in.size()) - 1));
        for (int i = 0; i < win && dst + static_cast<std::size_t>(i) < target; ++i)
        {
            int const si = std::min(best + i, static_cast<int>(in.size()) - 1);
            out[dst + static_cast<std::size_t>(i)] += in[static_cast<std::size_t>(si)] * window[static_cast<std::size_t>(i)];
        }
        previous.assign(static_cast<std::size_t>(win), 0.f);
        for (int i = 0; i < win; ++i)
        {
            int const si = std::min(best + i, static_cast<int>(in.size()) - 1);
            previous[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(si)];
        }
        havePrev = true;
        src += step;
        dst += static_cast<std::size_t>(hop);
        if (src >= static_cast<double>(in.size()))
        {
            break;
        }
    }
    // The overlap-add above is unnormalised. A second pass divides by the window sum
    // so a constant input stays a constant.
    std::vector<float> norm(target, 0.f);
    src = 0.0;
    dst = 0;
    while (dst < target && src < static_cast<double>(in.size()))
    {
        for (int i = 0; i < win && dst + static_cast<std::size_t>(i) < target; ++i)
        {
            norm[dst + static_cast<std::size_t>(i)] += window[static_cast<std::size_t>(i)];
        }
        src += step;
        dst += static_cast<std::size_t>(hop);
    }
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (norm[i] > 1e-4f)
        {
            out[i] /= norm[i];
        }
    }
    return out;
}

std::vector<float> stretchOne(std::vector<float> const& in, double speed, bool* wsola)
{
    auto const target = targetLength(in.size(), speed);
    if (std::fabs(speed - 1.0) < 1e-4)
    {
        auto out = in;
        out.resize(target, 0.f);
        return out;
    }
    auto vocoded = phaseVocoder(in, target);
    if (vocoded.size() == target && in.size() >= static_cast<std::size_t>(kWindow))
    {
        if (wsola != nullptr)
        {
            *wsola = false;
        }
        return vocoded;
    }
    if (wsola != nullptr)
    {
        *wsola = true;
    }
    return wsolaStretch(in, target);
}
} // namespace

char const* audioModeName(AudioMode mode)
{
    switch (mode)
    {
    case AudioMode::Mute:
        return "mute";
    case AudioMode::Stretch:
        return "stretch";
    case AudioMode::Follow:
        return "follow";
    }
    return "mute";
}

AudioMode parseAudioMode(std::string const& text, bool* ok)
{
    if (ok != nullptr)
    {
        *ok = true;
    }
    if (text == "mute")
    {
        return AudioMode::Mute;
    }
    if (text == "stretch")
    {
        return AudioMode::Stretch;
    }
    if (text == "follow")
    {
        return AudioMode::Follow;
    }
    if (ok != nullptr)
    {
        *ok = false;
    }
    return AudioMode::Mute;
}

StretchResult timeStretch(std::vector<float> const& interleaved, int channels, double speed)
{
    StretchResult result;
    if (channels < 1)
    {
        channels = 1;
    }
    bool anyWsola = false;
    std::vector<std::vector<float>> chans;
    std::size_t target = 0;
    for (int c = 0; c < channels; ++c)
    {
        bool wsolaUsed = false;
        auto stretched = stretchOne(channel(interleaved, channels, c), speed, &wsolaUsed);
        anyWsola = anyWsola || wsolaUsed;
        target = stretched.size();
        chans.push_back(std::move(stretched));
    }
    result.samples = interleave(chans);
    result.usedWsola = anyWsola;
    (void)target;
    return result;
}

std::vector<float> resampleLinear(std::vector<float> const& interleaved, int channels, double speed)
{
    if (channels < 1)
    {
        channels = 1;
    }
    std::size_t const frames = interleaved.size() / static_cast<std::size_t>(channels);
    std::size_t const target = targetLength(frames, speed);
    std::vector<float> out(target * static_cast<std::size_t>(channels), 0.f);
    if (frames == 0)
    {
        return out;
    }
    for (std::size_t i = 0; i < target; ++i)
    {
        double const src = static_cast<double>(i) * speed;
        std::size_t const i0 = std::min(frames - 1, static_cast<std::size_t>(src));
        std::size_t const i1 = std::min(frames - 1, i0 + 1);
        float const frac = static_cast<float>(src - static_cast<double>(i0));
        for (int c = 0; c < channels; ++c)
        {
            float const a = interleaved[i0 * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
            float const b = interleaved[i1 * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
            out[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = a + (b - a) * frac;
        }
    }
    return out;
}

void applyMuteFade(std::vector<float>& interleaved, int frameInFade, int fadeFrames)
{
    if (fadeFrames <= 0 || interleaved.empty())
    {
        return;
    }
    float const start = 1.f - static_cast<float>(frameInFade) / static_cast<float>(fadeFrames);
    float const end = 1.f - static_cast<float>(frameInFade + 1) / static_cast<float>(fadeFrames);
    for (std::size_t i = 0; i < interleaved.size(); ++i)
    {
        float const t = interleaved.size() == 1 ? 0.f : static_cast<float>(i) / static_cast<float>(interleaved.size() - 1);
        interleaved[i] *= start + (end - start) * t;
    }
}

std::vector<std::uint8_t> writeWav(std::vector<float> const& interleaved, int channels, int sampleRate)
{
    if (channels < 1)
    {
        channels = 1;
    }
    std::uint32_t const dataBytes = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
    std::uint32_t const riff = 36u + dataBytes;
    std::vector<std::uint8_t> out(44 + dataBytes);
    auto put = [&](std::size_t offset, char const* text, std::size_t n) { std::memcpy(out.data() + offset, text, n); };
    auto u32 = [&](std::size_t offset, std::uint32_t v) {
        out[offset] = static_cast<std::uint8_t>(v);
        out[offset + 1] = static_cast<std::uint8_t>(v >> 8);
        out[offset + 2] = static_cast<std::uint8_t>(v >> 16);
        out[offset + 3] = static_cast<std::uint8_t>(v >> 24);
    };
    auto u16 = [&](std::size_t offset, std::uint16_t v) {
        out[offset] = static_cast<std::uint8_t>(v);
        out[offset + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    put(0, "RIFF", 4);
    u32(4, riff);
    put(8, "WAVE", 4);
    put(12, "fmt ", 4);
    u32(16, 16);
    u16(20, 3);
    u16(22, static_cast<std::uint16_t>(channels));
    u32(24, static_cast<std::uint32_t>(sampleRate));
    u32(28, static_cast<std::uint32_t>(sampleRate * channels * 4));
    u16(32, static_cast<std::uint16_t>(channels * 4));
    u16(34, 32);
    put(36, "data", 4);
    u32(40, dataBytes);
    std::memcpy(out.data() + 44, interleaved.data(), dataBytes);
    return out;
}
} // namespace replay
