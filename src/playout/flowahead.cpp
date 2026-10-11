#include "playout/flowahead.hpp"

#include <algorithm>
#include <utility>

namespace replay
{
FlowAhead::FlowAhead(Compute compute, std::function<void()> const& start, std::size_t capacity)
    : compute_(std::move(compute))
    , capacity_(std::max<std::size_t>(1, capacity))
    , thread_([this, start] {
        if (start)
        {
            start();
        }
        run();
    })
{
}

FlowAhead::~FlowAhead()
{
    {
        std::lock_guard lock{mutex_};
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void FlowAhead::plan(std::vector<Pair> pairs)
{
    {
        std::lock_guard lock{mutex_};
        if (pairs.empty() && plan_.empty())
        {
            return;
        }
        plan_ = std::move(pairs);
    }
    wake_.notify_one();
}

AheadFlow FlowAhead::find(std::string const& key)
{
    std::lock_guard lock{mutex_};
    auto const it = flows_.find(key);
    if (it == flows_.end())
    {
        return {};
    }
    it->second.used = ++uses_;
    return it->second.flow;
}

double FlowAhead::flowSeconds() const
{
    std::lock_guard lock{mutex_};
    return recent_[last_];
}

void FlowAhead::run()
{
    std::unique_lock lock{mutex_};
    while (!stop_)
    {
        // The first pair not computed yet that one more flow can finish before its last grain. When none can,
        // the one needed longest, once a second: that flow measures the GPU again.
        auto const now = Clock::now();
        auto const oneFlow = *std::min_element(recent_.begin(), recent_.end());
        auto const ready = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(oneFlow));
        Pair const* next = nullptr;
        Pair const* latest = nullptr;
        for (auto const& pair : plan_)
        {
            if (flows_.count(pair.key) != 0)
            {
                continue;
            }
            if (latest == nullptr || pair.due > latest->due)
            {
                latest = &pair;
            }
            if (next == nullptr && pair.due >= ready)
            {
                next = &pair;
            }
        }
        if (next == nullptr && latest != nullptr && now >= retry_)
        {
            next = latest;
        }
        if (next == nullptr)
        {
            if (latest == nullptr)
            {
                wake_.wait(lock);
            }
            else
            {
                wake_.wait_until(lock, retry_);
            }
            continue;
        }
        auto const pair = *next;
        lock.unlock();
        auto const began = Clock::now();
        auto flow = compute_(pair);
        auto const ended = Clock::now();
        lock.lock();
        if (flow.gpu || flow.cpu)
        {
            last_ = (last_ + 1) % recent_.size();
            recent_[last_] = std::chrono::duration<double>(ended - began).count();
        }
        retry_ = ended + std::chrono::seconds(1);
        AheadFlow evicted;
        if (flows_.size() >= capacity_)
        {
            auto const oldest = std::min_element(flows_.begin(), flows_.end(), [](auto const& a, auto const& b) { return a.second.used < b.second.used; });
            evicted = std::move(oldest->second.flow);
            flows_.erase(oldest);
        }
        flows_[pair.key] = Entry{std::move(flow), ++uses_};
        // A device flow is freed on this thread's stream, not under the lock the playout takes.
        lock.unlock();
        evicted = {};
        lock.lock();
    }
}
} // namespace replay
