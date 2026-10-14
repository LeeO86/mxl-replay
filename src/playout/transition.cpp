#include "playout/transition.hpp"

#include "util/logging.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace replay
{
char const* transitionName(Transition transition)
{
    return transition == Transition::Mix ? "mix" : "cut";
}

Transition parseTransition(std::string const& text, bool* ok)
{
    if (ok != nullptr)
    {
        *ok = text == "cut" || text == "mix";
    }
    return text == "mix" ? Transition::Mix : Transition::Cut;
}

TransitionPlan transitionPlan(PlaylistEntry const& incoming)
{
    TransitionPlan plan;
    if (incoming.transition == Transition::Mix)
    {
        plan.videoFrames = std::clamp(incoming.transitionFrames > 0 ? incoming.transitionFrames : kDefaultMixFrames, 1, kMaxTransitionFrames);
    }
    plan.audioFrames = std::clamp(incoming.audioFrames < 0 ? plan.videoFrames : incoming.audioFrames, 0, kMaxTransitionFrames);
    return plan;
}

double mixWeight(int k, int n)
{
    if (n <= 0 || k >= n)
    {
        return 1.0;
    }
    if (k < 0)
    {
        return 0.0;
    }
    return static_cast<double>(k + 1) / static_cast<double>(n + 1);
}

std::uint32_t blendWeight1024(double weight)
{
    return static_cast<std::uint32_t>(std::clamp(std::lround(weight * 1024.0), 0L, 1024L));
}

std::uint16_t blendSample(std::uint16_t a, std::uint16_t b, std::uint32_t weight1024)
{
    return static_cast<std::uint16_t>((a * (1024u - weight1024) + b * weight1024) >> 10);
}

CrossfadeGains crossfadeGains(std::int64_t n, std::int64_t total)
{
    if (total <= 0 || n >= total)
    {
        return {0.f, 1.f};
    }
    if (n < 0)
    {
        return {1.f, 0.f};
    }
    double const t = (static_cast<double>(n) + 0.5) / static_cast<double>(total);
    double const angle = t * std::numbers::pi / 2.0;
    return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

double dbToGain(double db)
{
    return db <= -120.0 ? 0.0 : std::pow(10.0, db / 20.0);
}

double DuckRamp::next(bool ducked, double depthDb, double attackSamples, double releaseSamples)
{
    double const depth = std::min(0.0, depthDb);
    double const target = ducked ? depth : 0.0;
    // The whole depth per ramp (a depth changed while ducked moves at the same rate).
    double const range = std::max(-depth, std::fabs(db - target));
    if (db > target)
    {
        db = attackSamples >= 1.0 ? std::max(target, db - range / attackSamples) : target;
    }
    else if (db < target)
    {
        db = releaseSamples >= 1.0 ? std::min(target, db + range / releaseSamples) : target;
    }
    return dbToGain(db);
}

double endFadeGain(std::int64_t remainingNs, std::int64_t fadeNs)
{
    if (remainingNs < 0 || fadeNs <= 0 || remainingNs >= fadeNs)
    {
        return 1.0;
    }
    return static_cast<double>(remainingNs) / static_cast<double>(fadeNs);
}

double edgeGain(std::int64_t position, std::int64_t length)
{
    if (position < 0 || position >= length)
    {
        return 0.0;
    }
    double const edge = static_cast<double>(std::min(kAuxEdgeSamples, length / 2 > 0 ? length / 2 : 1));
    double const fromStart = (static_cast<double>(position) + 0.5) / edge;
    double const toEnd = (static_cast<double>(length - position) - 0.5) / edge;
    return std::clamp(std::min(fromStart, toEnd), 0.0, 1.0);
}

std::string entryTransitionJson(PlaylistEntry const& entry)
{
    auto const plan = transitionPlan(entry);
    std::ostringstream out;
    out << "\"transition\":\"" << transitionName(entry.transition) << "\",\"transition_frames\":" << plan.videoFrames
        << ",\"audio_frames\":" << entry.audioFrames << ",\"audio_frames_effective\":" << plan.audioFrames
        << ",\"own_audio\":" << (entry.ownAudio ? "true" : "false");
    return out.str();
}

namespace
{
// An integer member in [low, high]; absent: unchanged. False with `problem`.
bool readInt(Json const& item, char const* key, int low, int high, int& value, std::string& problem)
{
    auto const* field = item.get(key);
    if (field == nullptr)
    {
        return true;
    }
    auto const number = jsonInt(*field);
    if (!number || *number < low || *number > high)
    {
        problem = std::string(key) + " must be an integer from " + std::to_string(low) + " to " + std::to_string(high);
        return false;
    }
    value = static_cast<int>(*number);
    return true;
}

bool readNumber(Json const& item, char const* key, double low, double high, double& value, std::string& problem)
{
    auto const* field = item.get(key);
    if (field == nullptr)
    {
        return true;
    }
    auto const number = jsonDouble(*field);
    if (!number || !std::isfinite(*number) || *number < low || *number > high)
    {
        std::ostringstream text;
        text << key << " must be a number from " << low << " to " << high;
        problem = text.str();
        return false;
    }
    value = *number;
    return true;
}

bool readBool(Json const& item, char const* key, bool& value, std::string& problem)
{
    auto const* field = item.get(key);
    if (field == nullptr)
    {
        return true;
    }
    auto const flag = jsonBool(*field);
    if (!flag)
    {
        problem = std::string(key) + " must be true or false";
        return false;
    }
    value = *flag;
    return true;
}
} // namespace

bool readEntryTransition(Json const& item, PlaylistEntry& entry, std::string& problem)
{
    if (auto const* field = item.get("transition"))
    {
        auto const text = jsonText(*field);
        bool ok = false;
        if (text)
        {
            entry.transition = parseTransition(*text, &ok);
        }
        if (!ok)
        {
            problem = "transition must be cut or mix";
            return false;
        }
    }
    if (!readInt(item, "transition_frames", 0, kMaxTransitionFrames, entry.transitionFrames, problem) ||
        !readInt(item, "audio_frames", -1, kMaxTransitionFrames, entry.audioFrames, problem) || !readBool(item, "own_audio", entry.ownAudio, problem))
    {
        return false;
    }
    if (entry.transition == Transition::Mix && entry.transitionFrames == 0)
    {
        entry.transitionFrames = kDefaultMixFrames;
    }
    if (entry.transition == Transition::Cut)
    {
        entry.transitionFrames = 0;
    }
    return true;
}

std::string auxJson(AuxTrack const& aux)
{
    std::ostringstream out;
    out << "{\"source\":\"" << jsonEscape(aux.source) << "\",\"level_db\":" << aux.levelDb << ",\"loop\":" << (aux.loop ? "true" : "false")
        << ",\"fade_out_frames\":" << aux.fadeOutFrames << ",\"duck_db\":" << aux.duckDb << ",\"attack_ms\":" << aux.attackMs
        << ",\"release_ms\":" << aux.releaseMs << '}';
    return out.str();
}

bool readAux(Json const& item, AuxTrack& aux, std::string& problem)
{
    if (item.kind != Json::Kind::Object)
    {
        problem = "aux must be an object";
        return false;
    }
    if (auto const* field = item.get("source"))
    {
        auto const text = jsonText(*field);
        if (!text)
        {
            problem = "aux source must be a clip or audio item id";
            return false;
        }
        aux.source = *text;
    }
    return readNumber(item, "level_db", -60, 12, aux.levelDb, problem) && readBool(item, "loop", aux.loop, problem) &&
           readInt(item, "fade_out_frames", 0, 1000, aux.fadeOutFrames, problem) && readNumber(item, "duck_db", -60, 0, aux.duckDb, problem) &&
           readInt(item, "attack_ms", 0, 5000, aux.attackMs, problem) && readInt(item, "release_ms", 0, 10000, aux.releaseMs, problem);
}
} // namespace replay
