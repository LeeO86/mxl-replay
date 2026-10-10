// Previews (SPECIFICATION.md §8.6), widgets (§8.7), the playout resync counter and the audio stall watch.
#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "config/config.hpp"
#include "media/mosaic.hpp"
#include "media/v210.hpp"
#include "mxl/io.hpp"
#include "ops/api.hpp"
#include "ops/preview.hpp"
#include "playout/scheduler.hpp"
#include "util/json.hpp"
#include "version.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace replay;

namespace
{
std::map<std::string, std::string> settings(std::map<std::string, std::string> extra = {})
{
    std::map<std::string, std::string> env{{"REPLAY_FORMAT", "64x32p50"}, {"REPLAY_INPUTS", "2"}, {"REPLAY_CHANNELS", "2"}, {"REPLAY_BUFFER_HOURS", "0.01"},
        {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}};
    for (auto const& [key, value] : extra)
    {
        env[key] = value;
    }
    return env;
}

Config configOf(std::map<std::string, std::string> extra = {})
{
    return loadConfig(settings(std::move(extra)), {}).config;
}

// An engine on its own directory; webrtc mode when `mode` says so.
std::unique_ptr<Engine> engineIn(char const* name, std::map<std::string, std::string> extra = {})
{
    auto const dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    extra["REPLAY_STORAGE_DIR"] = (dir / "media").string();
    extra["REPLAY_STATE_DIR"] = (dir / "state").string();
    auto const loaded = loadConfig(settings(extra), {});
    auto engine = std::make_unique<Engine>(loaded.config, loaded.flat);
    engine->openBuffer();
    return engine;
}

HttpResponse get(Engine& engine, std::string path, std::string query = {}, std::map<std::string, std::string> headers = {})
{
    HttpRequest request;
    request.method = "GET";
    request.path = std::move(path);
    request.query = std::move(query);
    request.headers = std::move(headers);
    return handleApi(engine, request, "<html>replay</html>");
}

std::string header(HttpResponse const& response, std::string const& name)
{
    for (auto const& [key, value] : response.headers)
    {
        if (key == name)
        {
            return value;
        }
    }
    return {};
}

// The luma of canvas pixel x, y and the CbCr pair of its 2×2 block.
struct Pixel
{
    int y;
    int cb;
    int cr;
};
Pixel pixelAt(std::vector<std::uint8_t> const& luma, std::vector<std::uint8_t> const& chroma, int width, int x, int y)
{
    auto const pair = static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x & ~1);
    return {luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)], chroma[pair], chroma[pair + 1]};
}
} // namespace

