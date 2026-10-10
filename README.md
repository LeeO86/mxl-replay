# mxl-replay

Live slow-motion replay and clip player for [MXL](https://github.com/dmf-mxl/mxl). It records several inputs on the TAI timeline, plays them back from 0% to 200% with frame repeat, frame blend, or optical-flow interpolation, and exposes every input and playout channel as an NMOS receiver or sender pair.

The interpolation is a derivative of [Futatabi](https://nageru.sesse.net/doc/futatabi.html) (part of Nageru, GPL-3.0-or-later) by Steinar H. Gunderson. The algorithm is Dense Inverse Search (Kroeger, Timofte, Dai, and Van Gool, ECCV 2016) plus the occlusion-aware blend Futatabi uses. See `NOTICE`.

This program is free software under **GPL-3.0-or-later**. See `LICENSE`.

The published image also contains the [MediaMTX](https://github.com/bluenviron/mediamtx) 1.20.1 binary (MIT, licence in `/usr/share/doc/mediamtx/LICENSE`), a separate program the WebRTC preview runs, and NVIDIA `libcudart.so.12` and `libnvjpeg.so.12`. Those libraries are NVIDIA's, used under the NVIDIA software licence, and are loaded at runtime. `libcuda` is not in the image; the NVIDIA container toolkit injects it. The source can be built without CUDA, in which case JPEG uses libjpeg-turbo and interpolation falls back to repeat or blend.

## What it does

- Records up to 12 cameras (default 4) of one house raster (default 1080p50) into a local NVMe ring.
- Plays one or more channels (default PGM and PVW): scrub, mark in/out, speed ramp, angle switch at the same TAI time, back to live.
- Slow motion by `repeat`, `blend`, or `interpolate`.
- Native high frame rate and phased high frame rate (for example 3 × 50p). Missing phases repeat a neighbour and are counted.
- Clips, a shotbox, playlists, upload (FFmpeg), and export (JPEG sequence + WAV).
- NMOS IS-04/IS-05 node, REST, WebSocket, and Prometheus metrics on `WEB_PORT` (default 8150).
- UI previews as JPEG pictures, or as one WebRTC mosaic of every camera and channel encoded once (`REPLAY_PREVIEW_MODE=webrtc`, see "Previews").
- Operator-screen widgets: `transport` and `clip-list` (see "Widgets").

## Storage

Video is JPEG 4:2:2, 8-bit, default quality 92, encoded with nvJPEG when a GPU is visible and with libjpeg-turbo otherwise. One hour of 1080p50 at that quality is about **70 GB per camera**:

```
bytes/frame = width * height * (quality / 92) * 0.1875
```

High frame rate scales with the frame rate. Audio is float32 PCM, 48 kHz. The process refuses to start when `REPLAY_BUFFER_HOURS` does not fit on `REPLAY_STORAGE_DIR`, and it says how many bytes it needs. Its own segments from an earlier run count as available.

The buffer lives on `REPLAY_STORAGE_DIR`: one series of segment files (`cam<N>/seg-<TAI>.bin`, `REPLAY_SEGMENT_SECONDS` each) per camera. Memory holds only an index of about 32 bytes per frame (4 cameras × 1.5 h at 50p: about 35 MB) and a few open files. Segments whose newest frame is older than the camera's buffer duration behind its newest frame are deleted, so disk use stays at the configured duration. After a restart the existing segments are indexed again: the buffer and the clips survive. HTTP starts first, so `/livez` answers while the index is built; `/readyz` and the API answer 503 until it is done. The buffers of cameras that are no longer configured are deleted at startup, unless a clip uses that camera.

**`REPLAY_STORAGE_DIR` must be on a dedicated local NVMe.** The operating-system disk, a rotating disk and network storage are not supported. The recorder writes every camera's frames as they arrive, and live playout shows a frame two grains after it arrived: on a 10k SAS disk a single write held the recorder up for 20–30 ms, and live channels then repeated a frame and skipped the next a few times in two minutes (none with the buffer in RAM). Protected clip ranges are never overwritten: a segment that a clip touches stays until the clip is deleted. New clips warn once the segments that clips keep exceed `REPLAY_PROTECT_MAX_PCT` (default 50%) of the budget. Frames that cannot be written (full disk or I/O error) are dropped and counted in `storage_write_failed_total`; the log has `segment_write_failed` once per run of failures and `segment_write_recovered` with the count.

Uploaded files are stored in their own library buffer (`REPLAY_STORAGE_DIR/library/frames`); their clips list camera `0`. A library segment stays as long as a clip uses it.

10-bit and 12-bit JPEG are not used. nvJPEG's baseline 4:2:2 encoder is 8-bit. See `IMPLEMENTATION_PLAN.md`.

The container image is the GPU build. It still starts with no GPU, which is enough for recording and clip playback (`repeat` / `blend`): on 2× Xeon Gold 6136 without a GPU, 4 cameras at 1080p50 plus 4 channels blending at 0.5× take about 6 cores (libjpeg-turbo). Interpolation and nvJPEG run when the NVIDIA container toolkit injects a device (`docker run --gpus all` or the Kubernetes `nvidia` runtime). Set `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility` (the image already does). The process runs as uid/gid 1000. One GPU is shared by every channel. The device holds three 4:2:2 frames and up to nine flow pairs: about 180 MB resident at 1080p and about 700 MB at 2160p, before nvJPEG's own workspace. A 4 GB GPU covers that path. These figures are the sizes of the allocations in `src/flow/dis_cuda.cu`, not a profile on an A4000 or an L4.

## Build

C++20, CMake ≥ 3.24, Ninja, GCC ≥ 12 or Clang ≥ 16.

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/unit-tests
./build/mxl-replay --help
```

Dependencies: libjpeg-turbo, SQLite, FFmpeg libraries (libavcodec, libavformat, libavutil, libswscale, libswresample). libmxl and nmos-cpp are optional and follow the pins in `IMPLEMENTATION_PLAN.md`. CUDA ≥ 12.4 enables `src/flow/dis_cuda.cu`.

```
tests/integration/replay.sh ./build/mxl-replay
MEDIAMTX_BIN=/path/to/mediamtx tests/integration/preview.sh ./build/mxl-replay
```

## Configuration

Precedence is environment, then `REPLAY_CONFIG_FILE`, then the imported state file `REPLAY_STATE_DIR/config.json`, then the defaults. The config file is a JSON object of strings or `KEY=value` lines. Unknown file keys are an error (exit 78). Unknown environment variables are ignored. Invalid values exit 78 with the key name.

There are no secret settings. Nothing in the configuration is a password, and configuration values are not written to the log. `GET /api/v1/config/export?include_secrets=true` returns the same document as the export without that query.

Process state (the SQLite catalog, imported settings, and IS-05 routes) lives in `REPLAY_STATE_DIR` (default `/config`). Media bytes stay in `REPLAY_STORAGE_DIR`. A catalog left in the storage directory from an older build is copied into the state directory on the first start.

| Key | Default |
| --- | --- |
| `HOST_ID` | hostname. Used in the default `NMOS_SEED`. If it is an IPv4 literal and `NMOS_HOST_ADDRESS` is unset, it is also the announced address |
| `REPLAY_CONFIG_FILE` | empty |
| `REPLAY_STATE_DIR` | `/config` |
| `REPLAY_FORMAT` | `1080p50` |
| `REPLAY_INPUTS` / `REPLAY_CHANNELS` | 4 / 2 |
| `REPLAY_STORAGE_DIR` | `/data/replay` |
| `REPLAY_BUFFER_HOURS` | 2 |
| `REPLAY_JPEG_QUALITY` | 92 |
| `REPLAY_PROTECT_MAX_PCT` | 50 |
| `REPLAY_FLOW_MODULE` | `dis-cuda` (`ofa` is accepted and still uses the DIS path) |
| `REPLAY_INTERP_PRESET` | `balanced` |
| `REPLAY_SLOWMO_AUDIO` | `mute` |
| `REPLAY_RAMP_FRAMES` | 3 |
| `REPLAY_HFR_SNAP` | 0.1 |
| `REPLAY_SEGMENT_SECONDS` | 10 |
| `REPLAY_LIVE_DELAY_FRAMES` | 2 |
| `REPLAY_INPUT_STALL_S` | 2. A connected camera input that records nothing for this many seconds logs `recording_stopped` (0.1–3600) |
| `REPLAY_PLAY_ON_FIRST_CLICK` | false |
| `REPLAY_ALLOW_CPU_INTERP` | false |
| `REPLAY_STORAGE_MIN_MBPS` | 100 |
| `REPLAY_ODIRECT` | false (ignored since 1.1.0, see the implementation plan) |
| `REPLAY_SYNTHETIC` | false |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` |
| `MXL_OUTPUT_DOMAIN_DIR` | `<scan>/replay-<short id>` |
| `MXL_OUTPUT_DOMAIN_ID` | UUIDv5 of `mxl-replay/<NMOS_SEED>/domain` |
| `MXL_HISTORY_DURATION` | `2000000000` nanoseconds, written only when the domain is created |
| `MXL_CLEANUP_ON_EXIT` | false |
| `SHUTDOWN_TIMEOUT_S` | 10 |
| `NMOS_ENABLE` | true |
| `NMOS_REGISTRY_ADDRESS` | empty |
| `NMOS_REGISTRY_PORT` | 3210 |
| `NMOS_QUERY_ADDRESS` | `NMOS_REGISTRY_ADDRESS` |
| `NMOS_QUERY_PORT` | `NMOS_REGISTRY_PORT + 1` |
| `NMOS_DNS_SD` | false. `pri`, `highest_pri`, and `authorization_highest_pri` are set to the maximum integer, so the process does not browse DNS-SD and does not advertise itself with mDNS. Avahi and D-Bus are not required |
| `NMOS_PORT` | 3302. The IS-04 events WebSocket listens on `NMOS_PORT + 1` |
| `NMOS_SEED` | `<HOST_ID>-replay` |
| `NMOS_LABEL` | unset. The node label stays `HOST_ID` and the device label stays `MXL Replay`. When set, it is the node label and the device label |
| `NMOS_TAGS` | empty JSON object. A JSON object of tag name to array of strings, added to the node and the device |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4. This is the only address announced (node href, API endpoints, IS-05 controls). `0.0.0.0`, `127.0.0.1`, and hostnames are rejected |
| `WEB_ENABLE` | true |
| `WEB_PORT` | 8150 |
| `REPLAY_PREVIEW_MODE` | `jpeg`: pictures on request. `webrtc`: one H.264 mosaic over WebRTC, no camera or channel JPEG ("Previews") |
| `PREVIEW_PUBLISH_URL` | empty: the image's own MediaMTX runs. An `rtsp://` or `rtsps://` base (no path, no credentials): publish to that shared MediaMTX, start none |
| `PREVIEW_PATH_PREFIX` | `mxl-replay`. The stream is `<prefix>/mosaic` |
| `PREVIEW_WHEP_URL` / `PREVIEW_HLS_URL` | empty: the own MediaMTX on `NMOS_HOST_ADDRESS`. Otherwise the public base the page plays from |
| `MEDIAMTX_RTSP_PORT` | 8854, RTSP ingest of the own MediaMTX on 127.0.0.1 |
| `MEDIAMTX_WHEP_PORT` / `MEDIAMTX_HLS_PORT` | 8689 / 8688 (own MediaMTX) |
| `MEDIAMTX_ICE_UDP_PORT` | 8489, WebRTC ICE of the own MediaMTX, UDP and TCP |
| `WIDGET_FRAME_ANCESTORS` | `'self'`. CSP `frame-ancestors` of the widget pages; `/widgets` answers these origins with CORS |
| `LOG_LEVEL` | `info` |
| `LOG_FORMAT` | `json` |

Per-camera keys are `CAM1_LABEL`, `CAM1_COLOUR` (alias `CAM1_COLOR`), `CAM1_RECORD`, `CAM1_AUDIO`, `CAM1_PHASES`, `CAM1_HFR_FPS`, `CAM1_BUFFER_HOURS`. Per-channel keys are `CH1_LABEL` (default `PGM`, `PVW`, then `CH3`, `CH4` …), `CH1_IDLE` (`last`, `black`, `e2e`), `CH1_MOTION`, `CH1_AUDIO`, `CH1_TC` (`source` or `output`), `CH1_FLOW`, `CH1_ATMOS`, `CH1_LOCK`.

`GET /api/v1/config/export` returns one JSON document (`version`, `settings`, `clips`, `playlists`). `POST /api/v1/config/import` accepts that document. Channel and camera labels and modes apply immediately. Ports, format, counts, the seed, and the domain are stored and take effect on the next start (`restart_required` in the response). IS-05 activations are kept in `REPLAY_STATE_DIR/routes.json` and restored after a restart.

## Operation

The UI is served on `/`. The pages are tabs with their own address (`/#shotbox`, `#lsm`, `#library`, `#playlists`, `#cameras`, `#nmos`, `#settings`):

- **Shotbox**: one button per clip and playlist. One click cues it (yellow), the next plays it (green, with progress), the next pauses, the next resumes; an ended shot is grey. *Back to live*, speed presets, and the target channel at the top. Keys: 1–9 and 0 select a button of the bank, Enter cues/plays it, Space is play/pause, PgUp/PgDn change the bank.
- **LSM**: the channel monitor (a JPEG preview five times a second, or the full-motion mosaic in WebRTC mode; full motion is also the WebRTC monitor on the channel output), the cameras (click or 1–9 switches the angle), IN/OUT, Go IN/OUT, frame and second steps, Play/Pause/Live, the speed fader with presets and reverse, motion, audio and timecode mode, lock to the first channel, and the buffer timeline (drag to scrub, wheel ±1 frame, Shift ±1 s). *Create clip* asks for name, all angles, colour and tags.
- **Library**: search, the clip inspector (IN/OUT frame by frame, speed, modes, end action, name, colour, tags), export, consolidate, delete, upload.
- **Playlists**: entries with their own speed, end action and auto-advance; play on the selected channel. When an entry ends with *next*, auto-advance plays the next clip at once, otherwise it is cued and waits for Play.
- **Cameras**, **NMOS**, **Settings** (configuration export and import, the keyboard map, a WebHID jog/shuttle).

The default keys are Space play/pause, I/O mark IN/OUT, Shift+I/O go to IN/OUT, ←/→ one frame, Shift+←/→ one second, ↑/↓ speed ±5 %, L live, C create clip; the Settings page changes them (kept in the browser). WebHID works with a Contour ShuttleXpress, ShuttlePRO or ShuttlePRO v2 in Chrome or Edge, on a secure page (https or localhost): the ring sets the speed and plays, the jog wheel steps frames.

HTTP on `WEB_PORT`:

| Method | Path |
| --- | --- |
| GET | `/livez`, `/readyz`, `/statusz`, `/metrics`, `/` |
| GET | `/api/v1/status`, `/api/v1/config`, `/api/v1/config/export`, `/api/v1/nmos`, `/api/v1/clips`, `/api/v1/playlists`, `/api/v1/playlists/{id}` |
| GET | `/api/v1/channels/{n}/preview.jpg`, `/api/v1/cameras/{n}/preview.jpg` (JPEG mode), `/api/v1/clips/{id}/thumbnail.jpg`, `/api/v1/preview/map` |
| GET | `/widgets`, `/widget/transport?channel=<n>`, `/widget/clip-list[?channel=<n>]` (with `&theme=dark\|light\|transparent`) |
| POST | `/api/v1/channels/{n}/transport` (`command`: `play`, `pause`, `live`), `/speed` (`speed`, −1…2), `/position` (`frames`, `seconds` or `tai_ns`), `/marks` (`which`: `in`, `out`, `goto-in`, `goto-out`), `/angle` (`camera`), `/mode` (`motion`, `audio`, `timecode`: `source` or `output`), `/lock` (`enable`) |
| POST | `/api/v1/clips` (`channel`, `name`, `all_angles`, `force`, `colour`, `tags`), `/api/v1/clips/{id}/export`, `/api/v1/clips/{id}/consolidate`, `/api/v1/shotbox/{id}` (a clip or playlist; `channel`) |
| POST | `/api/v1/playlists` (`name`, `clips` or `entries`), `/api/v1/playlists/{id}/play` (`channel`), `/api/v1/uploads?name=…` (the file is the body), `/api/v1/config/import`, `/api/v1/control` |
| PATCH | `/api/v1/clips/{id}` (`name`, `in_ns`, `out_ns`, `speed`, `motion`, `audio`, `end`, `colour`, `tags`, `force`) |
| PUT | `/api/v1/playlists/{id}` (`name`, `entries`: `clip_id`, `speed`, `end`, `auto_advance`) |
| DELETE | `/api/v1/clips/{id}`, `/api/v1/playlists/{id}` |
| WebSocket | `/api/v1/events` (the status, ten times a second) |

Request bodies are JSON objects. A missing or invalid value answers 400 with `{"error": …}`, an unknown channel, camera, clip or playlist 404, and `POST /api/v1/clips` answers 409 when clips already keep `REPLAY_PROTECT_MAX_PCT` of the storage (send `force: true` to create it anyway). Channel commands answer with the status. The status has each channel's timecode as the output carries it (`timecode`) and the source timecode of its position, IN and OUT (`position_tc`, `in_tc`, `out_tc`), all at the house rate, non-drop-frame, from the TAI time of day, and `target_speed`, the speed set on the channel (`speed` ramps to it while playing; a new clip takes `target_speed`). A new IN or OUT sent with `PATCH` needs a recorded frame within one frame period and lands on it (400 otherwise); a longer clip at the protection cap answers 409 unless `force` is set. Clip and playlist-entry speeds are 0.01–2: a clip plays forward. A playlist `PUT` keeps entries whose clip was deleted (playback skips them). Previews are made on request and kept a little while for other UIs: 640 pixels wide for a channel (its last output grain, 150 ms), a quarter-size decode of the stored JPEG for a camera (its newest frame, 1 s) and a clip (its IN frame, 404 when that frame is gone). The WebSocket never waits for a client: one that cannot take a status message at once is disconnected, and the UI reconnects.

`/livez` is 200 as soon as HTTP listens. `/readyz` and the API are 503 (`{"status":"indexing"}`) until the buffer is indexed, then `/readyz` is 200 when the process is serving. If `NMOS_REGISTRY_ADDRESS` is set and NMOS is enabled, it is 200 only after the Query API at `NMOS_QUERY_ADDRESS:NMOS_QUERY_PORT` lists this node. `/metrics` is Prometheus text with the prefix `mxl_replay_`.

On SIGTERM the process stops playout, releases MXL readers and writers, removes its NMOS resources so the registry receives DELETEs, and, when `MXL_CLEANUP_ON_EXIT=true`, deletes only its own output domain. It then exits 143. Work still running after `SHUTDOWN_TIMEOUT_S` is abandoned.

Playout channels are NMOS senders (video `video/v210`, audio `audio/float32`, data `video/smpte291`) in the replay's own domain, labelled `<channel label> Video`, `Audio` and `Data`. Inputs are receivers, labelled `<camera label> Video` and `Audio` the same way. A receiver accepts activation before the flow exists and stays `waiting` until the grain can be read. A connected camera input that records nothing for `REPLAY_INPUT_STALL_S` (its writer stopped or restarted, the flow was removed, a fabrics mirror lost its link) logs `recording_stopped` once, with `camera`, `phase` and `reason` (`no_grains`; `invalid_grains`: grains arrive but are invalid or incomplete; `flow_missing`: the flow cannot be opened). Recording resumes by itself when grains come back, also on a flow that was removed and created again, and logs `recording_resumed` with the `gap_ms`. Nothing is stored for the gap. Camera audio (`CAM<n>_AUDIO`, default on) is stored with each video frame: the frame's samples of a 48 kHz `audio/float32` flow, first two channels, mono doubled. Another sample rate leaves the audio receiver `unsupported`. An HFR camera takes its audio with phase 1. The process does not write into a mirror domain.

A camera whose audio delivers no samples for `REPLAY_INPUT_STALL_S` worth of video frames while its video records logs `recording_audio_stopped` (`camera`, `reason`: `flow_missing` or `no_samples`) once, and `recording_audio_resumed` with `gap_ms` when it is back; those frames are stored without audio. A channel that falls more than two grains behind its output clock continues at the current grain (a resync): `output_resyncs_total` counts it and the log has `output_resync` (`channel`, `resyncs` since the last line, `jumped_grains`) at most every 10 s. One at the start is normal (the first grains set up the GPU path).

`REPLAY_SYNTHETIC=true` feeds a moving test raster into the recorder so the UI and the integration test can run without other MXL flows.

## Previews

`REPLAY_PREVIEW_MODE` picks one of two ways the UI shows cameras and channels; the replay never makes both.

- `jpeg` (the default): the pictures above, made on request (a channel 5 times a second on the LSM page, a camera once a second).
- `webrtc`: **one mosaic** of every channel and every camera at 25 pictures a second (every second grain at 50p, every grain up to 30p), encoded **once** as H.264 with NVENC and published to MediaMTX, which serves WebRTC (WHEP) and HLS. The page opens one WHEP session and shows every picture as a `<video>` on that one stream, cropped to its tile with CSS `object-view-box` (Chrome and Edge 104+; other browsers get the same crop by position). There is no audio. Without NVENC (no GPU, or `video` missing from `NVIDIA_DRIVER_CAPABILITIES`) libx264 encodes it and `preview_nvenc_unavailable` says why. Camera and channel JPEGs answer 404; clip thumbnails stay.

The mosaic has the channels at 640×360 (three a row) above the cameras at 480×270 (four a row): 1920×630 for 2 channels and 4 cameras. A canvas taller than 1080 lines is scaled down to fit 1920×1080. `GET /api/v1/preview/map` lists every tile (`ch<n>`, `cam<n>`) with `x`, `y`, `w`, `h`, the canvas size, the stream path and the WHEP and HLS URLs. Each camera's reader and each channel's playout thread draw their own tile from the grain they already have on the GPU (area average; without a GPU point-sampled on the CPU), so nothing extra is decoded or copied to the host.

The platform's preview contract: with `PREVIEW_PUBLISH_URL` the mosaic goes to that shared MediaMTX as `<PREVIEW_PATH_PREFIX>/mosaic` over RTSP/TCP and the replay starts no MediaMTX; set `PREVIEW_WHEP_URL` (and `PREVIEW_HLS_URL`) to its public bases so the page plays from there. Without it the image's MediaMTX runs as a child of the replay (restarted when it exits, stopped with the replay) on these ports, chosen next to the other media functions:

| Port | Own MediaMTX |
| --- | --- |
| 8854/tcp | RTSP ingest, 127.0.0.1 only |
| 8689/tcp | WHEP (WebRTC) |
| 8688/tcp | low-latency HLS |
| 8489/udp+tcp | WebRTC ICE (host candidate `NMOS_HOST_ADDRESS`) |

mxl-webrtc-monitor uses 8554/8889/8888/8189 (and 9997/9998), the FlowXer engine 8654/8989/8988/8289/9897, mxl-multiviewer 1.4.0 8754/8789/8788/8389. A collision with `WEB_PORT` or `NMOS_PORT`/`+1` exits 78. `/statusz` has `preview` (`mode`, and in WebRTC mode `publish` own or shared, `publish_url`, `path`, `state` connecting/publishing/error, `error`, `encoder` nvenc or x264, `frames`, the own MediaMTX's `running` and `restarts`, `streams`).

Lab, NVIDIA A16 (GPU 3), 4 cameras 1080p50 from the test player and 2 live channels, buffer in RAM; CPU of the replay container (perf cgroup and `/proc`): JPEG mode with no page open 0.77–0.80 cores, with one LSM page polling (5 channel and 4 camera pictures a second, median 5 ms each on the HTTP thread) +0.04 cores, with three pages about the same (the pictures are shared for 150 ms and 1 s); WebRTC mode 0.85–0.87 cores with no viewer and the same with one (all 5 tiles of the LSM page from one stream at 25 fps). Its own parts: the encoder thread 2 % of a core, NVENC 1.65 ms per picture (copy on the GPU, encode, send; 2 % of the encoder engine, SM load unchanged at 47 %), the NVENC driver about 2 %, MediaMTX 1 % plus about 1.5 % per WHEP viewer.

## Widgets

Operator screens (the platform's production designer) frame single controls of the replay:

| Widget | Query | Minimum size | Shows |
| --- | --- | --- | --- |
| `transport` | `channel` (required) | 480×160 | label, state, timecode; Cue (back to IN, paused), Play/Pause, Live (E2E); speed fader 0–200 % and presets |
| `clip-list` | `channel` (optional: without it a channel picker) | 400×300 | every clip, newest first, with Load (cues it on the channel; then Play, Pause) |

`GET /widgets` lists them with a JSON schema of their parameters; it answers an `Origin` that `WIDGET_FRAME_ANCESTORS` names (or `*`) with `Access-Control-Allow-Origin`. `GET /widget/<id>?…[&theme=dark|light|transparent]` is the page without the app around it, on the replay's own API; a bad parameter answers 400, an unknown widget 404. Only these routes carry `Content-Security-Policy: frame-ancestors <WIDGET_FRAME_ANCESTORS>` (default `'self'`), and none carries `X-Frame-Options`. The page posts `{type: "widget-ready"}` and `{type: "widget-size", w, h}` to its parent.

## Metrics

Prefix `mxl_replay_`. Cameras (label `camera`) report `record_frames_total`, `record_dropped_total`, `phase_missing_total`, `storage_write_failed_total`, `protected_bytes`, `disk_bytes`, and `record_last_frame_age_seconds`: seconds since the camera's newest recorded frame, counted from the process start at most. It stays below 0.1 s while the camera records and grows when it does not (also when it is not connected or `CAM<n>_RECORD` is off); the status has it per camera as `last_frame_age_s`. Channels report state, speed, and late grains. `frame_gpu_seconds` is a histogram labelled by stage. Storage reports `write_bytes_per_second` (what the recorder wrote since the previous scrape, over at least a second: 0 when nothing records), `storage_measured_bytes_per_second` (the write test at start, `storage_bps` in the status), free bytes, and protected bytes. Channels also report `output_resyncs_total`. Previews: `preview_mode{mode}` (jpeg, webrtc) and in WebRTC mode `preview_publish_mode{mode}` (own, shared), `preview_publish_state{stream,state}`, `preview_encoder{encoder}`, `preview_frames_total`, `preview_encode_seconds` (histogram of one picture's copy, encode and send) and `preview_mediamtx_restarts_total` (own mode). A Grafana dashboard is in `deploy/grafana/`.

## Deploy

`docker/Dockerfile` builds `ghcr.io/leeo86/mxl-replay`. The image labels include `org.opencontainers.image.source`, `org.opencontainers.image.revision`, `org.opencontainers.image.licenses`, and `io.dmf.mxl.revision`.

| Tag | When |
| --- | --- |
| `nightly-dev` | push to `main`, the 04:00 UTC rebuild, and a manual run on `main` |
| `git-<sha7>` | every published build |
| `X.Y.Z`, `X.Y`, `X` | a `vX.Y.Z` release tag |

`nightly-dev` is the only tag that moves. Pull requests build the image and do not push it. On the platform the pod uses the pod network (not the host network), uid/gid 1000, `supplementalGroups: [1000]`, the MXL root at `/Volumes/mxl`, a writable `/config`, and probes on `/livez` and `/readyz`. `deploy/k8s/deployment.yaml` is that shape. `terminationGracePeriodSeconds` is 30, which is longer than `SHUTDOWN_TIMEOUT_S`.

```bash
docker run --gpus all \
  -e NVIDIA_DRIVER_CAPABILITIES=compute,video,utility \
  -e NMOS_HOST_ADDRESS=<this-container-ipv4> \
  -e NMOS_SEED=<production>-replay \
  -e NMOS_REGISTRY_ADDRESS=<registry-ipv4> \
  -e NMOS_DNS_SD=false \
  -e MXL_DOMAIN_SCAN_PATH=/Volumes/mxl \
  -e MXL_OUTPUT_DOMAIN_DIR=/Volumes/mxl/<instance> \
  -e MXL_CLEANUP_ON_EXIT=true \
  -e REPLAY_STATE_DIR=/config \
  -v /Volumes/mxl:/Volumes/mxl \
  -v /data/replay:/data/replay \
  -v replay-config:/config \
  -p 8150:8150 -p 3302:3302 -p 3303:3303 \
  ghcr.io/leeo86/mxl-replay:1.4.0
```

The Kubernetes deployment pins the pod to a node with a local NVMe `hostPath` and requests `nvidia.com/gpu: 1` with `runtimeClassName: nvidia`. GPU access uses the NVIDIA runtime class, so the container does not run as root and does not set `hostIPC`. Add `graphics` to `NVIDIA_DRIVER_CAPABILITIES` only if the OFA Vulkan path is enabled later.

`docker/docker-compose.demo.yaml` is the compose demo: registry, two test-player outputs as cameras, this replay, and the WebRTC monitor on the channel outputs.

GPU performance targets (concurrent 1080p50 interpolate channels, 2160p50, one hour with no late grains) are measured by the procedure in `docs/benchmarks/README.md`. They are not filled in until that run exists.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | clean shutdown |
| 75 | temporary failure (storage or state directory, a port that cannot be bound, unexpected error) |
| 78 | configuration, including a buffer that does not fit |
| 143 | SIGTERM |
