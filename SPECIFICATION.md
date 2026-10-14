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
- A connected input that records nothing for `REPLAY_INPUT_STALL_S` (its writer
  stopped, the flow was removed, a fabrics mirror lost its link) logs
  `recording_stopped` once (camera, phase, reason). Recording resumes by itself when
  grains come back, also on a flow that was removed and created again (a writer or
  fabrics agent restart, new inode), and logs `recording_resumed` with the gap.
  Nothing is stored for a gap: a position inside it shows the nearest recorded
  frame, and its grains are not dropped ones (§10).
- A camera whose audio flow delivers no samples for `REPLAY_INPUT_STALL_S` worth of
  video frames while its video keeps recording logs `recording_audio_stopped` once
  (camera, reason: `flow_missing` or `no_samples`), and `recording_audio_resumed` with
  the gap when samples come back (1.4.0). The frames of the gap are stored without audio.
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

- Ring buffer on a dedicated local NVMe volume (`REPLAY_STORAGE_DIR`, hostPath on the
  node; required, see §10),
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

- `REPLAY_CHANNELS` playout channels (default 2), labelled `LSM A`, `LSM B`, `LSM C` … unless
  `CH<n>_LABEL` is set (1.5.0; before `PGM`, `PVW`, `CH3` …; the NMOS senders follow:
  `LSM A Video`, `LSM A Audio`, `LSM A Data`).
  Each is one video sender, one audio sender and one ANC (timecode) sender, grouped,
  writing into the replay's own domain (stable ID, never a mirror domain).
- Output format = house format (`REPLAY_FORMAT`, default 1080p50); all channels and
  inputs use the same raster. Inputs with another raster are scaled on decode
  (bilinear/bicubic on GPU), and this is shown as a warning.
- The output clock is TAI: one grain per output grain index, always. A channel more than
  two grains late continues at the current grain (a resync): counted per channel in
  `output_resyncs_total`, logged as `output_resync` at most once per 10 s (1.4.0). The playout
  never waits for an optical flow (§5.3, 1.4.1), and its GPU work goes before the recording's,
  which goes before the flows'. When nothing is
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
- Flows are computed ahead (1.4.1). A worker per channel computes the flows of the source
  pairs the next 8 grains interpolate (a paused channel: its own pair, and the first pairs
  of a play), one at a time, on a CUDA stream below the playout's and the recording's. A
  command (cue, play, speed, angle, mode) plans them at once; a pair is planned once its
  frame B is recorded. A pair the worker cannot finish before its last grain (one flow takes
  the shortest of the last three) is skipped for the next one; when none can, one is
  computed once a second.
