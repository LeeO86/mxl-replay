# mxl-replay

Live slow-motion replay and clip player for [MXL](https://github.com/dmf-mxl/mxl). It records several inputs on the TAI timeline, plays them back from 0% to 200% with frame repeat, frame blend, or optical-flow interpolation, and exposes every input and playout channel as an NMOS receiver or sender pair.

The interpolation is a derivative of [Futatabi](https://nageru.sesse.net/doc/futatabi.html) (part of Nageru, GPL-3.0-or-later) by Steinar H. Gunderson. The algorithm is Dense Inverse Search (Kroeger, Timofte, Dai, and Van Gool, ECCV 2016) plus the occlusion-aware blend Futatabi uses. See `NOTICE`.

This program is free software under **GPL-3.0-or-later**. See `LICENSE`.

The published image also contains NVIDIA `libcudart.so.12` and `libnvjpeg.so.12`. Those libraries are NVIDIA's, used under the NVIDIA software licence, and are loaded at runtime. `libcuda` is not in the image; the NVIDIA container toolkit injects it. The source can be built without CUDA, in which case JPEG uses libjpeg-turbo and interpolation falls back to repeat or blend.

## What it does

- Records up to 12 cameras (default 4) of one house raster (default 1080p50) into a local NVMe ring.
- Plays one or more channels (default PGM and PVW): scrub, mark in/out, speed ramp, angle switch at the same TAI time, back to live.
- Slow motion by `repeat`, `blend`, or `interpolate`.
- Native high frame rate and phased high frame rate (for example 3 × 50p). Missing phases repeat a neighbour and are counted.
- Clips, a shotbox, playlists, upload (FFmpeg), and export (JPEG sequence + WAV).
- NMOS IS-04/IS-05 node, REST, WebSocket, and Prometheus metrics on `WEB_PORT` (default 8150).

## Storage

Video is JPEG 4:2:2, 8-bit, default quality 92, encoded with nvJPEG when a GPU is visible and with libjpeg-turbo otherwise. One hour of 1080p50 at that quality is about **70 GB per camera**:

```
bytes/frame = width * height * (quality / 92) * 0.1875
```

High frame rate scales with the frame rate. Audio is float32 PCM, 48 kHz. The process refuses to start when `REPLAY_BUFFER_HOURS` does not fit on `REPLAY_STORAGE_DIR`, and it says how many bytes it needs. Its own segments from an earlier run count as available.

The buffer lives on `REPLAY_STORAGE_DIR`: one series of segment files (`cam<N>/seg-<TAI>.bin`, `REPLAY_SEGMENT_SECONDS` each) per camera. Memory holds only an index of about 32 bytes per frame (4 cameras × 1.5 h at 50p: about 35 MB) and a few open files. Segments whose newest frame is older than the camera's buffer duration behind its newest frame are deleted, so disk use stays at the configured duration. After a restart the existing segments are indexed again: the buffer and the clips survive.

Use a dedicated NVMe, not the operating-system disk. Protected clip ranges are never overwritten: a segment that a clip touches stays until the clip is deleted. New clips warn once protected data exceeds `REPLAY_PROTECT_MAX_PCT` (default 50%) of the budget.

10-bit and 12-bit JPEG are not used. nvJPEG's baseline 4:2:2 encoder is 8-bit. See `IMPLEMENTATION_PLAN.md`.

The container image is the GPU build. It still starts with no GPU, which is enough for clip playback (`repeat` / `blend`). Interpolation and nvJPEG run when the NVIDIA container toolkit injects a device (`docker run --gpus all` or the Kubernetes `nvidia` runtime). Set `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility` (the image already does). The process runs as uid/gid 1000. One GPU is shared by every channel. The device holds three 4:2:2 frames and up to nine flow pairs: about 180 MB resident at 1080p and about 700 MB at 2160p, before nvJPEG's own workspace. A 4 GB GPU covers that path. These figures are the sizes of the allocations in `src/flow/dis_cuda.cu`, not a profile on an A4000 or an L4.

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
| `LOG_LEVEL` | `info` |
| `LOG_FORMAT` | `json` |

Per-camera keys are `CAM1_LABEL`, `CAM1_COLOUR` (alias `CAM1_COLOR`), `CAM1_RECORD`, `CAM1_AUDIO`, `CAM1_PHASES`, `CAM1_HFR_FPS`, `CAM1_BUFFER_HOURS`. Per-channel keys are `CH1_LABEL` (default `PGM`, `PVW`, then `CH3`, `CH4` …), `CH1_IDLE` (`last`, `black`, `e2e`), `CH1_MOTION`, `CH1_AUDIO`, `CH1_TC` (`source` or `output`), `CH1_FLOW`, `CH1_ATMOS`, `CH1_LOCK`.

`GET /api/v1/config/export` returns one JSON document (`version`, `settings`, `clips`, `playlists`). `POST /api/v1/config/import` accepts that document. Channel and camera labels and modes apply immediately. Ports, format, counts, the seed, and the domain are stored and take effect on the next start (`restart_required` in the response). IS-05 activations are kept in `REPLAY_STATE_DIR/routes.json` and restored after a restart.

## Operation

The UI is served on `/`. The shotbox is the main page: one click cues a clip, the next plays it, another pauses. Space is play/pause, number keys pick buttons, arrows scrub.

HTTP on `WEB_PORT`:

| Method | Path |
| --- | --- |
| GET | `/livez`, `/readyz`, `/statusz`, `/metrics`, `/` |
| GET | `/api/v1/status`, `/api/v1/config`, `/api/v1/config/export`, `/api/v1/nmos`, `/api/v1/clips`, `/api/v1/playlists` |
| POST | `/api/v1/config/import`, `/api/v1/clips`, `/api/v1/playlists`, `/api/v1/uploads`, `/api/v1/control` |
| POST | `/api/v1/channels/{n}/transport`, `/api/v1/shotbox/{id}`, `/api/v1/clips/{id}/export`, `/api/v1/clips/{id}/consolidate` |
| DELETE | `/api/v1/clips/{id}` |
| WebSocket | `/api/v1/events` |

`/readyz` is 200 when the process is serving. If `NMOS_REGISTRY_ADDRESS` is set and NMOS is enabled, it is 200 only after the Query API at `NMOS_QUERY_ADDRESS:NMOS_QUERY_PORT` lists this node. `/metrics` is Prometheus text with the prefix `mxl_replay_`.

On SIGTERM the process stops playout, releases MXL readers and writers, removes its NMOS resources so the registry receives DELETEs, and, when `MXL_CLEANUP_ON_EXIT=true`, deletes only its own output domain. It then exits 143. Work still running after `SHUTDOWN_TIMEOUT_S` is abandoned.

Playout channels are NMOS senders (video `video/v210`, audio `audio/float32`, data `video/smpte291`) in the replay's own domain, labelled `<channel label> Video`, `Audio` and `Data`. Inputs are receivers, labelled `<camera label> Video` and `Audio` the same way. A receiver accepts activation before the flow exists and stays `waiting` until the grain can be read. The process does not write into a mirror domain.

`REPLAY_SYNTHETIC=true` feeds a moving test raster into the recorder so the UI and the integration test can run without other MXL flows.

## Metrics

Prefix `mxl_replay_`. Cameras report record rate, drops, missing phases, and buffer occupancy. Channels report state, speed, and late grains. `frame_gpu_seconds` is a histogram labelled by stage. Storage reports write rate, free bytes, and protected bytes. A Grafana dashboard is in `deploy/grafana/`.

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
  ghcr.io/leeo86/mxl-replay:1.1.0
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