TEST_CASE("preview mode: jpeg by default, webrtc with the own or a shared MediaMTX, never both")
{
    auto const jpeg = configOf();
    CHECK(jpeg.previewMode == "jpeg");
    CHECK_FALSE(previewPlan(jpeg).webrtc);
    CHECK_FALSE(previewPlan(jpeg).ownMediamtx);

    auto const own = previewPlan(configOf({{"REPLAY_PREVIEW_MODE", "WebRTC"}}));
    CHECK(own.webrtc);
    CHECK(own.ownMediamtx);
    CHECK(own.path == "mxl-replay/mosaic");
    CHECK(own.publishUrl == "rtsp://127.0.0.1:8854/mxl-replay/mosaic");

    auto const sharedConfig = configOf({{"REPLAY_PREVIEW_MODE", "webrtc"}, {"PREVIEW_PUBLISH_URL", "rtsp://mediamtx.mxl:8554/"}, {"PREVIEW_PATH_PREFIX", "/test-all/rp1/"}});
    auto const shared = previewPlan(sharedConfig);
    CHECK(shared.webrtc);
    CHECK_FALSE(shared.ownMediamtx);
    CHECK(shared.publishUrl == "rtsp://mediamtx.mxl:8554/test-all/rp1/mosaic");
    CHECK(previewWhepUrl(sharedConfig) == "http://10.4.4.4:8689/test-all/rp1/mosaic/whep");
    auto const publicUrls = configOf({{"PREVIEW_WHEP_URL", "https://whep.example/"}, {"PREVIEW_HLS_URL", "https://hls.example"}});
    CHECK(previewWhepUrl(publicUrls) == "https://whep.example/mxl-replay/mosaic/whep");
    CHECK(previewHlsUrl(publicUrls) == "https://hls.example/mxl-replay/mosaic/index.m3u8");

    CHECK_THROWS_AS(configOf({{"REPLAY_PREVIEW_MODE", "both"}}), ConfigError);
    CHECK_THROWS_AS(configOf({{"PREVIEW_PUBLISH_URL", "http://mediamtx:8554"}}), ConfigError);
    CHECK_THROWS_AS(configOf({{"PREVIEW_PUBLISH_URL", "rtsp://user:pw@mediamtx:8554"}}), ConfigError);
    CHECK_THROWS_AS(configOf({{"PREVIEW_PUBLISH_URL", "rtsp://mediamtx:8554/path"}}), ConfigError);
    CHECK_THROWS_AS(configOf({{"PREVIEW_PATH_PREFIX", "a/../b"}}), ConfigError);
    CHECK_THROWS_AS(configOf({{"WIDGET_FRAME_ANCESTORS", "'self'; script-src *"}}), ConfigError);
    // The own MediaMTX's ports may not take the web or NMOS ports; a shared one binds nothing here.
    CHECK_THROWS_AS(configOf({{"REPLAY_PREVIEW_MODE", "webrtc"}, {"MEDIAMTX_WHEP_PORT", "8150"}}), ConfigError);
    CHECK_THROWS_AS(configOf({{"REPLAY_PREVIEW_MODE", "webrtc"}, {"MEDIAMTX_HLS_PORT", "3303"}}), ConfigError);
    CHECK_NOTHROW(configOf({{"REPLAY_PREVIEW_MODE", "webrtc"}, {"MEDIAMTX_WHEP_PORT", "8150"}, {"PREVIEW_PUBLISH_URL", "rtsp://shared:8554"}}));

    CHECK(mosaicStep(configOf({{"REPLAY_FORMAT", "1080p50"}})) == 2);
    CHECK(mosaicStep(configOf({{"REPLAY_FORMAT", "1080p25"}})) == 1);

    auto const yml = renderMediamtxConfig(configOf({{"REPLAY_PREVIEW_MODE", "webrtc"}}));
    CHECK(yml.find("rtspAddress: 127.0.0.1:8854\n") != std::string::npos);
    CHECK(yml.find("webrtcAddress: :8689\n") != std::string::npos);
    CHECK(yml.find("hlsAddress: :8688\n") != std::string::npos);
    CHECK(yml.find("webrtcLocalUDPAddress: :8489\n") != std::string::npos);
    CHECK(yml.find("webrtcAdditionalHosts: [\"10.4.4.4\"]\n") != std::string::npos);
    CHECK(yml.find("api: false\n") != std::string::npos);
}

TEST_CASE("never both: the WebRTC mode makes no camera or channel JPEG")
{
    auto jpeg = engineIn("mxl-replay-preview-jpeg");
    Frame10 frame;
    frame.allocate(64, 32);
    frame.fill(400, 512, 512);
    jpeg->ingestVideo(1, 1, 1000 * 1000000000ull, frame);
    (void)jpeg->render(1, 1000 * 1000000000ull);
    CHECK(get(*jpeg, "/api/v1/channels/1/preview.jpg").status == 200);
    CHECK(get(*jpeg, "/api/v1/cameras/1/preview.jpg").status == 200);
    CHECK(get(*jpeg, "/api/v1/preview/map").body.find("\"mode\":\"jpeg\"") != std::string::npos);

    auto webrtc = engineIn("mxl-replay-preview-webrtc", {{"REPLAY_PREVIEW_MODE", "webrtc"}});
    webrtc->ingestVideo(1, 1, 1000 * 1000000000ull, frame);
    (void)webrtc->render(1, 1000 * 1000000000ull);
    CHECK(get(*webrtc, "/api/v1/channels/1/preview.jpg").status == 404);
    CHECK(get(*webrtc, "/api/v1/cameras/1/preview.jpg").status == 404);
    CHECK(get(*webrtc, "/api/v1/preview/map").body.find("\"mode\":\"webrtc\"") != std::string::npos);
}

