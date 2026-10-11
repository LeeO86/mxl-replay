#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace replay
{
struct GpuFlow;
struct FlowPair;

// One computed flow: on the device, or on the host without a GPU (REPLAY_ALLOW_CPU_INTERP). Both null: it
// could not be computed.
struct AheadFlow
{
    std::shared_ptr<GpuFlow const> gpu;
    std::shared_ptr<FlowPair const> cpu;
};

// The optical flows of one channel, computed ahead on a worker thread (SPECIFICATION.md §5.3). The playout
// posts the pairs its next grains interpolate; the worker computes one at a time, the first pair it can
// still finish before its last grain; the playout uses a flow only when it is there and never waits for one.
class FlowAhead
{
public:
    using Clock = std::chrono::steady_clock;
    struct Pair
    {
        std::string key; // flowKey(camera, taiA, taiB)
        int camera = 0;
        std::uint64_t taiA = 0;
        std::uint64_t taiB = 0;
        Clock::time_point due; // when the last grain that uses it is rendered
    };
    // Computes a pair's flow on the worker thread.
    using Compute = std::function<AheadFlow(Pair const&)>;

    // `start` runs first on the worker thread (its GPU state). The `capacity` newest or last used flows are kept.
    FlowAhead(Compute compute, std::function<void()> const& start = {}, std::size_t capacity = 12);
    ~FlowAhead();
    FlowAhead(FlowAhead const&) = delete;
    FlowAhead& operator=(FlowAhead const&) = delete;

    // The pairs the next grains interpolate, in the order they are needed; replaces the last plan.
    void plan(std::vector<Pair> pairs);
    // The flow of `key` once it is computed, else empty.
    [[nodiscard]] AheadFlow find(std::string const& key);
    // How long the last flow took: frames read and decoded, flow computed (seconds).
    [[nodiscard]] double flowSeconds() const;

private:
    struct Entry
    {
        AheadFlow flow;
        std::uint64_t used = 0;
    };
    void run();

    Compute compute_;
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Pair> plan_;
    std::map<std::string, Entry> flows_;
    std::uint64_t uses_ = 0;
    // The last three flows' times: the shortest is what one more flow takes (a slow first flow, which sets
    // up the worker's GPU state, or a single slow one does not make the worker skip pairs).
    std::array<double, 3> recent_{};
    std::size_t last_ = 0;
    // A plan that no flow can make in time is measured again a second after the last flow.
    Clock::time_point retry_{};
    bool stop_ = false;
    std::thread thread_;
};
} // namespace replay
