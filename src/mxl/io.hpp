#pragma once

#include "app/engine.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace replay
{
// Whether a grain that left an MXL flow's history before the reader got to it was dropped.
// The reader waits when its next grain is not written yet. When that grain is gone a moment
// later, the writer started or resumed after it (it opened the flow before writing, or it
// restarted): the grain was never written and nothing was lost. A reader that fell behind its
// writer lost it, and so did one that slept longer than the history between two looks.
struct ReaderWait
{
    bool waiting = false;
    std::uint64_t sinceNs = 0;

    // The writer had not reached the reader's next grain at `nowNs`.
    void wait(std::uint64_t nowNs)
    {
        waiting = true;
        sinceNs = nowNs;
    }
    // The reader got a grain: it follows the writer again.
    void read() { waiting = false; }
    // `historyNs` is how long the flow keeps a grain (0 when unknown: every late grain counts).
    [[nodiscard]] bool lateIsDrop(std::uint64_t nowNs, std::uint64_t historyNs) const { return !waiting || nowNs - sinceNs >= historyNs; }
};

// Stall detection for one connected camera input (a video phase). A writer that stopped, a flow
// that was removed, and a fabrics mirror whose link is down all look the same to the reader: no
// new grain. When nothing was recorded for `timeoutNs` (REPLAY_INPUT_STALL_S) the input has
// stopped; the next recorded grain resumes it.
struct InputWatch
{
    std::uint64_t timeoutNs = 0;
    std::uint64_t lastNs = 0;  // the last recorded grain, or the connection
    std::uint64_t skipped = 0; // grains read but not recordable (invalid or incomplete) since lastNs
    bool stopped = false;

    explicit InputWatch(std::uint64_t timeout = 0)
        : timeoutNs(timeout)
    {
    }
    // The input is connected at `nowNs`: starts the clock once.
    void connect(std::uint64_t nowNs)
    {
        if (lastNs == 0)
        {
            lastNs = nowNs;
        }
    }
    // A grain was recorded at `nowNs`. Returns the gap when this resumes a stopped input, else 0.
    std::uint64_t recorded(std::uint64_t nowNs)
    {
        auto const gap = stopped && nowNs > lastNs ? nowNs - lastNs : 0;
        stopped = false;
        skipped = 0;
        lastNs = nowNs;
        return gap;
    }
    // Nothing was recorded at `nowNs`. True once, when the input stops.
    bool idle(std::uint64_t nowNs)
    {
        if (stopped || lastNs == 0 || nowNs < lastNs + timeoutNs)
        {
            return false;
        }
        stopped = true;
        return true;
    }
    // Why it stopped: the flow could not be opened, its grains could not be recorded, or none came.
    [[nodiscard]] char const* reason(bool open) const { return !open ? "flow_missing" : skipped != 0 ? "invalid_grains" : "no_grains"; }
};

class MxlBridge
{
public:
    explicit MxlBridge(Engine& engine);
    ~MxlBridge();
    void start();
    void stop();
    // Writes one channel's grain; channels may call it from their own threads.
    void publish(int channel, RenderedFrame const& frame, std::uint64_t taiNs);
    [[nodiscard]] bool active() const { return active_; }

private:
    // One thread per camera phase: each records (encodes) its own grains, so cameras
    // no longer wait for each other.
    void readInput(int camera, int phase);
    Engine& engine_;
    std::atomic<bool> run_{false};
    std::atomic<bool> active_{false};
    std::vector<std::thread> threads_;
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace replay
