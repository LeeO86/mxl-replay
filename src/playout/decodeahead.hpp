#pragma once

#include "playout/scheduler.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

namespace replay
{
// A source frame a channel's next grains show: its camera and the time it is asked for (frame × source period).
struct AheadFrame
{
    int camera = 0;
    std::uint64_t taiNs = 0;

    friend bool operator==(AheadFrame const&, AheadFrame const&) = default;
};

// One source of the frames a channel's next grains show (1.8.6): a copy of a scheduler as render steps it, the
// frames its current grain shows, and its camera. Frames after `untilNs` are not shown (the outgoing entry of a
// mix holds at its OUT; a clip that cuts, loops or plays the next one at OUT).
struct AheadSource
{
    Scheduler scheduler;
    SourcePick current;
    int camera = 0;
    std::uint64_t periodNs = 0; // the camera's source frame period
    double hfrFactor = 1;
    double hfrSnap = 0.1;
    MotionMode motion = MotionMode::Repeat;
    bool withB = true;
    std::uint64_t untilNs = std::numeric_limits<std::uint64_t>::max();
    int grains = 0; // how many grains ahead (0: planAhead's `grains`)
};
// The stored pictures one channel's next grains show, decoded ahead on a worker thread into a few slots
// (SPECIFICATION.md §5.1, 1.8.5). The playout plans the frames each grain; the worker decodes them in that
// order. The playout takes a slot by frame: a ready one at once, one still decoding after a bounded wait; else
// it decodes the frame itself. No allocation after construction.
//
// Slots (1.8.6): a plan holds at most kMaxPlan frames (before OUT 2 grains of one source and the next clip's first
// 6 frames; in a mix 6 grains of the incoming entry and 2 of the outgoing one), and the current grain keeps at
// most kMaxKeep frames it is about to take. kSlots = kMaxPlan + kMaxKeep, so the worker never waits for a slot to
// come back. A slot whose picture was taken is reused first: a frame decoded early for a later grain stays.
class DecodeAhead
{
public:
    static constexpr int kMaxPlan = 8;
    static constexpr int kMaxKeep = 2;
    static constexpr int kSlots = kMaxPlan + kMaxKeep;
    // Decodes `frame` into slot `slot` on the worker thread; the stored frame's time in `storedNs`. False: no
    // picture (not stored, a rejected bitstream).
    using Decode = std::function<bool(AheadFrame const& frame, int slot, std::uint64_t& storedNs)>;

    // `start` runs first on the worker thread (its GPU state).
    explicit DecodeAhead(Decode decode, std::function<void()> const& start = {});
    ~DecodeAhead();
    DecodeAhead(DecodeAhead const&) = delete;
    DecodeAhead& operator=(DecodeAhead const&) = delete;

    // The frames the next grains need, in the order they are needed (at most kMaxPlan); replaces the last plan.
    // `keep` (at most kMaxKeep): frames the current grain is about to take; a slot that has one is not reused,
    // but they are not decoded.
    void plan(AheadFrame const* frames, int count, AheadFrame const* keep = nullptr, int keepCount = 0);

    enum class Result
    {
        Hit,  // decoded ahead: use `slot`, then give() it back
        Late, // still decoding after the wait
        Miss  // not planned, or could not be decoded
    };
    struct Take
    {
        Result result = Result::Miss;
        int slot = -1;
        std::uint64_t storedNs = 0;
        std::chrono::nanoseconds waited{0};
        bool planned = false; // the worker had the frame (decoding or decoded)
    };
    // The slot that holds `frame`. One still decoding is waited for up to `maxWait`. A hit is held (the worker
    // does not reuse it) until give().
    Take take(AheadFrame const& frame, std::chrono::nanoseconds maxWait);
    void give(int slot);

private:
    enum class State
    {
        Free,
        Decoding,
        Ready,
        Failed // not decoded: not tried again while it is planned
    };
    struct Slot
    {
        State state = State::Free;
        AheadFrame frame;
        std::uint64_t storedNs = 0;
        std::uint64_t used = 0;
        bool held = false;
        bool taken = false; // the playout took its picture
    };
    void run();
    [[nodiscard]] int find(AheadFrame const& frame) const;
    [[nodiscard]] bool planned(AheadFrame const& frame) const;

    Decode decode_;
    std::mutex mutex_;
    std::condition_variable wake_; // the worker: a new plan, a slot given back
    std::condition_variable done_; // the playout: a decode finished
    std::array<AheadFrame, kMaxPlan> plan_{};
    int planCount_ = 0;
    std::array<AheadFrame, kMaxKeep> keep_{};
    int keepCount_ = 0;
    std::array<Slot, kSlots> slots_{};
    std::uint64_t uses_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

// What a channel posts to its DecodeAhead each grain: the frames the next `grains` grains show from `sources`
// (one, or the incoming and the outgoing entry of a mix), grain by grain, with `next` (the next clip's first
// frames, at most 6) after the first grain's; and the current grains' frames to keep.
struct DecodePlan
{
    std::array<AheadFrame, DecodeAhead::kMaxPlan> frames{};
    int count = 0;
    std::array<AheadFrame, DecodeAhead::kMaxKeep> keep{};
    int keepCount = 0;
};
[[nodiscard]] DecodePlan planAhead(AheadSource const* sources, int count, AheadFrame const* next, int nextCount, int grains);
} // namespace replay
