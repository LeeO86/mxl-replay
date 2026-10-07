#include "app/engine.hpp"
#include "config/config.hpp"
#include "domain/scan.hpp"
#include "media/timebase.hpp"
#include "mxl/io.hpp"
#include "nmos/node.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <sys/resource.h>
#include <thread>
#include <vector>

#ifdef REPLAY_HAS_UI
#include "ops/webui_generated.hpp"
#endif

extern char** environ;

namespace
{
std::atomic<bool> gStop{false};
std::atomic<int> gSignal{0};

void onSignal(int signal)
{
    gSignal.store(signal);
    gStop.store(true);
}

std::map<std::string, std::string> environmentMap()
{
    std::map<std::string, std::string> env;
    for (char** cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor)
    {
        std::string entry(*cursor);
        auto const eq = entry.find('=');
        if (eq != std::string::npos)
        {
            env.emplace(entry.substr(0, eq), entry.substr(eq + 1));
        }
    }
    return env;
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::cout << "mxl-replay " << REPLAY_VERSION << "\nMXL pin " << REPLAY_MXL_REVISION << "\nUsage: mxl-replay [--config FILE]\n";
            return 0;
        }
    }
    // Every MXL flow keeps one descriptor per grain (50 for 1 s at 50p): four cameras
    // and four channels (video, audio, ANC each) pass Docker's default soft limit of
    // 1024, and further readers then fail with "Too many open files".
    rlimit files{};
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max)
    {
        files.rlim_cur = files.rlim_max;
        setrlimit(RLIMIT_NOFILE, &files);
    }
    auto env = environmentMap();
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc && env.count("REPLAY_CONFIG_FILE") == 0)
        {
            env["REPLAY_CONFIG_FILE"] = argv[++i];
        }
    }
    replay::setLogFormatJson(true);
    try
    {
        std::map<std::string, std::string> file;
        auto const configPath = env.count("REPLAY_CONFIG_FILE") ? env["REPLAY_CONFIG_FILE"] : std::string{};
        if (!configPath.empty())
        {
            file = replay::readConfigFile(configPath);
        }
        std::map<std::string, std::string> state;
        auto const stateDir = replay::stateDirectory(env, file);
        auto const statePath = stateDir + "/config.json";
        if (std::filesystem::is_regular_file(statePath))
        {
            state = replay::readConfigFile(statePath);
        }
        auto loaded = replay::loadConfig(env, file, state);
        replay::setLogLevel(replay::parseLogLevel(loaded.config.logLevel));
        replay::setLogFormatJson(loaded.config.logFormat != "text");
        auto const flat = loaded.flat;
        replay::Engine engine(std::move(loaded.config), flat);
        replay::HttpServer server;
        std::string indexHtml;
#ifdef REPLAY_HAS_UI
        indexHtml = std::string(replay::webui::indexHtml());
#endif
        // HTTP first: /livez answers while the retained segments are indexed (minutes for
        // hours of buffer); /readyz and the API answer 503 until the buffer is open.
        if (engine.config().webEnable)
        {
            server.start(engine.config().webPort, [&](replay::HttpRequest const& request) { return replay::handleApi(engine, request, indexHtml); });
            replay::logInfo("web_listen", {{"port", std::to_string(server.port())}});
        }
        engine.openBuffer();
        replay::NmosNode node(engine.config(), engine);
        node.start();
        replay::MxlBridge bridge(engine);
        bridge.start();
        std::signal(SIGTERM, onSignal);
        std::signal(SIGINT, onSignal);
        auto const period = std::chrono::nanoseconds(replay::framePeriodNs(engine.config().format.rateNum, engine.config().format.rateDen));
        // Each channel renders and writes on its own thread. One thread rendering every
        // channel in turn made four channels late while the GPU was far from busy.
        std::vector<std::thread> playout;
        for (int channel = 1; channel <= engine.config().channels; ++channel)
        {
            playout.emplace_back([&engine, &bridge, channel] {
                // One grain per house period on the TAI grid, rendered for the grain's own time.
                // Rendering for the wake-up time put live playout between two source frames
                // (interpolated, or flipping between them) and could write a grain index twice.
                auto const num = engine.config().format.rateNum;
                auto const den = engine.config().format.rateDen;
                std::uint64_t next = 0;
                while (!gStop.load())
                {
                    auto const current = replay::timestampToIndex(num, den, replay::taiNowNs());
                    // Start, or more than two grains late: continue at the current grain.
                    if (next == 0 || next + 2 < current || next > current + 1)
                    {
                        next = current;
                    }
                    auto const tai = replay::indexToTimestamp(num, den, next);
                    auto rendered = engine.render(channel, tai);
                    bridge.publish(channel, rendered, tai);
                    ++next;
                    auto const wait = static_cast<std::int64_t>(replay::indexToTimestamp(num, den, next)) - static_cast<std::int64_t>(replay::taiNowNs());
                    if (wait > 0)
                    {
                        std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
                    }
                }
            });
        }
        auto next = std::chrono::steady_clock::now();
        // The status goes to the UIs ten times a second; every frame was 50 messages a second
        // per browser for a UI that draws at 10 Hz.
        auto nextPush = next;
        std::uint64_t synthetic = 0;
        while (!gStop.load())
        {
            auto const tai = replay::taiNowNs();
            if (engine.config().synthetic)
            {
                for (auto const& camera : engine.config().cameras)
                {
                    replay::Frame10 frame;
                    frame.allocate(engine.config().format.width, engine.config().format.height);
                    frame.fill(static_cast<std::uint16_t>(64 + (synthetic + static_cast<std::uint64_t>(camera.index) * 40) % 800), 512, 512);
                    engine.ingestVideo(camera.index, 1, tai, std::move(frame));
                }
                ++synthetic;
            }
            if (engine.config().webEnable && std::chrono::steady_clock::now() >= nextPush)
            {
                server.broadcast(engine.statusJson());
                nextPush = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            }
            next += period;
            std::this_thread::sleep_until(next);
        }
        for (auto& thread : playout)
        {
            thread.join();
        }
        bridge.stop();
        bool const nmosStopped = node.stop();
        if (engine.config().cleanupOnExit)
        {
            std::string error;
            if (!replay::removeOwnDomain(engine.config().outputDomainDir, engine.config().outputDomainId, error))
            {
                replay::logError("mxl_cleanup_refused", {{"dir", engine.config().outputDomainDir}, {"error", error}});
            }
        }
        server.stop();
        if (gSignal.load() == SIGTERM)
        {
            if (!nmosStopped)
            {
                std::_Exit(143);
            }
            return 143;
        }
        return 0;
    }
    catch (replay::ConfigError const& ex)
    {
        replay::logError("config", {{"error", ex.what()}});
        return 78;
    }
    catch (replay::StartupError const& ex)
    {
        replay::logError("startup", {{"error", ex.what()}});
        return ex.code();
    }
    catch (std::exception const& ex)
    {
        replay::logError("fatal", {{"error", ex.what()}});
        return 75;
    }
}
