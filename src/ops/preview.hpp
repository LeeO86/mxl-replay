#pragma once

#include "config/config.hpp"
#include "media/mosaic.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace replay
{
// What runs for the UI previews (SPECIFICATION.md §8.6). Never both: REPLAY_PREVIEW_MODE=jpeg makes
// JPEG pictures on request and publishes nothing; webrtc draws, encodes and publishes the mosaic and
// makes no camera or channel JPEG.
struct PreviewPlan
{
    bool webrtc = false;
    // webrtc without PREVIEW_PUBLISH_URL: this process runs its own MediaMTX.
    bool ownMediamtx = false;
    // The mosaic's MediaMTX path, <PREVIEW_PATH_PREFIX>/mosaic.
    std::string path;
    // PREVIEW_PUBLISH_URL, or rtsp://127.0.0.1:MEDIAMTX_RTSP_PORT.
    std::string publishBase;
    // <publishBase>/<path>.
    std::string publishUrl;
};
[[nodiscard]] PreviewPlan previewPlan(Config const& cfg);

// <PREVIEW_WHEP_URL>/<path>/whep and <PREVIEW_HLS_URL>/<path>/index.m3u8, or the own MediaMTX on
// NMOS_HOST_ADDRESS when the setting is empty (the page then puts in its own host name).
[[nodiscard]] std::string previewWhepUrl(Config const& cfg);
[[nodiscard]] std::string previewHlsUrl(Config const& cfg);

// Tile `channels + camera - 1` is camera `camera`, tile `channel - 1` channel `channel`.
[[nodiscard]] MosaicLayout mosaicLayout(Config const& cfg);

// The mosaic is drawn and encoded on every `step`-th house grain: up to 30 pictures a second (25 at 50p).
[[nodiscard]] int mosaicStep(Config const& cfg);

// The built-in MediaMTX (own mode): RTSP ingest on 127.0.0.1:MEDIAMTX_RTSP_PORT (TCP only), WHEP on
// MEDIAMTX_WHEP_PORT with ICE on MEDIAMTX_ICE_UDP_PORT (UDP and TCP) and NMOS_HOST_ADDRESS as ICE host,
// low-latency HLS on MEDIAMTX_HLS_PORT; no API, metrics, RTMP, SRT or MoQ. Adapted from
// mxl-webrtc-monitor 1.3.0 src/ops/mediamtx.cpp (MIT, Copyright (c) 2026 Adrian Hilber).
[[nodiscard]] std::string renderMediamtxConfig(Config const& cfg);

// The state of the mosaic's RTSP publish, as /statusz and the metrics show it.
struct PreviewStatus
{
    // "connecting" until MediaMTX took the stream, "publishing" while packets go out, "error" after a
    // failure (with `error`) until it publishes again.
    std::string state = "connecting";
    std::string error;
    // "nvenc" or "x264" once an encoder is open.
    std::string encoder;
    std::uint64_t frames = 0;
    // The built-in MediaMTX (own mode).
    bool mediamtxRunning = false;
    std::uint64_t mediamtxRestarts = 0;
};

// The "preview" object of /statusz.
[[nodiscard]] std::string previewStatusJson(Config const& cfg, PreviewStatus const& status);
// GET /api/v1/preview/map: the mode, the canvas, where the page plays it, and every tile.
[[nodiscard]] std::string previewMapJson(Config const& cfg);
// One query parameter of a widget (SPECIFICATION.md §8.7): `channel` (1..REPLAY_CHANNELS), a `boolean`
// (true, false, 1 or 0) or a `playlist` id.
struct WidgetParam
{
    std::string name;
    std::string kind; // "channel", "boolean" or "playlist"
    std::string title;
    bool required = false;
    bool defaultTrue = false; // a boolean's default
};
// The one list of widgets that GET /widgets answers and GET /widget/<id> accepts.
struct WidgetSpec
{
    std::string id;
    std::string title;
    int minW = 0;
    int minH = 0;
    std::vector<WidgetParam> params;
};
[[nodiscard]] std::vector<WidgetSpec> const& widgetSpecs();
// Null for an unknown widget.
[[nodiscard]] WidgetSpec const* findWidget(std::string const& id);
// The query of /widget/<id> (`value` gives a parameter's decoded value, empty when absent): an error, or empty when valid.
[[nodiscard]] std::string widgetQueryError(Config const& cfg, WidgetSpec const& widget, std::function<std::string(std::string const&)> const& value);
// GET /widgets: the operator-screen widgets and their query parameters (JSON schema).
[[nodiscard]] std::string widgetsJson(Config const& cfg);
// True when the WIDGET_FRAME_ANCESTORS source list names this Origin (or holds `*`).
[[nodiscard]] bool listedOrigin(std::string const& sources, std::string origin);
} // namespace replay
