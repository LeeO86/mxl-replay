#include "playout/readahead.hpp"

#include <utility>

namespace replay
{
ReadAhead::ReadAhead(Read read) : read_(std::move(read)), thread_([this] { run(); })
{
}

ReadAhead::~ReadAhead()
{
    {
        std::lock_guard lock{mutex_};
        stop_ = true;
    }
    wake_.notify_one();
    thread_.join();
}

void ReadAhead::request(Request request)
{
    {
        std::lock_guard lock{mutex_};
        queue_.push_back(request);
        while (queue_.size() > kMaxQueued)
        {
            queue_.pop_front();
        }
    }
    wake_.notify_one();
}

void ReadAhead::run()
{
    std::unique_lock lock{mutex_};
    for (;;)
    {
        wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
        if (stop_)
        {
            return;
        }
        auto const next = queue_.front();
        queue_.pop_front();
        lock.unlock();
        read_(next);
        lock.lock();
    }
}
} // namespace replay
