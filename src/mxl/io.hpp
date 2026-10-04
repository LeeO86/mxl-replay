#pragma once

#include "app/engine.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace replay
{
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