- The playout MUST NOT wait for a flow: a grain whose flow is not ready is rendered as
  `blend` and counted (`interpolation_fallbacks_total{channel}`, logged as
  `interpolation_fallback` at most once per 10 s with the count and the last flow's time).
  This is the degradation under GPU contention: the channel stays on time and the recording
  does not drop. Without contention steady play at 25 % and 50 % has no fallbacks. Two
  cases do: the first 1–3 grains of a play that starts within about two flow times of its
  cue (lab A16: 22 ms per flow), and a few grains when the replay alone saturates the GPU
  (75 % on an A16: 16 of 1100 grains in 30 s).

### 5.4 Audio

- At 100 %: audio plays normally, frame-accurate with video.
- Below 100 % (default `slowmo_audio=mute`): audio fades out over 2 frames. Options:
  `mute` (default), `stretch` (pitch-preserving time-stretch, phase vocoder with WSOLA
  fallback), `follow` (plain resample, pitch changes — for effect only).
- Above 100 %: `mute` or `stretch`.
- A channel that stands still (paused, cued, ended with *freeze* or *black*, speed 0) is
  silent in every mode: its audio fades out over 2 frames and fades in over 2 frames when
  it plays or goes live (1.5.0; it repeated the frame's samples before). Live is unchanged.
- Audio comes from the camera being played (angle switch switches audio), or from a
  fixed "atmos" camera (configurable per channel).
- **Playlist transitions** (1.6.0, §7): a transition into an entry crossfades the two entries'
  audio over `audio_frames` with **equal power** (`out = cos(π/2·t)`, `in = sin(π/2·t)`,
  `t = (n + 0.5)/S` per sample over the crossfade's `S` samples). Two entries are different
  moments or cameras, uncorrelated programme: equal power keeps the loudness across the join,
  where a linear crossfade dips by 3 dB in the middle. Both gains are continuous and start and
  end within half a sample step of 1 and 0: no click. Each side reads its own audio at its own
  position with its own speed and mode; an entry with `own_audio` off is silent (with the 2-frame
  fade). A channel that stops during a crossfade ends it with a 1-frame fade of the outgoing side.
- **Aux track** (1.6.0): a playlist's `aux` plays under its entries while the channel is on
  the playlist and not live: a clip's audio (IN to OUT) or an **audio item** (§7). It starts at
  its beginning when the playlist starts (play, cue, shotbox, idle) and runs at 100 %, whatever
  the entries' speeds; with `loop` it starts again at its end, else it stops. Its gain per sample
  is `level_db` × duck × end fade × edge: the **duck** ramps in dB to `duck_db` (−60…0, default
  −12) over `attack_ms` (200) while the current entry's own audio is audible (own audio on, not
  muted by slow motion or a stop; the outgoing entry's during a crossfade) and back over
  `release_ms` (500); the **end fade** is linear to silence over the playlist's last
  `fade_out_frames` (50; from the countdown `playlist_remaining_ns`, so a looped playlist does not
  fade); the **edges** are 5 ms fades at the source's start, end and loop point. Paused, cued or
  stopped the aux pauses with the channel (2-frame fades). CPU only: per grain one read (a ring
  frame's audio or a `pread` of the item's file) and a few operations per sample.

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
  clips (any speed, interpolation, frame-accurate). A file with an audio stream and no
  video (WAV, MP3, AAC, FLAC: anything FFmpeg decodes) is an **audio item** (1.6.0):
  resampled to 48 kHz stereo float and kept as `<library>/audio/<id>.f32` (ids `audio-N`;
  `GET /api/v1/audio`, `GET`/`PUT {"name"}`/`DELETE /api/v1/audio/{id}`; deleting one removes
  it from the playlists that use it as aux). Items are files: they are not in the config export.
- **Playlists**: ordered clips with per-entry speed, end action, auto-advance;
  next clip preloaded; cut transitions in v1, **mix** transitions since 1.6.0. A playlist can **loop** (1.6.0): after its
  last entry it starts again at its first (`loop` in the API, the catalog and the config
  export). With `loop` on, the last playable entry's `freeze` or `black` acts as `next`
  (the list wraps and plays on); that entry's own `loop` (the clip repeats) and
  `return-to-live` still apply.
- End action per clip: `freeze` (default), `black`, `loop`, `next`, `return-to-live`.
- **Transitions** (1.6.0). Each entry has a transition **into** it from the entry before (the
  edit-list convention; the first entry's applies when a looped or idle playlist wraps to it):
  `transition` `cut` (default) or `mix`, `transition_frames` (a mix: 1–250, default 25),
  `audio_frames` (the audio crossfade, 0–250; −1, the default, is the mix's length, 2 frames on a
  cut, so a cut does not click; 0 is a hard audio cut)
  and `own_audio` (default true). A transition takes `L = max(transition_frames, audio_frames)`
  frames and starts on the grain where the outgoing entry has `L` grains left to its OUT at
  its speed, when that entry continues into the next one with *next* and auto-advance (or the
  loop wrap, or the idle playlist); otherwise the join is a cut as before. At that grain the
  channel **moves to the incoming entry** (clip, index, speed, modes, status, countdowns) and
  keeps the outgoing side as it plays on to its OUT (and holds there). The two entries overlap
  by `L`: the countdowns and `duration_ns` take it off. Grain `k` of an `N`-frame mix shows
  `(k+1)/(N+1)` of the incoming picture over the outgoing (never a pure picture inside the
  mix); each side shows its nearest source frame at its own position, as `repeat` (exact at
  100 % on a house-rate camera; a side is not interpolated inside a mix). On the GPU the two
  JPEGs are decoded and blended by the blend path without a flow (one extra nvJPEG decode per
  grain of a mix, nothing outside one); on the CPU in 1/1024 steps. A cut with `audio_frames`
  cuts the picture at the start of the overlap and crossfades the audio over it. The audio is in
  §5.4. Per channel the status has `own_audio`, `transition` (`null` or `kind`, `frames`,
  `audio_frames`, `done`) and `aux` (`null` or `source`, `gain_db`, `ducked`, `position_ns`);
  `mxl_replay_transitions_total{channel,kind}` counts them and `transition` logs each.
- **Aux track** per playlist (1.6.0, §5.4): `aux` `{source, level_db, loop, fade_out_frames,
  duck_db, attack_ms, release_ms}` in `POST`/`PUT /api/v1/playlists` (a partial object on `PUT`
  changes only its fields; `source` `""` removes it), the answers, the catalog and the config
  export. `GET /api/v1/playlists` adds each playlist's `aux_source`.
- **Idle playlist** (1.6.0): per channel `CH<n>_IDLE_PLAYLIST` (a playlist id; Settings;
  `POST /api/v1/channels/{n}/idle-playlist {"playlist": id}`, `""` clears it; kept in the
  state directory and the config export; deleting the playlist clears it). It plays, looped:
  - when it is set on an idle channel: stopped, ended or black, or playing its idle playlist.
    A live, scrubbed, playing, cued or paused channel keeps its shot; the idle playlist is armed and
    starts at the next clip end, stop or playlist end. Also at start, once the buffer is indexed;
  - when the channel is **stopped** (`stop`: `POST /api/v1/control {"action":"stop"}`,
    transport command `stop`, *Stop* in the shotbox and the transport widget; a paused idle
    playlist plays on). Without an idle playlist, `stop` ends the shot where it stands
    (`ended`, paused); a live channel stays live;
  - when a clip or a playlist ends: in place of `freeze` and `black`, and of the end of a
    playlist (`next` after the last entry, not looped). `loop` and `return-to-live` keep
    their behaviour; an entry without auto-advance still waits cued.
  In the idle playlist every entry plays on (end actions and auto-advance are not used) and
  it starts again after its last entry. A cue, a play, live or a pause take the channel as
  before. Status per channel: `idle_playlist`, `idle` (it plays now); log `idle_playlist`.
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
- Nothing else on this screen except: big **Back to live** and **Stop** buttons, speed preset
  buttons, the countdowns, the filter, and a small status line (recording OK, storage, GPU).
- Keyboard: number keys select buttons, Space play/pause, Enter cue/play.
- **Countdowns** (1.6.0) for every channel: the playing time to the clip's OUT and to the
  playlist's end at the speed set (status `clip_remaining_ns`, `playlist_remaining_ns`; null
  when there is nothing to count or the speed never gets there: 0, or an entry that loops).
  The playlist ends after the first entry that does not end with `next`, else after its last
  (looped or idle: the end of this pass); the time an entry waits cued is not counted. The
  countdown turns red under 5 s while the channel plays. Also in the transport widget.
- **Filter** (1.6.0): all, clips, or playlists (kept in the browser).
- A playlist's button stacks the thumbnails of its first three clips (1.6.0;
  `GET /api/v1/playlists` gives each playlist's `clip_ids`).
- **Stop** (1.6.0) next to *Back to live*: the idle playlist (§7).

### 8.2 LSM / edit screen

- Camera row with live thumbnails (low-rate) and recording state; click selects the
  camera for the active channel (angle switch).
- Channel monitor (low-rate JPEG preview, or the full-motion WebRTC mosaic of §8.6;
  full-motion preview also via mxl-webrtc-monitor routed to the channel output).
- Timeline of the buffer with markers for clips; scrubbing by drag, mouse wheel
  (frame/second), keyboard (←/→ frame, shift = second).
- Buttons: IN, OUT, Go IN, Go OUT, Play, Pause, Live, Create clip, speed fader with
  presets. The channel modes are in Settings (§8.3, 1.5.0).
- Clip inspector: edit IN/OUT frame-accurately, speed, modes, end action, name,
  colour, tags.

### 8.3 Other pages

Library (clips, uploads, conversion queue, export; audio items, 1.6.0), Playlists (per
entry the clip's IN thumbnail, the transition in and own audio; the aux track, 1.6.0), Cameras (inputs,
phase groups, buffer settings, storage status), NMOS, Settings (per channel: motion,
audio and timecode mode, lock to the first channel, play on red light (§8.8) and the
tally; config import/export as one JSON document) — like the siblings.

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
Clips are edited with `PATCH /api/v1/clips/{id}` and playlists read, replaced and
deleted at `/api/v1/playlists/{id}` (with the entries' transitions and the `aux` track, 1.6.0);
audio items are listed at `/api/v1/audio` and renamed or deleted at `/api/v1/audio/{id}`; `POST /api/v1/playlists/{id}/play {"channel"}` plays one
and `…/cue` cues its first clip, paused (a shotbox click on it then plays). Low-rate JPEG previews are served per channel,
per camera and per clip (`…/preview.jpg`, `/api/v1/clips/{id}/thumbnail.jpg`; in the
WebRTC mode of §8.6 only the clip thumbnails, and `/api/v1/preview/map`); the
status carries the timecodes the UI shows (§5.5), formatted by the server.
`/livez`, `/readyz`, `/statusz`, `/metrics` on `WEB_PORT`. HTTP starts before the
retained segments are indexed: `/livez` answers at once, while `/readyz` and the API stay
503 until the index is built and, when a registry address is set, until the node is
visible on the Query API.
`GET /api/v1/config/export` and `POST /api/v1/config/import` exchange one JSON
document. There are no secret settings.

---

### 8.6 Previews: JPEG or one WebRTC mosaic (1.4.0)

`REPLAY_PREVIEW_MODE` selects how the UI shows cameras and channels. Never both.

- `jpeg` (default, unset): the pictures of §8.5, made on request.
- `webrtc`: **one mosaic** of every channel output and every camera input (phase 1),
  composed on the GPU (on the CPU without one) and encoded **once** as H.264 with NVENC;
  libx264 only when NVENC cannot be opened, which is logged (`preview_nvenc_unavailable`).
  The camera and channel JPEG pictures answer 404; clip thumbnails stay. No audio.
  - Layout: channels at 640×360, three a row, then cameras at 480×270, four a row; the
    canvas is as wide as its widest row and is scaled down to fit 1920×1080 when taller.
    Every edge is even. `GET /api/v1/preview/map` gives the mode, canvas size, frame rate,
    stream path, the WHEP and HLS URLs and each tile (`id` `ch<n>` / `cam<n>`, `kind`,
    `index`, `label`, `x`, `y`, `w`, `h`).
  - Rate: every house grain up to 30 per second, every second one above (25 at 50p).
    Each camera's reader and each channel's playout thread draw their own tile when their
    grain is due: from the grain already on the GPU (area average), else point-sampled on
    the CPU. Nothing waits for anything else; a tile may show parts of two pictures.
  - The UI opens one WHEP session per page and shows each picture as a `<video>` on that
    one `MediaStream`, cropped to its tile in every browser: a box with the tile's aspect
    ratio and `overflow: hidden`, the video in it scaled and moved by a CSS transform
    (CSS `object-view-box` crops only in Chrome and Edge).
- **Preview contract** (platform §11.5 / D-185, as mxl-webrtc-monitor 1.3.0):
  - `PREVIEW_PUBLISH_URL` (`rtsp://` or `rtsps://` base, no path or credentials): set,
    the mosaic is published there (RTSP over TCP) and no MediaMTX is started; empty, the
    image's MediaMTX runs as a supervised child (restarted 1 s after it exits, doubling to
    10 s; SIGTERM and after 3 s SIGKILL on shutdown; SIGTERM when the replay dies), with
    RTSP ingest on `127.0.0.1:MEDIAMTX_RTSP_PORT`.
  - `PREVIEW_PATH_PREFIX` (default `mxl-replay`): the stream is `<prefix>/mosaic`.
  - `PREVIEW_WHEP_URL` / `PREVIEW_HLS_URL`: public bases, `<base>/<prefix>/mosaic/whep`
    and `.../index.m3u8`; empty: the own MediaMTX on `NMOS_HOST_ADDRESS` (the page puts in
    its own host name).
  - The own MediaMTX's ports (8854 RTSP on localhost, 8689 WHEP, 8688 HLS, 8489 ICE UDP
    and TCP; no API or metrics) avoid mxl-webrtc-monitor (8554/8889/8888/8189/9997/9998),
    the FlowXer engine (8654/8989/8988/8289/9897) and mxl-multiviewer 1.4.0
    (8754/8789/8788/8389). They may not equal the web or NMOS ports (exit 78).
  - `/statusz` (and the status) has `preview`: `mode`; in WebRTC mode `publish` (`own` or
    `shared`), `publish_url`, `path_prefix`, `path`, `state` (`connecting`, `publishing`,
    `error`), `error`, `encoder` (`nvenc`, `x264`), `frames`, in own mode `mediamtx`
    (`running`, `restarts`), and `streams` (`path`, `state`, `error`). Metrics in §10.

### 8.7 Widgets (1.4.0, operator screens)

The contract of mxl-webrtc-monitor 1.3.0 §6.6.

- `GET /widgets` answers `[{id, title, params, min_size: {w, h}, version}]` (`params` a
  JSON schema of the query). It carries `Access-Control-Allow-Origin` for an `Origin`
  that `WIDGET_FRAME_ANCESTORS` lists (or `*`), and `Vary: Origin`; GET only.
- One list of widgets serves both routes: `/widgets` answers it and `/widget/<id>` accepts
  only its ids and checks the query against its `params`. A `channel` is an integer
  1..`REPLAY_CHANNELS`, a boolean `true`, `false`, `1` or `0` (schema `"type":"boolean"` with
  its `default`), a `playlist` a playlist id (`^[A-Za-z0-9._-]{1,64}$`; one that does not
  exist is shown as such, it may be deleted later).
- `transport` (`channel`, required; `clip`, default false; `presets`, default true;
  `min_size` 480×180, 480×160 before 1.6.0): one channel's label, state and timecode, the
  countdowns (§8.1), Cue (to its IN, paused), Play / Pause, Stop (§7), Live (E2E), the speed
  fader 0–200 % and, with `presets`, the presets. With `clip`: IN, OUT, Go IN, Go OUT, the
  marks and their length, and Create clip (IN to OUT on the channel's camera with the
  automatic name; at the protection cap, 409, the operator confirms and it is sent with
  `force`). Without the new params it looks as before.
- `clip-list` (`channel`, optional; `min_size` 400×300): every clip, newest first, with its
  shotbox button on the channel (Load cues it; then Play, Pause). Without `channel` the
  widget has a channel picker.
- `playlist-editor` (`playlist`, optional; `min_size` 480×320): one playlist's entries, each
  with its clip's IN thumbnail, length, speed (1–200 %), end action, transition (*Cut*,
  *Mix 12*, *Mix 25*, *Mix 50*; 1.6.0), own audio (♪), up, down and remove, the aux track's
  source and level (1.6.0), and *Add a clip* (newest first; a new entry ends with `next` and
  auto-advances). Every change is
  saved at once (`PUT /api/v1/playlists/{id}` with the entries). Without `playlist` the
  widget has a playlist picker. A channel on the playlist marks its entry.
- `playlist-player` (`channel`, required; `playlist`, optional: a picker; `min_size`
  420×200): the channel's label, state and timecode; the countdown to the playlist's end
  (§8.1, red under 5 s while it plays), the entry playing; Cue (the first clip, paused:
  `POST /api/v1/playlists/{id}/cue {"channel"}`), Play / Pause (on this playlist: play or
  pause the channel; else `…/play`), Stop (§7) and Loop (the playlist's `loop`, saved at
  once).
- `timeline` (`channel`, required; `min_size` 600×180): the LSM tab's timeline (§8.2) for
  the channel: its camera's buffer, the clips as marks, IN/OUT and the position; drag to
  seek, the wheel ±1 frame (Shift ±1 s), the zoom; IN, OUT, Go IN, Go OUT, ±1 frame and
  Play / Pause, with the marks' timecodes and length.
- `GET /widget/<id>?<params>[&theme=dark|light|transparent]` is the embedded page with only
  that widget, no app chrome, on the replay's own API (same origin). An invalid parameter
  answers 400, an unknown widget 404. These routes carry `Content-Security-Policy:
  frame-ancestors <WIDGET_FRAME_ANCESTORS>` (default `'self'`; `;`, `,` or control
  characters exit 78) and no `X-Frame-Options`.
- The page posts `{type: "widget-ready"}` once it shows the replay's status, and
  `{type: "widget-size", w, h}` then and on every resize, to `window.parent`.

### 8.8 Tally (TSL UMD 5.0, 1.5.0)

- The receiver of mxl-webrtc-monitor 1.2.0 (a port of mxl-multiviewer's): `TSL_ENABLE`
  (default false), UDP on `TSL_UDP_PORT` (8914) and TCP with DLE/STX framing on
  `TSL_TCP_PORT` (8915), UTF-16 or ASCII text, `TSL_SCREEN` (-1: every screen) and
  `TSL_MAP` (`display:channel,…`; empty: display `i` is channel `i+1`). The ports sit next
  to the multiviewer's (8910/8911) and the monitor's (8912/8913). A port that cannot be
  bound exits 75. The source is the platform's tally controller.
- On the rising edge of red (LH, or the text tally) on a channel whose shot is cued or
  paused, the channel plays (the path of `POST /api/v1/control {"action":"play"}`), when
  `CH<n>_PLAY_ON_TALLY` is true (default; Settings, `POST
  /api/v1/channels/{n}/play-on-tally {"enable": bool}`, kept in the state directory and
  the config export). A steady red does not play again; live, playing, ended and idle
  channels are not touched. Each play logs `tally_play`.
- Status: `tsl_enable`; per channel `play_on_tally`, `tsl_lh`, `tsl_rh`, `tsl_text_tally`,
  `tally` (text, else RH, else LH) and `tally_play_ns` (TAI of the last red-light play).
  Metrics: `tsl_packets_total`, `tsl_errors_total`, `tally_plays_total{channel}`.

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
| `REPLAY_INPUT_STALL_S` | 2 (seconds without a recorded grain before `recording_stopped`) |
| `REPLAY_STATE_DIR` | `/config` (catalog, imported settings, IS-05 routes) |
| `MXL_DOMAIN_SCAN_PATH` / `MXL_OUTPUT_DOMAIN_DIR` / `MXL_OUTPUT_DOMAIN_ID` | `/Volumes/mxl` / `/Volumes/mxl/replay-<seed-short>` / UUIDv5 from `NMOS_SEED` |
| `MXL_HISTORY_DURATION` / `MXL_CLEANUP_ON_EXIT` | 2000000000 ns / false |
| `NMOS_REGISTRY_ADDRESS` / `_PORT`, `NMOS_QUERY_ADDRESS` / `_PORT` | empty / 3210, registry address / registry port + 1 |
| `NMOS_DNS_SD`, `NMOS_PORT`, `NMOS_SEED`, `NMOS_LABEL`, `NMOS_TAGS`, `NMOS_HOST_ADDRESS` | false, 3302, `HOST_ID-replay`, unset, `{}`, first non-loopback IPv4 |
| `WEB_PORT` / `SHUTDOWN_TIMEOUT_S` | 8150 / 10 |
| `REPLAY_PREVIEW_MODE` | `jpeg` (`webrtc`: §8.6) |
| `PREVIEW_PUBLISH_URL` / `PREVIEW_PATH_PREFIX` | empty (own MediaMTX) / `mxl-replay` |
| `PREVIEW_WHEP_URL` / `PREVIEW_HLS_URL` | empty (the own MediaMTX) |
| `MEDIAMTX_RTSP_PORT` / `_WHEP_PORT` / `_HLS_PORT` / `_ICE_UDP_PORT` | 8854 / 8689 / 8688 / 8489 |
| `WIDGET_FRAME_ANCESTORS` | `'self'` |
| `TSL_ENABLE` / `TSL_UDP_PORT` / `TSL_TCP_PORT` | false / 8914 / 8915 (§8.8) |
| `TSL_SCREEN` / `TSL_MAP` | -1 / empty (display `i` is channel `i+1`) |
| `CH<n>_PLAY_ON_TALLY` | true |
| `CH<n>_IDLE_PLAYLIST` | empty (none; §7) |

Metrics (prefix `mxl_replay_`): per camera `record_fps`, `record_dropped_total`
(grains the source wrote that were not recorded; grains it never wrote, before it
started or while it stopped, do not count),
`storage_write_failed_total` (frames lost to a full disk or an I/O error),
`phase_missing_total`, `buffer_seconds`, `jpeg_encode_seconds`,
`record_last_frame_age_seconds` (since the newest recorded frame, counted from the
process start at most; also `last_frame_age_s` per camera in the status); per channel
`channel_state`, `speed`, `motion_mode` (info), `late_grains_total`,
`frame_gpu_seconds` (histogram by stage: decode, flow, interpolate, pack; only `flow` is
measured, 1.4.1),
`flow_cache_hits_total`; storage `write_bytes_per_second` (what the recorder writes,
0 when nothing records), `storage_measured_bytes_per_second` (the write test at
start), `read_bytes_per_second`, `free_bytes`, `protected_bytes`; GPU SM/memory/OFA
utilisation; per channel `output_resyncs_total` (§5.1) and `interpolation_fallbacks_total`
(§5.3); previews `preview_mode{mode}`,
and in WebRTC mode `preview_publish_mode{mode}` (own, shared),
`preview_publish_state{stream,state}`, `preview_encoder{encoder}`, `preview_frames_total`,
`preview_encode_seconds` (histogram: copy, encode, send of one picture) and in own mode
`preview_mediamtx_restarts_total`; tally `tsl_packets_total`, `tsl_errors_total` and per
channel `tally_plays_total` (§8.8). Grafana dashboard in `deploy/grafana/`.

Deployment:
- Pod network; MXL root `hostPath`; **node-pinned** with local NVMe `hostPath` for
  storage. A dedicated NVMe is a requirement, not a recommendation: the OS disk, a
  rotating disk or network storage stalls the recorder (§4.3).
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
  The image carries the official MediaMTX binary (pinned, MIT, with its licence) for the
  own preview mode (§8.6); NVENC needs `video` in `NVIDIA_DRIVER_CAPABILITIES`.
- Pod network, uid 1000, no `hostIPC`. On SIGTERM: release MXL readers and writers,
  DELETE the node from the registry, and with `MXL_CLEANUP_ON_EXIT=true` remove only
  the function's own output domain, and stop the own MediaMTX. Exit codes 0, 75
  (including a port that will not bind), 78, 143.

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
- Previews (1.4.0): unit tests for the mode selection, never both, the tile map, the
  canvas, `/widgets`, the CSP and CORS headers, the resync counter and the audio stall
  watch; `tests/integration/preview.sh` runs the JPEG mode, the WebRTC mode with the
  own MediaMTX and with a separate (shared) one that goes away and comes back.
- Flows ahead (1.4.1): unit tests for the pairs ahead (25/50/75 %, reverse, a ramp, a
  paused channel, the angle in the key), the worker (order, a new plan, a pair it cannot
  finish skipped) and a grain blended and counted while its flow is not ready, then
  interpolated, also after an angle switch.
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
