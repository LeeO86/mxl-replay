# mxl-replay — Specification

Status: Draft v0.1 (for implementation by Claude Code or Cursor in a new, empty repository)
Repository: `LeeO86/mxl-replay` (name can still change)
License: **GPL-3.0-or-later** (this repo reuses the algorithm of Futatabi, part of Nageru, GPL-3.0-or-later)
Aligns with: `LeeO86/mxl-decklink`, `LeeO86/mxl-st2110-gateway`, `LeeO86/mxl-fabrics-agent`,
`LeeO86/mxl-multiviewer`, `LeeO86/mxl-webrtc-monitor`, `LeeO86/mxl-test-player`,
`LeeO86/mxl-srt-gateway`, `LeeO86/mxl-color-corrector`, platform repo `mmz-srf/mxl-poc-platform`

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

---

## 1. Purpose and scope

`mxl-replay` is a live slow-motion (LSM) replay server and clip player for MXL:

- **Records** several MXL inputs continuously and in sync on the TAI timeline into a
  ring buffer on local NVMe.
- **Replays** from the buffer on one or more **playout channels**: scrub, mark in/out
  live, play at any speed from 0 % to 200 %, switch camera angle at the same moment,
  back to live.
- **Slow motion** by frame repeat, frame blend, or **optical-flow interpolation** on
  the GPU (CUDA port of Futatabi's algorithm).
- **Clips**: marked ranges become protected clips; files can be uploaded and are
  converted at upload. A **shotbox** UI plays clips with one click.
- **High frame rate**: native HFR flows and **phased** HFR (e.g. 3 × 50p phases =
  150 fps) are recorded and replayed with real frames instead of interpolated ones.

Every playout channel is an NMOS sender pair, every input an NMOS receiver pair; both
are routed by the platform crosspoint like any other media function.

Out of scope for v1: hardware controllers (other than keyboard and optional WebHID
jog/shuttle, §8.4), EVS/IPDirector integration, multi-server clip sharing, editing
of sequences beyond playlists, compressed outputs, HDR.

---

## 2. Architecture

```
 INPUTS (per camera / phase group)
   MXL readers (v210, float32) ─► GPU JPEG encode (nvJPEG | CPU) ─► recorder ─► NVMe ring buffer
                                                                 └► frame index (SQLite/WAL)
 PLAYOUT CHANNEL
   control (scrub / play / speed / angle) ─► frame scheduler (TAI output clock)
      ─► fetch source frames (NVMe) ─► GPU JPEG decode ─► {repeat | blend | interpolate}
      ─► v210 pack ─► MXL writer (video) ; audio path ─► MXL writer (audio) ; ANC timecode
 INTERPOLATION ENGINE (CUDA)
   flow module: {DIS-CUDA (Futatabi port) | OFA (hardware optical flow, Vulkan)} ─► interpolation (occlusion-aware)
 per process: nmos-cpp node, web server (shotbox, LSM, library, settings), REST, WebSocket, /metrics
```

- All per-frame image work stays on the GPU from JPEG decode to v210 pack; no
  host round trips except reading compressed frames from NVMe and writing MXL grains.
- One CUDA context per process; each playout channel uses its own CUDA streams so
  channels overlap.

---

## 3. Technology and build

- C++20, CMake ≥ 3.24, Ninja; GCC ≥ 12 or Clang ≥ 16; CUDA ≥ 12.4.
- MXL `release/v1.1` at `218ddaa` (platform pin), Fabrics OFF; nmos-cpp at the
  siblings' commit.
- **nvJPEG** for GPU JPEG encode/decode (4:2:2, 8-bit; record whether 10-bit or
  12-bit JPEG is usable); **libjpeg-turbo** as CPU fallback.
- Optical flow: own CUDA kernels (§6). Optional OFA module via Vulkan
  `VK_NV_optical_flow` with CUDA–Vulkan interop (external memory/semaphores).
- SQLite (WAL mode) for frame index, clips, playlists and state.
- FFmpeg libraries for file upload conversion and audio file decode.
- Audio time-stretch: phase vocoder with WSOLA fallback, own implementation or a
  compatible library (license check; GPL-compatible is enough).
- Web UI: Vue 3 SPA embedded, no CDN.
- Tests: doctest (vendored), shell integration tests, the interpolation reference
  harness (§6.4).
- **Attribution**: README and `NOTICE` credit Futatabi/Nageru (Steinar H. Gunderson)
  and the papers its algorithm is based on (Kroeger et al., *Fast Optical Flow using
  Dense Inverse Search*; and the occlusion-reasoning temporal interpolation method it
  follows). Ported files keep the original copyright headers in addition to ours.
- Record every deviation in `IMPLEMENTATION_PLAN.md`.

---

## 4. Inputs and recording

### 4.1 Inputs

- `REPLAY_INPUTS` inputs (default 4, max 12 at 1080p50 — limit from measurement).
  Each input is a **camera** with one video receiver and one audio receiver
  (BCP-007-03), group hint `<camera label>:Video` / `:Audio`.
- Receiver behaviour as all media functions: activation accepted before the flow
  exists (`waiting`, retry), IS-04 `subscription` updated, domain scan of the MXL
  root including mirror domains.
- All cameras are recorded against the **TAI timeline**: the stored timestamp of a
  frame is its TAI grain time. Positions in the UI are TAI times (shown as timecode),
  so all angles of a moment share the same position.
- Per camera: label, colour tag, record on/off, audio record on/off.

### 4.2 High frame rate inputs

- **Native HFR**: a video flow whose grain rate is above the house rate (e.g.
  100, 120, 150, 200 fps). Recorded as is; the camera's HFR factor is
  `flow rate / house rate`.
- **Phased HFR**: a camera configured as a **phase group** of N video receivers
  (N = 2…4; e.g. 3 × 1080p50 = 150 fps from an HFR camera's 3G-SDI phase outputs,
  arriving via mxl-decklink or mxl-st2110-gateway). Configuration: number of phases,
  phase order (receiver → phase index), and the phase offset (default `k/N` of the
  house frame period for phase k). The recorder interleaves the phases into one HFR
  timeline (frame time = grain TAI time + phase offset).
  - A phase that is missing or late is detected per grain; playback then repeats the
    neighbouring phase and counts it (metric). Recording continues.
  - The UI shows a phase-alignment check: a moving test pattern (e.g. from
    mxl-test-player) must move monotonically across interleaved frames.
- Audio of HFR cameras comes from one designated receiver (phase 1 by default).

### 4.3 Storage

- Ring buffer on a local NVMe volume (`REPLAY_STORAGE_DIR`, hostPath on the node),
  one segment file series per camera (e.g. 10 s segments) plus the SQLite index.
  RAM holds only a frame index; frames are read back from the segments. Expired
  segments are deleted unless a clip touches them; a restart indexes the existing
  segments again.
- Video stored as **JPEG 4:2:2** per frame (intra-frame, so any frame decodes
  independently), quality configurable (default 92), via nvJPEG or CPU fallback.
  Audio stored as float32 PCM per camera.
- **Buffer duration** per camera configurable (default 2 h). On start the recorder
  checks free space and refuses buffer durations that do not fit, with a clear
  message. Sizing formula and measured bitrates per format documented in the README
  (expected order of magnitude: 1080p50 at quality 92 ≈ 150 Mbit/s ≈ 70 GB/h per
  camera; HFR scales with frame rate).
- **Protected ranges**: frames referenced by clips are never overwritten; the ring
  skips them. If protected ranges fill the storage beyond `REPLAY_PROTECT_MAX_PCT`
  (default 50 %), new clip creation warns and requires export or deletion.
- Write path is sequential and O_DIRECT-friendly; reads for playback are random by
  frame. Storage throughput is measured at start (warning if below the configured
  load) and exposed as metrics.

---

## 5. Playout channels

### 5.1 Channels

- `REPLAY_CHANNELS` playout channels (default 2: e.g. "PGM" and "PVW/second LSM").
  Each is one video sender, one audio sender and one ANC (timecode) sender, grouped,
  writing into the replay's own domain (stable ID, never a mirror domain).
- Output format = house format (`REPLAY_FORMAT`, default 1080p50); all channels and
  inputs use the same raster. Inputs with another raster are scaled on decode
  (bilinear/bicubic on GPU), and this is shown as a warning.
- The output clock is TAI: one grain per output grain index, always. When nothing is
  playing, a channel shows its idle source (configurable: last frame, black, or a
  live input "E2E").

### 5.2 Live slow-motion control

- **Live / E2E**: channel follows a camera live (minimal delay).
- **Scrub**: move the play position back in the buffer by frames, seconds or with a
  jog control; the picture follows frame-accurately (paused).
- **Mark IN / OUT** at the current position; **Go to IN**, **Go to OUT**.
- **Play** from the current position at the current speed; **Pause**; **Back to live**.
- **Speed**: continuous 0–200 % via a speed fader (UI slider, mouse wheel, keys),
  plus presets 25, 33, 50, 66, 75, 100 %. Speed changes ramp over a configurable
  number of frames (default 3) to avoid jumps. Reverse playback (−100…0 %) MAY be
  supported if trivial; otherwise listed as a candidate.
- **Angle switch**: change camera at the same TAI position while paused or playing;
  the channel continues seamlessly from the other camera.
- **Lock channels** (optional): channel 2 follows channel 1's position with another
  camera (for split-screen or second angle).

### 5.3 Slow-motion modes

Per channel (and stored per clip), `motion_mode`:

| Mode | Behaviour |
| --- | --- |
| `repeat` | nearest source frame, repeated (no artefacts, judders at low speeds) |
| `blend` | linear blend of the two neighbouring source frames weighted by phase |
| `interpolate` (default when a GPU is present) | optical-flow interpolation at the exact phase (§6) |

- HFR cameras: as long as `speed ≤ 100 % / hfr_factor` is not needed, real frames
  are used; interpolation only fills the remaining phases (e.g. 25 % on a 150 fps
  camera: 3 real frames per 4 output frames, … – the scheduler picks the nearest real
  frame if it is within `hfr_snap` (default 0.1 frame) of the wanted time, else
  interpolates between real HFR frames).
- At 100 % speed on a house-rate camera no processing is done (source frames only).
- The interpolation engine is invoked only when needed; idle channels use no GPU.

### 5.4 Audio

- At 100 %: audio plays normally, frame-accurate with video.
- Below 100 % (default `slowmo_audio=mute`): audio fades out over 2 frames. Options:
  `mute` (default), `stretch` (pitch-preserving time-stretch, phase vocoder with WSOLA
  fallback), `follow` (plain resample, pitch changes — for effect only).
- Above 100 %: `mute` or `stretch`.
- Audio comes from the camera being played (angle switch switches audio), or from a
  fixed "atmos" camera (configurable per channel).

### 5.5 Timecode

- The ANC sender carries SMPTE 12M timecode (ATC, same payload format as
  mxl-test-player/mxl-decklink): either the **source** timecode (recording TAI time
  of the frame being shown — so downstream systems see when the action happened) or
  the **output** time of day; configurable per channel (default source).
- The same value is shown in the UI.

---

## 6. Interpolation engine (CUDA port of Futatabi)

### 6.1 Algorithm

Port Futatabi's interpolation pipeline from OpenGL compute shaders to CUDA, keeping
the algorithm and its parameters:

1. image pyramid (grayscale/luma, several levels);
2. **Dense Inverse Search** patch optimisation per level (inverse compositional
   search on overlapping patches);
3. densification (patches → per-pixel flow);
4. **variational refinement** (iterative solver);
5. forward and backward flow between the two source frames;
6. **occlusion-aware interpolation** at phase `t ∈ (0, 1)`: splat/warp both frames
   to time t, handle holes and occlusions as Futatabi does, blend.

- Arbitrary `t` per output frame (no frame-doubling restriction).
- **Flow reuse**: the flow pair between two source frames is computed once and reused
  for every output frame between them (e.g. 3 output frames at 25 %); cached per
  channel.
- Operating resolution of the flow (full, half) and number of pyramid levels are
  configurable quality presets (`fast`, `balanced` default, `quality`).
- The port is a derivative work of Futatabi: GPL-3.0-or-later, original copyright
  headers kept.

### 6.2 Flow modules

The flow step (1–5) is a module behind one interface:

- `dis-cuda` (default): the CUDA port above.
- `ofa`: NVIDIA's hardware optical flow engine via the Vulkan extension
  `VK_NV_optical_flow` (the route used by FFmpeg's `fruc_vulkan` filter), grid 2×2
  or 4×4, upsampled to a dense flow; interpolation (step 6) stays in CUDA. Purpose:
  free the CUDA cores for other media functions sharing the GPU. Available on
  Ampere and newer (A4000, L4).
- Selectable per channel; the UI shows which module is active and its timing.

### 6.3 Performance targets (to be measured and recorded)

- A4000 and L4: number of concurrent 1080p50 `interpolate` channels with
  `dis-cuda` `balanced` and with `ofa`; feasibility of 2160p50.
- Per output frame: total GPU time (decode + flow + interpolation + pack) < 50 % of
  the frame period at the target channel count; zero late output grains over 1 h.
- GPU utilisation (SM, OFA engine, memory) per channel, published in the README.

### 6.4 Reference harness (required)

- Build original Futatabi (OpenGL/EGL) in a separate CI/test image (GPL, same license).
- A tool runs the **original** and the **port** on the same frame pairs and phases
  (test sequences: sports clips from Futatabi's sample data or own recordings,
  synthetic moving patterns) and compares output frames with PSNR and SSIM.
- Acceptance: port vs original PSNR ≥ 40 dB (or another threshold justified in
  `IMPLEMENTATION_PLAN.md` after first measurements) on all test sequences; any
  systematic difference explained.
- The same harness compares `dis-cuda` vs `ofa` (quality and speed) and writes a
  report to `docs/benchmarks/`.

---

## 7. Clips, library and files

- **Create clip** from IN/OUT on a channel: stores camera(s), IN, OUT, speed,
  motion mode, audio mode, name, colour, tags. Option "all angles": one clip per
  camera at the same range (grouped).
- Clips reference buffer frames (protected, §4.3); **Export** writes a clip to a
  file (intra-frame mezzanine, e.g. ProRes 422 HQ or JPEG MOV, plus WAV) for
  archiving; **Consolidate** copies a clip into library storage so the ring can be
  released.
- **Upload** files (video/still) like mxl-test-player: converted at upload to house
  format and stored as JPEG frame sequence + PCM, so they behave exactly like buffer
  clips (any speed, interpolation, frame-accurate).
- **Playlists**: ordered clips with per-entry speed, end action, auto-advance;
  next clip preloaded; cut transitions in v1.
- End action per clip: `freeze` (default), `black`, `loop`, `next`, `return-to-live`.
- Library search by name/tag/camera/time; thumbnails per clip.

---

## 8. User interface

### 8.1 Shotbox (main screen — clean and simple)

- A grid of large buttons, one per clip (or playlist), each with thumbnail, name,
  duration and speed badge. Grid size adapts to the screen; pages/banks of buttons.
- **Target channel** selector at the top (1 or 2), with a live thumbnail of that
  channel's output.
- One click on a button **cues** the clip (button turns yellow), a second click
  **plays** (green, with progress bar); while playing, a click **pauses**. When it
  ends: grey with the end action applied. Option "play on first click".
- Nothing else on this screen except: big **Back to live** button, speed preset
  buttons, and a small status line (recording OK, storage, GPU).
- Keyboard: number keys select buttons, Space play/pause, Enter cue/play.

### 8.2 LSM / edit screen

- Camera row with live thumbnails (low-rate) and recording state; click selects the
  camera for the active channel (angle switch).
- Channel monitor (low-rate JPEG preview; full-motion preview via
  mxl-webrtc-monitor routed to the channel output).
- Timeline of the buffer with markers for clips; scrubbing by drag, mouse wheel
  (frame/second), keyboard (←/→ frame, shift = second).
- Buttons: IN, OUT, Go IN, Go OUT, Play, Pause, Live, Create clip, speed fader with
  presets, motion mode, audio mode.
- Clip inspector: edit IN/OUT frame-accurately, speed, modes, end action, name,
  colour, tags.

### 8.3 Other pages

Library (clips, uploads, conversion queue, export), Playlists, Cameras (inputs,
phase groups, buffer settings, storage status), NMOS, Settings (config
import/export as one JSON document) — like the siblings.

### 8.4 Controllers

- Keyboard shortcuts (configurable map).
- Optional: USB jog/shuttle controllers via the browser's WebHID API (e.g.
  Contour ShuttlePro), mapped to scrub, speed and buttons. Out of scope: dedicated
  LSM remote panels.

### 8.5 API

REST under `/api/v1/…` (cameras, channels: transport/position/speed/angle, clips,
playlists, library, uploads, exports, config) and WebSocket `/api/v1/events`
(positions, states, thumbnails at low rate, alarms) so several UIs stay in sync and
other systems (e.g. a control surface or a vision mixer macro) can drive the replay.
`/livez`, `/readyz`, `/statusz`, `/metrics` on `WEB_PORT`. HTTP starts before the
retained segments are indexed: `/livez` answers at once, while `/readyz` and the API stay
503 until the index is built and, when a registry address is set, until the node is
visible on the Query API.
`GET /api/v1/config/export` and `POST /api/v1/config/import` exchange one JSON
document. There are no secret settings.

---

## 9. NMOS

- One Node, one Device; deterministic UUIDv5 IDs from `NMOS_SEED`, including the
  default output domain id. `NMOS_LABEL` sets the node label and the device label.
  `NMOS_TAGS` is added to the node and the device. Group hints stay on the resources.
- Inputs: per camera video + audio receivers; phase groups: N video receivers + one
  audio receiver, grouped (`<camera>:Video Phase k`).
- Outputs: per channel video, audio and data (`video/smpte291`) senders with Sources
  and Flows, grouped (`<channel>:Video|Audio|Data`). Each sender's active connection
  carries `mxl_domain_id` and `mxl_flow_id`. Flow IDs change only when the
  house format changes.
- Receivers accept an IS-05 PATCH (`sender_id`, `master_enable`, `transport_params`,
  `activate_immediate`). `master_enable: false` stops reading. The active connection
  is restored from `REPLAY_STATE_DIR` after a restart.
- Static registry. `NMOS_DNS_SD` defaults to false and then disables DNS-SD browse
  and mDNS advertisement. Query API defaults to the registry address on
  registration port + 1. The node href and API endpoints use `NMOS_HOST_ADDRESS`.
- The Node API listens on `NMOS_PORT`. The events WebSocket listens on `NMOS_PORT + 1`.

---

## 10. Configuration, metrics, deployment

| Key | Default |
| --- | --- |
| `REPLAY_FORMAT` | `1080p50` |
| `REPLAY_INPUTS` / `REPLAY_CHANNELS` | 4 / 2 |
| `REPLAY_STORAGE_DIR` | `/data/replay` (local NVMe) |
| `REPLAY_BUFFER_HOURS` | 2 (per camera, overridable) |
| `REPLAY_JPEG_QUALITY` | 92 |
| `REPLAY_PROTECT_MAX_PCT` | 50 |
| `REPLAY_FLOW_MODULE` | `dis-cuda` (`ofa` optional) |
| `REPLAY_INTERP_PRESET` | `balanced` |
| `REPLAY_SLOWMO_AUDIO` | `mute` |
| `REPLAY_STATE_DIR` | `/config` (catalog, imported settings, IS-05 routes) |
| `MXL_DOMAIN_SCAN_PATH` / `MXL_OUTPUT_DOMAIN_DIR` / `MXL_OUTPUT_DOMAIN_ID` | `/Volumes/mxl` / `/Volumes/mxl/replay-<seed-short>` / UUIDv5 from `NMOS_SEED` |
| `MXL_HISTORY_DURATION` / `MXL_CLEANUP_ON_EXIT` | 2000000000 ns / false |
| `NMOS_REGISTRY_ADDRESS` / `_PORT`, `NMOS_QUERY_ADDRESS` / `_PORT` | empty / 3210, registry address / registry port + 1 |
| `NMOS_DNS_SD`, `NMOS_PORT`, `NMOS_SEED`, `NMOS_LABEL`, `NMOS_TAGS`, `NMOS_HOST_ADDRESS` | false, 3302, `HOST_ID-replay`, unset, `{}`, first non-loopback IPv4 |
| `WEB_PORT` / `SHUTDOWN_TIMEOUT_S` | 8150 / 10 |

Metrics (prefix `mxl_replay_`): per camera `record_fps`, `record_dropped_total`,
`storage_write_failed_total` (frames lost to a full disk or an I/O error),
`phase_missing_total`, `buffer_seconds`, `jpeg_encode_seconds`; per channel
`channel_state`, `speed`, `motion_mode` (info), `late_grains_total`,
`frame_gpu_seconds` (histogram by stage: decode, flow, interpolate, pack),
`flow_cache_hits_total`; storage `write_bytes_per_second`,
`read_bytes_per_second`, `free_bytes`, `protected_bytes`; GPU SM/memory/OFA
utilisation. Grafana dashboard in `deploy/grafana/`.

Deployment:
- Pod network; MXL root `hostPath`; **node-pinned** with local NVMe `hostPath` for
  storage (documented: dedicated NVMe recommended, not the OS disk).
- GPU required for `interpolate` (CPU fallback only offers `repeat`/`blend` and
  fewer inputs): `nvidia.com/gpu: 1`, `runtimeClassName: nvidia`,
  `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility` (add `graphics` only for the
  `ofa` module if the Vulkan path needs it — verify and record).
- Compose demo: registry, two mxl-test-player outputs as "cameras" (motion and
  field-order patterns), the replay, mxl-webrtc-monitor on the channel outputs; and a
  Kubernetes Deployment that `mxl-poc-platform` can vendor.
- CI publishes `ghcr.io/leeo86/mxl-replay`: `nightly-dev` and `git-<sha7>` from `main`,
  and `X.Y.Z` / `X.Y` / `X` from a `vX.Y.Z` tag. Labels include
  `org.opencontainers.image.source`, `.revision`, `.licenses`, and `io.dmf.mxl.revision`.
  The reference-harness image is built in CI but not published by default.
- Pod network, uid 1000, no `hostIPC`. On SIGTERM: release MXL readers and writers,
  DELETE the node from the registry, and with `MXL_CLEANUP_ON_EXIT=true` remove only
  the function's own output domain. Exit codes 0, 75 (including a port that will not
  bind), 78, 143.

