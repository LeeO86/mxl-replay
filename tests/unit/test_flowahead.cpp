// Flows computed ahead (SPECIFICATION.md §5.3): the pairs the next grains need, the worker, and the playout
// that blends a grain whose flow is not ready and counts it.
#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "playout/flowahead.hpp"
#include "playout/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace replay;

namespace
{
constexpr std::int64_t kPeriod = 20000000;

// A 50p scheduler `frames` source frames in, at `speed` without a ramp.
Scheduler schedulerAt(double frames, double speed, bool playing = true)
{
    Scheduler scheduler;
    scheduler.positionNs = static_cast<std::int64_t>(frames * kPeriod);
    scheduler.setTargetSpeed(speed, 0);
    scheduler.playing = playing;
    return scheduler;
}

// Frame A of a pair (B is the next) and the last grain that uses it.
struct Expected
{
    std::int64_t frameA;
    int last;
};

void checkPairs(std::vector<PairAhead> const& pairs, std::vector<Expected> const& expected)
{
    REQUIRE(pairs.size() == expected.size());
    for (std::size_t i = 0; i < pairs.size(); ++i)
    {
        CAPTURE(i);
        CHECK(pairs[i].frameA == expected[i].frameA);
        CHECK(pairs[i].frameB == expected[i].frameA + 1);
        CHECK(pairs[i].last == expected[i].last);
    }
}

std::string metric(Engine& engine, std::string const& series)
{
    auto const text = engine.metrics().render();
    auto const at = text.find("mxl_replay_" + series + " ");
    return at == std::string::npos ? std::string{} : text.substr(at + series.size() + 12, text.find('\n', at) - at - series.size() - 12);
}
} // namespace

TEST_CASE("pairs ahead at 25, 50 and 75 % and in reverse: each pair once, in order, with its last grain")
{
    // 25 %: 0.25, 0.5, 0.75 use pair 0-1, 1.0 is a real frame, 1.25 … 1.75 use 1-2.
    checkPairs(pairsAhead(schedulerAt(0, 0.25), 1, 0.1, 8), {{0, 3}, {1, 7}});
    checkPairs(pairsAhead(schedulerAt(0, 0.5), 1, 0.1, 8), {{0, 1}, {1, 3}, {2, 5}, {3, 7}});
    checkPairs(pairsAhead(schedulerAt(0, 0.75), 1, 0.1, 8), {{0, 1}, {1, 2}, {2, 3}, {3, 5}, {4, 6}, {5, 7}});
    checkPairs(pairsAhead(schedulerAt(10, -0.5), 1, 0.1, 6), {{9, 1}, {8, 3}, {7, 5}});
    // 100 % on a house-rate camera needs no flow, nor 33 % on a 3× HFR camera (real frames).
    CHECK(pairsAhead(schedulerAt(0, 1), 1, 0.1, 8).empty());
    CHECK(pairsAhead(schedulerAt(0, 1.0 / 3.0), 3, 0.1, 8).empty());
}

TEST_CASE("pairs ahead follow a speed ramp, and a paused channel keeps its pair and prepares the play")
{
    auto ramp = schedulerAt(0, 1);
    ramp.setTargetSpeed(0.5, 3); // 83 %, 67 %, then 50 %: 0.83, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5
    checkPairs(pairsAhead(ramp, 1, 0.1, 8), {{0, 1}, {1, 2}, {2, 4}, {3, 6}, {4, 8}});
    // Paused on 10.5: that pair for every grain ahead; from the second grain on as if it played at 50 %.
    checkPairs(pairsAhead(schedulerAt(10.5, 0.5, false), 1, 0.1, 8), {{10, 8}, {11, 3}, {12, 5}, {13, 7}});
    // The angle is in the key, the pairs are the same.
    CHECK(flowKey(1, 40000000, 60000000) == "1:40000000:60000000");
    CHECK(flowKey(2, 40000000, 60000000) != flowKey(1, 40000000, 60000000));
}

