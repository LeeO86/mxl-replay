# mxl-replay

Live slow-motion replay and clip player for [MXL](https://github.com/dmf-mxl/mxl). It records several inputs on the TAI timeline, plays them back from 0% to 200% with frame repeat, frame blend, or optical-flow interpolation, and exposes every input and playout channel as an NMOS receiver or sender pair.

The interpolation is a derivative of [Futatabi](https://nageru.sesse.net/doc/futatabi.html) (part of Nageru, GPL-3.0-or-later) by Steinar H. Gunderson. The algorithm is Dense Inverse Search (Kroeger, Timofte, Dai, and Van Gool, ECCV 2016) plus the occlusion-aware blend Futatabi uses. See `NOTICE`.

This program is free software under **GPL-3.0-or-later**. See `LICENSE`.

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

High frame rate scales with the frame rate. Audio is float32 PCM, 48 kHz. The process refuses to start when `REPLAY_BUFFER_HOURS` does not fit on `REPLAY_STORAGE_DIR`, and it says how many bytes it needs.

Use a dedicated NVMe, not the operating-system disk. Protected clip ranges are never overwritten. New clips warn once protected data exceeds `REPLAY_PROTECT_MAX_PCT` (default 50%) of the budget.

10-bit and 12-bit JPEG are not used. nvJPEG's baseline 4:2:2 encoder is 8-bit. See `IMPLEMENTATION_PLAN.md`.

The container image is the GPU build. It still starts with no GPU, which is enough for clip playback (`repeat` / `blend`). Interpolation and nvJPEG run when the NVIDIA container toolkit injects a device (`docker run --gpus all` or the Kubernetes `nvidia` runtime). Set `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility` (the image already does). Do not expect a separate CPU image.

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

Environment overrides `REPLAY_CONFIG_FILE`, which overrides the defaults. The file is a JSON object of strings or `KEY=value` lines. Unknown file keys are an error (exit 78). Unknown environment variables are ignored.

| Key | Default |
| --- | --- |
| `REPLAY_FORMAT` | `1080p50` |
| `REPLAY_INPUTS` / `REPLAY_CHANNELS` | 4 / 2 |
| `REPLAY_STORAGE_DIR` | `/data/replay` |
| `REPLAY_BUFFER_HOURS` | 2 |
| `REPLAY_JPEG_QUALITY` | 92 |
| `REPLAY_PROTECT_MAX_PCT` | 50 |
| `REPLAY_FLOW_MODULE` | `dis-cuda` |
| `REPLAY_INTERP_PRESET` | `balanced` |
| `REPLAY_SLOWMO_AUDIO` | `mute` |
| `WEB_PORT` | 8150 |
| `NMOS_PORT` | 3302 |
| `NMOS_SEED` | `HOST_ID-replay` |

`GET /api/v1/config/export` writes the flat `KEY=value` form. Per-camera keys are `CAM1_LABEL`, `CAM1_COLOUR`, `CAM1_RECORD`, `CAM1_AUDIO`, `CAM1_PHASES`, `CAM1_HFR_FPS`, `CAM1_BUFFER_HOURS`. Per-channel keys are `CH1_LABEL`, `CH1_IDLE` (`last`, `black`, `e2e`), `CH1_MOTION`, `CH1_AUDIO`, `CH1_TC` (`source` or `output`), `CH1_FLOW`, `CH1_ATMOS`, `CH1_LOCK`.

## Operation

The UI is served on `/`. The shotbox is the main page: one click cues a clip, the next plays it, another pauses. Space is play/pause, number keys pick buttons, arrows scrub.

Playout channels are NMOS senders (video `video/v210`, audio `audio/float32`, data `video/smpte291`) in the replay's own domain. Inputs are receivers. A receiver accepts activation before the flow exists and stays `waiting` until the grain can be read. The process does not write into a mirror domain.

`REPLAY_SYNTHETIC=true` feeds a moving test raster into the recorder so the UI and the integration test can run without other MXL flows.

## Metrics

Prefix `mxl_replay_`. Cameras report record rate, drops, missing phases, and buffer occupancy. Channels report state, speed, and late grains. `frame_gpu_seconds` is a histogram labelled by stage. Storage reports write rate, free bytes, and protected bytes. A Grafana dashboard is in `deploy/grafana/`.

## Deploy

`docker/Dockerfile` builds `ghcr.io/leeo86/mxl-replay` and labels it `io.dmf.mxl.revision`. The Container workflow publishes the same way as the sibling media functions:

| Tag | When |
| --- | --- |
| `nightly-dev` | every push to `main`, the nightly rebuild, and a manual run |
| `git-<sha>` | every published build |
| `X.Y.Z`, `X.Y`, `X`, `latest` | a `vX.Y.Z` release tag |

`nightly-dev` is the rolling development image. Pull requests build the image and do not push it.

```bash
docker run --gpus all --network host \
  -e NVIDIA_DRIVER_CAPABILITIES=compute,video,utility \
  -e MXL_DOMAIN_SCAN_PATH=/Volumes/mxl \
  -v /Volumes/mxl:/Volumes/mxl \
  -v /data/replay:/data/replay \
  ghcr.io/leeo86/mxl-replay:nightly-dev
```

The Kubernetes deployment pins the pod to a node with a local NVMe `hostPath` and requests `nvidia.com/gpu: 1` with `runtimeClassName: nvidia`. Add `graphics` to `NVIDIA_DRIVER_CAPABILITIES` only if the OFA Vulkan path is enabled later.

`docker/docker-compose.demo.yaml` is the compose demo: registry, two test-player outputs as cameras, this replay, and the WebRTC monitor on the channel outputs.

GPU performance targets (concurrent 1080p50 interpolate channels, 2160p50, one hour with no late grains) are measured by the procedure in `docs/benchmarks/README.md`. They are not filled in until that run exists.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | clean shutdown |
| 75 | temporary failure (storage directory, unexpected error) |
| 78 | configuration, including a buffer that does not fit |
| 143 | SIGTERM |