TEST_CASE("the tile map: channels above cameras, even edges, within 1920x1080")
{
    auto const layout = mosaicLayout(configOf({{"REPLAY_INPUTS", "4"}}));
    CHECK(layout.width == 1920);
    CHECK(layout.height == 630);
    REQUIRE(layout.tiles.size() == 6);
    CHECK(layout.tiles[0].id == "ch1");
    CHECK(layout.tiles[0].label == "PGM");
    CHECK(layout.tiles[1].id == "ch2");
    CHECK((layout.tiles[1].x == 640 && layout.tiles[1].y == 0 && layout.tiles[1].w == 640 && layout.tiles[1].h == 360));
    CHECK(layout.tiles[2].id == "cam1");
    CHECK(layout.tiles[2].kind == "camera");
    CHECK((layout.tiles[2].x == 0 && layout.tiles[2].y == 360 && layout.tiles[2].w == 480 && layout.tiles[2].h == 270));
    CHECK((layout.tiles[5].id == "cam4" && layout.tiles[5].x == 1440));

    auto const small = mosaicLayout({"PGM"}, {"Cam 1"});
    CHECK(small.width == 640);
    CHECK(small.height == 630);

    // The most the replay takes: 8 channels and 12 cameras.
    std::vector<std::string> channels(8, "ch");
    std::vector<std::string> cameras(12, "cam");
    auto const big = mosaicLayout(channels, cameras);
    CHECK(big.width <= 1920);
    CHECK(big.height <= 1080);
    CHECK(big.width % 2 == 0);
    CHECK(big.height % 2 == 0);
    for (std::size_t i = 0; i < big.tiles.size(); ++i)
    {
        auto const& a = big.tiles[i];
        CHECK((a.x % 2 == 0 && a.y % 2 == 0 && a.w % 2 == 0 && a.h % 2 == 0));
        CHECK(a.x + a.w <= big.width);
        CHECK(a.y + a.h <= big.height);
        for (std::size_t j = i + 1; j < big.tiles.size(); ++j)
        {
            auto const& b = big.tiles[j];
            CHECK((a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y));
        }
    }

    Json map;
    REQUIRE(parseJson(previewMapJson(configOf({{"REPLAY_PREVIEW_MODE", "webrtc"}})), map));
    CHECK(map.get("mode")->text == "webrtc");
    CHECK(map.get("width")->text == "1280");
    CHECK(map.get("height")->text == "630");
    CHECK(map.get("fps")->text == "25");
    CHECK(map.get("path")->text == "mxl-replay/mosaic");
    CHECK(map.get("whep")->text == "http://10.4.4.4:8689/mxl-replay/mosaic/whep");
    REQUIRE(map.get("tiles")->items.size() == 4);
    CHECK(map.get("tiles")->items[3].get("id")->text == "cam2");
    CHECK(map.get("tiles")->items[3].get("y")->text == "360");
}

TEST_CASE("the mosaic canvas takes each tile from its picture")
{
    PreviewMosaic mosaic(mosaicLayout({"PGM"}, {"Cam 1"}), false);
    REQUIRE_FALSE(mosaic.onDevice());
    auto const& layout = mosaic.layout();
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(layout.width * layout.height));
    std::vector<std::uint8_t> chroma(luma.size() / 2);
    REQUIRE(mosaic.copyTo(luma.data(), layout.width, chroma.data(), layout.width, false));
    auto const black = pixelAt(luma, chroma, layout.width, 10, 10);
    CHECK((black.y == 16 && black.cb == 128 && black.cr == 128));
    CHECK_FALSE(mosaic.copyTo(luma.data(), layout.width, chroma.data(), layout.width, true));

    // Camera 1 (tile 1) from a v210 grain: 10-bit values, 8 bits in the canvas.
    Frame10 frame;
    frame.allocate(96, 54);
    frame.fill(400, 300, 600);
    std::vector<std::uint8_t> v210(v210Size(96, 54));
    packV210(frame, v210.data(), 0);
    mosaic.drawV210(1, v210.data(), 96, 54);
    // The channel (tile 0) from 10-bit planes.
    frame.fill(800, 512, 512);
    mosaic.drawFrame(0, frame);
    mosaic.drawV210(7, v210.data(), 96, 54); // no such tile: ignored
    REQUIRE(mosaic.copyTo(luma.data(), layout.width, chroma.data(), layout.width, false));
    auto const& cam = layout.tiles[1];
    for (auto const& [x, y] : std::vector<std::pair<int, int>>{{cam.x, cam.y}, {cam.x + cam.w - 1, cam.y + cam.h - 1}, {cam.x + 101, cam.y + 37}})
    {
        auto const p = pixelAt(luma, chroma, layout.width, x, y);
        CHECK((p.y == 100 && p.cb == 75 && p.cr == 150));
    }
    auto const channel = pixelAt(luma, chroma, layout.width, 320, 180);
    CHECK((channel.y == 200 && channel.cb == 128 && channel.cr == 128));
    // Right of the camera row is still black.
    CHECK(pixelAt(luma, chroma, layout.width, cam.x + cam.w + 4, cam.y + 4).y == 16);
}

