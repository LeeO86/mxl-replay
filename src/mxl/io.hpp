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
