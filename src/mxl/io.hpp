#pragma once

#include "app/engine.hpp"

#include <atomic>
#include <thread>

namespace replay
{
class MxlBridge
{
public:
    explicit MxlBridge(Engine& engine);
    ~MxlBridge();
    void start();
    void stop();
    void publish(int channel, RenderedFrame const& frame, std::uint64_t taiNs);
    [[nodiscard]] bool active() const { return active_; }

private:
    void readLoop();
    Engine& engine_;
    std::atomic<bool> run_{false};
    std::atomic<bool> active_{false};
    std::thread thread_;
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace replay
