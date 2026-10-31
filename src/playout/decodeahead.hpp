#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
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

// The stored pictures one channel's next grains show, decoded ahead on a worker thread into a few slots
// (SPECIFICATION.md §5.1, 1.8.5). The playout plans the frames each grain; the worker decodes them in that
// order. The playout takes a slot by frame: a ready one at once, one still decoding after a bounded wait; else
// it decodes the frame itself. No allocation after construction.
class DecodeAhead
{
public:
    static constexpr int kSlots = 4;
    static constexpr int kMaxPlan = 4;
    // Decodes `frame` into slot `slot` on the worker thread; the stored frame's time in `storedNs`. False: no
    // picture (not stored, a rejected bitstream).
    using Decode = std::function<bool(AheadFrame const& frame, int slot, std::uint64_t& storedNs)>;

    // `start` runs first on the worker thread (its GPU state).
    explicit DecodeAhead(Decode decode, std::function<void()> const& start = {});
    ~DecodeAhead();
    DecodeAhead(DecodeAhead const&) = delete;
    DecodeAhead& operator=(DecodeAhead const&) = delete;

    // The frames the next grains need, in the order they are needed (at most kMaxPlan); replaces the last plan.
    void plan(AheadFrame const* frames, int count);

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
    std::array<Slot, kSlots> slots_{};
    std::uint64_t uses_ = 0;
    bool stop_ = false;
    std::thread thread_;
};
} // namespace replay
