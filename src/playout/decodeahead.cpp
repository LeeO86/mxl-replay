#include "playout/decodeahead.hpp"

#include <algorithm>
#include <utility>

namespace replay
{
DecodeAhead::DecodeAhead(Decode decode, std::function<void()> const& start)
    : decode_(std::move(decode))
    , thread_([this, start] {
        if (start)
        {
            start();
        }
        run();
    })
{
}

DecodeAhead::~DecodeAhead()
{
    {
        std::lock_guard lock{mutex_};
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void DecodeAhead::plan(AheadFrame const* frames, int count)
{
    count = std::clamp(count, 0, kMaxPlan);
    {
        std::lock_guard lock{mutex_};
        if (count == planCount_ && std::equal(frames, frames + count, plan_.begin()))
        {
            return;
        }
        std::copy(frames, frames + count, plan_.begin());
        planCount_ = count;
    }
    wake_.notify_one();
}

int DecodeAhead::find(AheadFrame const& frame) const
{
    for (int i = 0; i < kSlots; ++i)
    {
        if (slots_[static_cast<std::size_t>(i)].state != State::Free && slots_[static_cast<std::size_t>(i)].frame == frame)
        {
            return i;
        }
    }
    return -1;
}

bool DecodeAhead::planned(AheadFrame const& frame) const
{
    return std::find(plan_.begin(), plan_.begin() + planCount_, frame) != plan_.begin() + planCount_;
}

DecodeAhead::Take DecodeAhead::take(AheadFrame const& frame, std::chrono::nanoseconds maxWait)
{
    Take taken;
    std::unique_lock lock{mutex_};
    int const index = find(frame);
    if (index < 0)
    {
        return taken;
    }
    auto& slot = slots_[static_cast<std::size_t>(index)];
    taken.planned = true;
    if (slot.state == State::Decoding)
    {
        auto const began = std::chrono::steady_clock::now();
        done_.wait_until(lock, began + maxWait, [&] { return slot.state != State::Decoding || !(slot.frame == frame); });
        taken.waited = std::chrono::steady_clock::now() - began;
    }
    if (!(slot.frame == frame))
    {
        return taken;
    }
    if (slot.state == State::Decoding)
    {
        taken.result = Result::Late;
        return taken;
    }
    if (slot.state == State::Ready)
    {
        slot.held = true;
        slot.used = ++uses_;
        taken.result = Result::Hit;
        taken.slot = index;
        taken.storedNs = slot.storedNs;
    }
    return taken;
}

void DecodeAhead::give(int slot)
{
    if (slot < 0 || slot >= kSlots)
    {
        return;
    }
    {
        std::lock_guard lock{mutex_};
        slots_[static_cast<std::size_t>(slot)].held = false;
    }
    wake_.notify_one();
}

void DecodeAhead::run()
{
    std::unique_lock lock{mutex_};
    while (!stop_)
    {
        // The first planned frame no slot has, into a free slot, else the least recently used one that is not
        // held and not planned.
        AheadFrame const* next = nullptr;
        for (int i = 0; i < planCount_ && next == nullptr; ++i)
        {
            if (find(plan_[static_cast<std::size_t>(i)]) < 0)
            {
                next = &plan_[static_cast<std::size_t>(i)];
            }
        }
        int target = -1;
        for (int i = 0; next != nullptr && i < kSlots; ++i)
        {
            auto const& slot = slots_[static_cast<std::size_t>(i)];
            if (slot.state == State::Free)
            {
                target = i;
                break;
            }
            if (slot.held || slot.state == State::Decoding || planned(slot.frame))
            {
                continue;
            }
            if (target < 0 || slot.used < slots_[static_cast<std::size_t>(target)].used)
            {
                target = i;
            }
        }
        if (target < 0)
        {
            wake_.wait(lock);
            continue;
        }
        auto& slot = slots_[static_cast<std::size_t>(target)];
        auto const frame = *next;
        slot.state = State::Decoding;
        slot.frame = frame;
        slot.storedNs = 0;
        lock.unlock();
        std::uint64_t storedNs = 0;
        bool const ok = decode_(frame, target, storedNs);
        lock.lock();
        slot.state = ok ? State::Ready : State::Failed;
        slot.storedNs = storedNs;
        slot.used = ++uses_;
        done_.notify_all();
    }
}
} // namespace replay