TEST_CASE("the engine draws its channels and cameras into the mosaic on every second grain at 50p")
{
    auto engine = engineIn("mxl-replay-preview-draw", {{"REPLAY_PREVIEW_MODE", "webrtc"}});
    PreviewMosaic mosaic(mosaicLayout(engine->config()), false);
    engine->setPreviewMosaic(&mosaic);
    auto const& layout = mosaic.layout();
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(layout.width * layout.height));
    std::vector<std::uint8_t> chroma(luma.size() / 2);
    auto const grain = [](std::uint64_t index) { return index * 20000000ull; };
    Frame10 frame;
    frame.allocate(64, 32);
    // Grain 1001 is not drawn (odd), grain 1002 is.
    frame.fill(400, 512, 512);
    engine->ingestVideo(2, 1, grain(1001), frame);
    REQUIRE(mosaic.copyTo(luma.data(), layout.width, chroma.data(), layout.width, false));
    auto const& cam2 = layout.tiles[3];
    CHECK(pixelAt(luma, chroma, layout.width, cam2.x + 8, cam2.y + 8).y == 16);
    frame.fill(600, 512, 512);
    engine->ingestVideo(2, 1, grain(1002), frame);
    REQUIRE(mosaic.copyTo(luma.data(), layout.width, chroma.data(), layout.width, false));
    CHECK(pixelAt(luma, chroma, layout.width, cam2.x + 8, cam2.y + 8).y == 150);
    // Channel 2 shows camera 2 paused on that frame (stored as JPEG: within 2 of it).
    engine->setAngle(2, 2);
    engine->setPosition(2, grain(1002));
    (void)engine->render(2, grain(1004));
    REQUIRE(mosaic.copyTo(luma.data(), layout.width, chroma.data(), layout.width, false));
    auto const& ch2 = layout.tiles[1];
    CHECK(std::abs(pixelAt(luma, chroma, layout.width, ch2.x + 8, ch2.y + 8).y - 150) <= 2);
    engine->setPreviewMosaic(nullptr);
}

TEST_CASE("the status and the metrics show the preview mode and the publish state")
{
    auto jpeg = engineIn("mxl-replay-preview-status-jpeg");
    CHECK(jpeg->statusJson().find("\"preview\":{\"mode\":\"jpeg\"}") != std::string::npos);
    CHECK(get(*jpeg, "/metrics").body.find("mxl_replay_preview_mode{mode=\"jpeg\"} 1") != std::string::npos);

    auto webrtc = engineIn("mxl-replay-preview-status", {{"REPLAY_PREVIEW_MODE", "webrtc"}});
    webrtc->setPreviewStatus([] {
        PreviewStatus status;
        status.state = "publishing";
        status.encoder = "x264";
        status.frames = 42;
        status.mediamtxRunning = true;
        return status;
    });
    Json status;
    REQUIRE(parseJson(webrtc->statusJson(), status));
    auto const* preview = status.get("preview");
    REQUIRE(preview != nullptr);
    CHECK(preview->get("mode")->text == "webrtc");
    CHECK(preview->get("publish")->text == "own");
    CHECK(preview->get("publish_url")->text == "rtsp://127.0.0.1:8854");
    CHECK(preview->get("path")->text == "mxl-replay/mosaic");
    CHECK(preview->get("state")->text == "publishing");
    CHECK(preview->get("encoder")->text == "x264");
    CHECK(preview->get("mediamtx")->get("running")->boolean);
    REQUIRE(preview->get("streams")->items.size() == 1);
    CHECK(preview->get("streams")->items[0].get("state")->text == "publishing");
    auto const metrics = get(*webrtc, "/metrics").body;
    CHECK(metrics.find("mxl_replay_preview_mode{mode=\"webrtc\"} 1") != std::string::npos);
    CHECK(metrics.find("mxl_replay_preview_publish_mode{mode=\"own\"} 1") != std::string::npos);
    CHECK(metrics.find("mxl_replay_preview_publish_state{state=\"publishing\",stream=\"mxl-replay/mosaic\"} 1") != std::string::npos);
    CHECK(metrics.find("mxl_replay_preview_publish_state{state=\"error\",stream=\"mxl-replay/mosaic\"} 0") != std::string::npos);
    CHECK(metrics.find("mxl_replay_preview_encoder{encoder=\"x264\"} 1") != std::string::npos);
    CHECK(metrics.find("mxl_replay_preview_frames_total 42") != std::string::npos);
}

