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

void DecodeAhead::plan(AheadFrame const* frames, int count, AheadFrame const* keep, int keepCount)
{
    count = std::clamp(count, 0, kMaxPlan);
    keepCount = keep == nullptr ? 0 : std::clamp(keepCount, 0, kMaxKeep);
    {
        std::lock_guard lock{mutex_};
        std::copy(keep, keep + keepCount, keep_.begin());
        keepCount_ = keepCount;
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
    return std::find(plan_.begin(), plan_.begin() + planCount_, frame) != plan_.begin() + planCount_ ||
           std::find(keep_.begin(), keep_.begin() + keepCount_, frame) != keep_.begin() + keepCount_;
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
        slot.taken = true;
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
        // The first planned frame no slot has, into a free slot, else one that is not held, planned or kept: a
        // failed or taken one before one not taken yet, the least recently used first.
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
            auto const rank = [](Slot const& s) { return s.state == State::Failed ? 0 : s.taken ? 1 : 2; };
            auto const& best = slots_[static_cast<std::size_t>(target < 0 ? i : target)];
            if (target < 0 || rank(slot) < rank(best) || (rank(slot) == rank(best) && slot.used < best.used))
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
        slot.taken = false;
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
DecodePlan planAhead(AheadSource const* sources, int count, AheadFrame const* next, int nextCount, int grains)
{
    DecodePlan plan;
    auto const add = [&](AheadFrame const& frame) {
        auto const end = plan.frames.begin() + plan.count;
        if (plan.count < DecodeAhead::kMaxPlan && std::find(plan.frames.begin(), end, frame) == end)
        {
            plan.frames[static_cast<std::size_t>(plan.count++)] = frame;
        }
    };
    auto const keep = [&](AheadFrame const& frame) {
        auto const end = plan.keep.begin() + plan.keepCount;
        if (plan.keepCount < DecodeAhead::kMaxKeep && std::find(plan.keep.begin(), end, frame) == end)
        {
            plan.keep[static_cast<std::size_t>(plan.keepCount++)] = frame;
        }
    };
    auto const between = [](SourcePick const& pick) { return pick.kind == SourcePick::Kind::Blend || pick.kind == SourcePick::Kind::Interpolate; };
    for (int i = 0; i < count; ++i)
    {
        auto const& source = sources[i];
        if (!source.withB && between(source.current))
        {
            continue; // the grain decodes no single picture to keep
        }
        keep({source.camera, static_cast<std::uint64_t>(std::max<std::int64_t>(0, source.current.frameA)) * source.periodNs});
        if (between(source.current))
        {
            keep({source.camera, static_cast<std::uint64_t>(std::max<std::int64_t>(0, source.current.frameB)) * source.periodNs});
        }
    }
    // Grain by grain: framesAhead for g grains repeats the frames of the grains before g in the same order.
    int most = grains;
    for (int i = 0; i < count; ++i)
    {
        most = std::max(most, sources[i].grains);
    }
    for (int grain = 1; grain <= most; ++grain)
    {
        for (int i = 0; i < count; ++i)
        {
            auto const& source = sources[i];
            if (grain > (source.grains > 0 ? source.grains : grains))
            {
                continue;
            }
            auto const ahead = framesAhead(source.scheduler, source.current, source.hfrFactor, source.hfrSnap, source.motion, source.withB, grain);
            for (int k = 0; k < ahead.count; ++k)
            {
                auto const frame = ahead.frames[static_cast<std::size_t>(k)];
                auto const tai = static_cast<std::uint64_t>(frame) * source.periodNs;
                if (frame >= 0 && tai <= source.untilNs)
                {
                    add({source.camera, tai});
                }
            }
        }
        if (grain == 1)
        {
            for (int k = 0; k < nextCount; ++k)
            {
                add(next[k]);
            }
        }
    }
    return plan;
}
} // namespace replay
