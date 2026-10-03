#include "app/engine.hpp"
#include "config/config.hpp"
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
#include <cstring>
#include <iostream>
#include <map>
#include <thread>

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
        auto loaded = replay::loadConfig(env, file);
        replay::setLogLevel(replay::parseLogLevel(loaded.config.logLevel));
        replay::setLogFormatJson(loaded.config.logFormat != "text");
        replay::Engine engine(std::move(loaded.config));
        replay::NmosNode node(engine.config(), engine);
        node.start();
        replay::MxlBridge bridge(engine);
        bridge.start();
        replay::HttpServer server;
        std::string indexHtml;
#ifdef REPLAY_HAS_UI
        indexHtml = std::string(replay::webui::indexHtml());
#endif
        if (engine.config().webEnable)
        {
            server.start(engine.config().webPort, [&](replay::HttpRequest const& request) { return replay::handleApi(engine, request, indexHtml); });
            replay::logInfo("web_listen", {{"port", std::to_string(server.port())}});
        }
        std::signal(SIGTERM, onSignal);
        std::signal(SIGINT, onSignal);
        auto const period = std::chrono::nanoseconds(replay::framePeriodNs(engine.config().format.rateNum, engine.config().format.rateDen));
        auto next = std::chrono::steady_clock::now();
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
            for (int channel = 1; channel <= engine.config().channels; ++channel)
            {
                auto rendered = engine.render(channel, tai);
                bridge.publish(channel, rendered, tai);
            }
            if (engine.config().webEnable)
            {
                server.broadcast(engine.statusJson());
            }
            next += period;
            std::this_thread::sleep_until(next);
        }
        bridge.stop();
        node.stop();
        server.stop();
        if (gSignal.load() == SIGTERM)
        {
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