---

## 11. Testing

- Unit: frame scheduler (speed → source time per output grain, ramps, angle
  switch continuity), HFR phase interleaving and missing-phase handling, ring buffer
  with protected ranges, clip/playlist state machine, audio cadence and time-stretch
  length, JPEG round trip, config precedence, ID derivation.
- Interpolation: the reference harness (§6.4) in CI on a small sequence set (GPU
  runner if available; otherwise documented manual run), full set before releases.
- Integration (CI, CPU paths where possible): record two test-player flows for 60 s;
  create a clip; play at 50 % in `repeat` and `blend`; verify output grain count
  equals TAI elapsed, frame-accurate IN/OUT, angle switch at the same TAI position,
  timecode ANC matches source time; upload a file and play it as a clip; NMOS IS-05
  activation of an input before the flow exists.
- NMOS conformance: AMWA IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01.
- Hardware (documented): performance targets (§6.3), storage throughput with all
  cameras recording while two channels play, phased HFR with a real HFR camera or a
  simulated phase set.

## 12. Implementation order

1. Skeleton, config, ops, metrics, CI; recorder with CPU JPEG and SQLite index.
2. Playout channel with `repeat` and `blend`, live/scrub/IN/OUT/speed, TAI output.
3. NMOS receivers/senders, ANC timecode.
4. nvJPEG paths; clips, protection, shotbox UI, LSM screen.
5. Reference harness with original Futatabi; CUDA port of the flow and
   interpolation; `interpolate` mode.
6. Audio modes (mute, stretch), playlists, uploads, export.
7. HFR native and phased.
8. `ofa` flow module and benchmarks.

## 13. Candidates for later

Reverse playback, mix/wipe transitions in playlists, multi-server clip sharing,
LSM remote panel support, EVS-style "highlight" auto-clip from tally/markers,
compressed (proxy) export, 10-bit intra storage (e.g. JPEG XS) if 8-bit JPEG
shows banding on gradients.