TEST_CASE("widgets: the list with CORS for listed origins, the pages with CSP frame-ancestors")
{
    auto engine = engineIn("mxl-replay-widgets", {{"WIDGET_FRAME_ANCESTORS", "'self' https://designer.example"}});
    auto const list = get(*engine, "/widgets", {}, {{"origin", "https://designer.example"}});
    CHECK(list.status == 200);
    CHECK(header(list, "Access-Control-Allow-Origin") == "https://designer.example");
    CHECK(header(list, "Vary") == "Origin");
    CHECK(header(get(*engine, "/widgets", {}, {{"origin", "https://evil.example"}}), "Access-Control-Allow-Origin").empty());
    CHECK(header(get(*engine, "/widgets"), "Access-Control-Allow-Origin").empty());
    Json widgets;
    REQUIRE(parseJson(list.body, widgets));
    REQUIRE(widgets.items.size() == 2);
    auto const& transport = widgets.items[0];
    CHECK(transport.get("id")->text == "transport");
    CHECK(transport.get("min_size")->get("w")->text == "480");
    CHECK(transport.get("min_size")->get("h")->text == "160");
    CHECK(transport.get("params")->get("properties")->get("channel")->get("maximum")->text == "2");
    CHECK(transport.get("params")->get("required")->items[0].text == "channel");
    auto const& clips = widgets.items[1];
    CHECK(clips.get("id")->text == "clip-list");
    CHECK(clips.get("min_size")->get("w")->text == "400");
    CHECK(clips.get("params")->get("required") == nullptr);
    CHECK(clips.get("version")->text == REPLAY_VERSION);

    auto const page = get(*engine, "/widget/transport", "channel=2&theme=transparent");
    CHECK(page.status == 200);
    CHECK(page.body == "<html>replay</html>");
    CHECK(header(page, "Content-Security-Policy") == "frame-ancestors 'self' https://designer.example");
    CHECK(header(page, "X-Frame-Options").empty());
    CHECK(get(*engine, "/widget/clip-list").status == 200);
    CHECK(get(*engine, "/widget/clip-list", "channel=1&theme=light").status == 200);
    CHECK(get(*engine, "/widget/transport").status == 400);
    CHECK(get(*engine, "/widget/transport", "channel=3").status == 400);
    CHECK(get(*engine, "/widget/transport", "channel=x").status == 400);
    CHECK(get(*engine, "/widget/clip-list", "theme=pink").status == 400);
    CHECK(get(*engine, "/widget/tally").status == 404);
    // The app itself is not framed by others.
    CHECK(header(get(*engine, "/"), "Content-Security-Policy").empty());

    CHECK(listedOrigin("'self' https://A.example/", "https://a.example"));
    CHECK(listedOrigin("*", "http://x.example:8080"));
    CHECK_FALSE(listedOrigin("'self'", "null"));
    CHECK(configOf().widgetFrameAncestors == "'self'");
}

TEST_CASE("a channel more than two grains late resyncs once, counted and logged at most every 10 s")
{
    PlayoutGrid grid;
    bool resynced = false;
    CHECK(grid.take(100, resynced) == 100);
    CHECK_FALSE(resynced);
    CHECK(grid.take(101, resynced) == 101);
    CHECK(grid.take(103, resynced) == 102); // two late: catches up
    CHECK_FALSE(resynced);
    CHECK(grid.take(110, resynced) == 110); // seven late: jumps
    CHECK(resynced);
    CHECK(grid.lastJump == 7);
    CHECK(grid.resyncs == 1);
    CHECK(grid.take(111, resynced) == 111);
    CHECK_FALSE(resynced);
    CHECK(grid.take(105, resynced) == 105); // ahead of the clock (it went back): jumps back
    CHECK(resynced);
    CHECK(grid.lastJump == -7);
    CHECK(grid.resyncs == 2);

    std::uint64_t const s = 1000000000ull;
    CHECK(grid.toLog(100 * s) == 2);
    CHECK(grid.toLog(101 * s) == 0);
    CHECK(grid.take(120, resynced) == 120);
    CHECK(grid.toLog(105 * s) == 0); // within 10 s of the last line
    CHECK(grid.toLog(110 * s) == 1);
    CHECK(grid.toLog(130 * s) == 0); // nothing new
}

TEST_CASE("a camera's audio that stops while its video records is reported once, and when it is back")
{
    AudioWatch watch{3};
    std::uint64_t gap = 0;
    CHECK(watch.frame(true, &gap) == AudioWatch::Change::None);
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(false) == AudioWatch::Change::Stopped);
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(true, &gap) == AudioWatch::Change::Resumed);
    CHECK(gap == 5);
    // A short gap is not a stall.
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(true, &gap) == AudioWatch::Change::None);
    CHECK(gap == 1);
    // Not routed: nothing is watched.
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    watch.reset();
    CHECK(watch.limit == 3);
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(false) == AudioWatch::Change::None);
    CHECK(watch.frame(false) == AudioWatch::Change::Stopped);
}