TEST_CASE("the flow worker computes the plan in order, follows a new plan and skips a pair it cannot finish in time")
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> computed;
    bool hold = true;
    auto const later = [](int ms) { return FlowAhead::Clock::now() + std::chrono::milliseconds(ms); };
    auto const pair = [](std::string key, FlowAhead::Clock::time_point due) {
        FlowAhead::Pair p;
        p.key = std::move(key);
        p.due = due;
        return p;
    };
    auto const waitFor = [&](std::size_t count) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return computed.size() >= count; });
    };
    {
        // The first flow waits until the test lets it go; a "slow…" one takes 30 ms.
        FlowAhead ahead([&](FlowAhead::Pair const& p) {
            std::unique_lock lock{mutex};
            changed.wait(lock, [&] { return !hold || p.key != "a1"; });
            lock.unlock();
            if (p.key.rfind("slow", 0) == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            lock.lock();
            computed.push_back(p.key);
            changed.notify_all();
            AheadFlow flow;
            flow.cpu = std::make_shared<FlowPair const>();
            return flow;
        });
        ahead.plan({pair("a1", later(1000)), pair("a2", later(2000))});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(ahead.find("a1").cpu); // not ready: never waited for
        // The angle switched: a2 is not wanted any more.
        ahead.plan({pair("b1", later(1000))});
        {
            std::lock_guard lock{mutex};
            hold = false;
        }
        changed.notify_all();
        REQUIRE(waitFor(2));
        CHECK(ahead.find("a1").cpu);
        CHECK(ahead.find("b1").cpu);
        CHECK_FALSE(ahead.find("a2").cpu);
        // One slow flow (a1 too) does not make the worker skip a pair due soon.
        ahead.plan({pair("slow1", later(1000))});
        REQUIRE(waitFor(3));
        ahead.plan({pair("soon", later(5))});
        REQUIRE(waitFor(4));
        // After three slow ones a flow takes 30 ms: a pair due in 5 ms is skipped for the one after it (and
        // tried again a second after the last flow).
        ahead.plan({pair("slow2", later(1000)), pair("slow3", later(1000)), pair("slow4", later(1000))});
        REQUIRE(waitFor(7));
        ahead.plan({pair("late", later(5)), pair("slow5", later(1000))});
        REQUIRE(waitFor(8));
        std::lock_guard lock{mutex};
        std::string order;
        for (auto const& key : computed)
        {
            order += key + " ";
        }
        CHECK(order == "a1 b1 slow1 soon slow2 slow3 slow4 slow5 ");
    }
}

TEST_CASE("a grain whose flow is not ready is blended and counted; with the flow it interpolates, also after an angle switch")
{
    auto const dir = std::filesystem::temp_directory_path() / "mxl-replay-flowahead";
    std::filesystem::remove_all(dir);
    std::map<std::string, std::string> env{{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "2"}, {"REPLAY_CHANNELS", "2"}, {"REPLAY_BUFFER_HOURS", "0.01"},
        {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()}, {"REPLAY_STORAGE_MIN_MBPS", "0"},
        {"REPLAY_ALLOW_CPU_INTERP", "true"}, {"REPLAY_RAMP_FRAMES", "0"}, {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}};
    {
        Engine engine(loadConfig(env, {}).config);
        engine.openBuffer();
        CHECK(metric(engine, "interpolation_fallbacks_total{channel=\"1\"}") == "0");
        CHECK(metric(engine, "interpolation_fallbacks_total{channel=\"2\"}") == "0");
        for (int i = 0; i < 16; ++i)
        {
            for (int camera = 1; camera <= 2; ++camera)
            {
                Frame10 frame;
                frame.allocate(64, 32);
                frame.fill(static_cast<std::uint16_t>(100 + i * 40 + camera), 400, 600);
                engine.ingestVideo(camera, 1, static_cast<std::uint64_t>(i) * kPeriod, std::move(frame));
            }
        }
        // The worker has time for every pair it is given: no pair is late.
        auto const settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };
        engine.setPosition(1, 2 * kPeriod);
        engine.setSpeed(1, 0.5);
        engine.play(1);
        (void)engine.render(1, 0); // 2.5: no flow was asked for before this grain
        CHECK(metric(engine, "interpolation_fallbacks_total{channel=\"1\"}") == "1");
        settle();
        (void)engine.render(1, kPeriod); // 3.0, a real frame
        (void)engine.render(1, 2 * kPeriod); // 3.5: computed ahead
        CHECK(metric(engine, "flow_cache_hits_total{channel=\"1\"}") == "1");
        settle();
        engine.setSpeed(1, 0.25);
        engine.setAngle(1, 2);
        // 3.75 on camera 2: not asked for before this grain either. The plan is camera 2's now.
        (void)engine.render(1, 3 * kPeriod);
        CHECK(metric(engine, "interpolation_fallbacks_total{channel=\"1\"}") == "2");
        settle();
        (void)engine.render(1, 4 * kPeriod); // 4.0
        CHECK(engine.render(1, 5 * kPeriod).camera == 2); // 4.25 on camera 2: computed ahead
        CHECK(metric(engine, "flow_cache_hits_total{channel=\"1\"}") == "2");
        CHECK(metric(engine, "interpolation_fallbacks_total{channel=\"1\"}") == "2");
    }
    std::filesystem::remove_all(dir);
}
