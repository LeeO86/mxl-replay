#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace replay
{
// Stored frames a channel needs soon, read on a loader thread before the playout does (SPECIFICATION.md §5.3):
// the frames ahead of the position and the first frames of the item after the current one, so the output
// thread does not wait for the disk at a playlist transition or a cued clip's start.
class ReadAhead
{
public:
    struct Request
    {
        int channel = 0;
        int camera = 0;
        std::uint64_t fromNs = 0;
        std::uint64_t toNs = 0;
    };
    // Reads one request's frames on the loader thread.
    using Read = std::function<void(Request const&)>;

    explicit ReadAhead(Read read);
    ~ReadAhead();
    ReadAhead(ReadAhead const&) = delete;
    ReadAhead& operator=(ReadAhead const&) = delete;

    // Queues the request and returns at once. The oldest requests are dropped when kMaxQueued wait.
    void request(Request request);
    static constexpr std::size_t kMaxQueued = 16;

private:
    void run();

    Read read_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Request> queue_;
    bool stop_ = false;
    std::thread thread_;
};
} // namespace replay
