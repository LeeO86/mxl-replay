# mxl-replay — implementation plan

This is the map from `SPECIFICATION.md` draft v0.1 to the tree, including every place the code does not follow a sentence literally.

## Pins

| Piece | Pin |
| --- | --- |
| MXL | `dmf-mxl/mxl` `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, fabrics off |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` |
| JPEG | libjpeg-turbo, 8-bit 4:2:2 |
| UI | Vue 3 + Vite, one embedded HTML file, no CDN |
| Tests | doctest (vendored) |

Grain index math is the same 128-bit rounding as MXL at that pin (`src/media/timebase.cpp`). Index 0 is the epoch. A unit test checks index 0 at t = 0 and index 1 at the rounded 30000/1001 period.

## Layout

```
src/config        env > file > default, exit 78
src/media         v210, JPEG 4:2:2, scale, ANC timecode, audio, FFmpeg upload
src/flow          DIS port (CPU reference) and CUDA blend/pack
src/record        ring buffer, segment files, phased HFR
src/playout       scheduler, shotbox, clip end actions
src/library       SQLite WAL catalog
src/domain        MXL root scan, mirror rejection
src/nmos          UUIDv5 ids; nmos-cpp node when REPLAY_WITH_NMOS=ON
src/mxl           readers and writers when libmxl is found
src/ops           HTTP, WebSocket, Prometheus text
src/app           recorder + playout engine
web/              Vue 3 shotbox, LSM, library, playlists, cameras, NMOS, settings
```

`replay-core` is what the unit tests link. `mxl-replay` is the process.

## Deviations

1. **JPEG storage is 8-bit 4:2:2.** nvJPEG encodes and decodes that on the device when a GPU is visible. libjpeg-turbo is the fallback used for clip playback with no device, and for unit tests. nvJPEG's baseline encoder does not offer 10-bit or 12-bit 4:2:2, so `jpegSupports10Bit()` and `jpegSupports12Bit()` stay false. Status reports `"jpeg":"nvjpeg"` or `"libjpeg-turbo"`.
2. **Reverse playback is implemented** from −100% to 0%. The scheduler already takes a signed speed, so leaving it out would have been the larger change. The UI speed fader is 0–200% as specified; the API accepts a negative speed.
3. **`interpolate` on a CPU-only process falls back to `blend`.** The spec limits the CPU fallback to repeat and blend. Set `REPLAY_ALLOW_CPU_INTERP=true` to run the same DIS port on the CPU (tests and the harness). A visible CUDA device selects the GPU blend path.
4. **OFA is probed, not wired as a second GPU context.** `ofaProbe()` dlopens `libvulkan.so.1` and looks for `VK_NV_optical_flow`. There is no CUDA–Vulkan external-memory interop in this cut, so a machine that has the extension still uses `dis-cuda` for pixels. `NVIDIA_DRIVER_CAPABILITIES` should add `graphics` before that interop is turned on. This was not verified on an A4000 or L4.
5. **The Futatabi OpenGL binary is not built in default CI.** `mxl-replay-harness` compares PPM frames (PSNR / SSIM, 40 dB gate) and can render the port. `docs/benchmarks/README.md` is the procedure for the original-versus-port run. The unit test that shifts a Gaussian clears 40 dB against a ground-truth midpoint; that is not the Futatabi comparison.
6. **A4000 / L4 channel counts are not measured here.** The targets in specification §6.3 stay the acceptance bar. No number in the README is a measured result.
7. **AMWA NMOS Testing** is `tests/nmos/amwa.sh`, not a default CI job. CI starts the node only when `REPLAY_WITH_NMOS=ON`.
8. **Query API port defaults to `NMOS_REGISTRY_PORT + 1`.** `NMOS_QUERY_ADDRESS` and `NMOS_QUERY_PORT` override it. Same default as the other media functions.
9. **Unknown environment variables are ignored.** The config file still rejects unknown keys, so a stray `NMOS_CPP_REF` in the CI environment does not exit 78.
10. **Segments are one MXLR file per 10 s**, not one file per JPEG. The write is sequential and buffered: a finished segment starts its writeback at once (`sync_file_range`) and leaves the page cache at the next rotation (`posix_fadvise(DONTNEED)`). Since 1.1.0 `REPLAY_ODIRECT` is accepted and ignored with a warning (`odirect_ignored`): `O_DIRECT` needs aligned buffers, and the 1.0.x writer's unaligned writes did not work with it.
11. **Export is a JPEG sequence plus WAV**, not ProRes 422 HQ. Upload conversion does use FFmpeg and stores the result as the same JPEG + PCM clips the buffer uses.
12. **Phase order is receiver order.** Phase k is the k-th video receiver of the camera. A custom permutation list is not a separate key.
13. **One image, GPU when the toolkit injects a device.** `docker/Dockerfile` builds on `nvidia/cuda:12.8.2-devel` and the runtime stage carries `libcudart` and `libnvjpeg` only. `libcuda` comes from the NVIDIA container toolkit (`--gpus all`, or `runtimeClassName: nvidia`). Without a device the same binary records and plays with libjpeg, repeat, and blend. CI compiles that same CUDA binary (`ldd` must show `libnvjpeg`); it does not publish a second CPU image. On the device path the host sees the JPEG bitstream and the finished v210 grain. Search, refinement, blend, and packing stay in device memory, and the flow pair is cached there. `.github/workflows/container.yaml` publishes `ghcr.io/leeo86/mxl-replay:nightly-dev` from `main`, `git-<sha7>` on every published build, and `X.Y.Z`, `X.Y`, `X` from a `vX.Y.Z` tag. `nightly-dev` is the only tag that moves.
14. **The default output domain id matches the NMOS domain id** (`mxl-replay/<seed>/domain`). A deployment that previously relied on the shorter derived id without setting `MXL_OUTPUT_DOMAIN_ID` gets a new domain directory. The platform sets the id explicitly.
15. **Config export is one JSON document.** The earlier `KEY=value` body is not returned. `POST /api/v1/config/import` accepts the JSON document.
16. **The catalog lives in `REPLAY_STATE_DIR` (default `/config`).** An `index.sqlite` still sitting in `REPLAY_STORAGE_DIR` is copied across on the first start.
17. **Retention deletes whole segments** instead of overwriting the oldest ones in place. A segment goes when its newest frame is older than the camera's buffer duration behind the newest recorded frame and no clip touches it. Disk use stays at the duration plus one segment plus the clips. Segment names carry the first frame's TAI (`seg-<20 digits>.bin`), so a restart orders them without an index file; the 1.0.x files (`cam<N>_seg<k>.bin`, write-only, renumbered on every start) are removed at the first start.
18. **Camera audio rides on the video frames** (1.2.0). The phase-1 reader reads, for video grain N, the samples `[audioSamplesUntil(N), audioSamplesUntil(N+1))` of the camera's audio flow (MXL addresses them by their end index) and stores them with frame N as interleaved stereo: the first two channels, mono doubled. A flow that is not 48 kHz sets the audio receiver to `unsupported`. Playout reads the audio of the frame on the house time nearest the position (phase 1 of an HFR camera) and writes the output samples at their sample index, one ring per channel. Same `[index-count, index)` convention as mxl-decklink.
19. **Uploads have their own buffer** (1.2.0): `library/frames`, camera `0` (`kLibraryCamera`). A camera buffer only appends, so the 1.1.0 uploads at TAI 1 ns onward were refused once the camera had recorded. Uploads are placed on the frame grid after the library's newest frame; a retention of 1 ns keeps only the segments a clip touches, and deleting a clip frees them. Clips uploaded with 1.1.0 keep camera 1.
20. **Start order is HTTP, index, NMOS, MXL** (1.2.0). `Engine` checks the storage and opens the catalog; `openBuffer()` indexes the segments and protects the clips again. Until then every path except `/livez` answers 503 `indexing`.
21. **The clip cap counts kept segments** (1.2.0): `FrameRing::protectedBytes()` is the size of the segments a protected range overlaps, because a clip keeps whole segments.
22. **No disk I/O under the engine lock** (1.2.0). `FrameRing` has its own lock for the index; reads use a duplicate of the cached descriptor after the lock is released, and the single writer (serialised by a second mutex) writes without it. The engine releases its lock for recording writes, channel frame and audio reads, and exports. The CPU fallback still JPEG-encodes under the engine lock (only without a GPU). A janitor thread per buffer starts the writeback of a finished segment (`sync_file_range(SYNC_FILE_RANGE_WRITE)`), drops the one before from the page cache and deletes expired files: on the lab's SAS disk the writeback start alone blocked for up to 1 s (strace, 4 cameras at 1080p50).
24. **Playout is locked to the TAI grid** (1.2.0). Each channel thread renders grain N for `indexToTimestamp(N)` and sleeps until grain N+1 starts; more than two grains late, it continues at the current grain. Live position is then exactly `N - REPLAY_LIVE_DELAY_FRAMES` (an exact pick, no interpolation), and every grain index is written once.
25. **Late camera audio is waited for** (1.2.0, 1.2.1): up to one frame (`mxlFlowReaderGetSamples` with a timeout) until three reads in a row missed; then non-blocking, with one waiting read every 50 frames. A stalled audio flow costs the video at most one frame per 50, and a writer that is always a little late is still picked up.
26. **Live waits for its frame** (1.2.1). The recorder stays two grains behind (reading one behind found the player's audio not yet committed), so frame M−2 becomes readable when grain M starts, the moment a live channel renders it. Render waits up to half a frame (`FrameRing::waitFor`, signalled by `push`) when the camera's newest frame is within three frames before the position; otherwise it does not wait. Lab, 60 windows of 80 frames of live output audio: with the buffer on tmpfs 1.2.0 is already 60/60 clean; on the SAS disk 1.2.0 is 57/60 and 1.2.1 56–58/60, because a `write()` stalled by dirty-page throttling holds up the recorder for 20–30 ms. Not fixed for slow disks (that needs segment writes off the recorder thread); the README requires a dedicated NVMe. Also tried and not released: rendering grains in the middle of their period with the recorder waking at grain boundaries (58/60 and 60/60 on the SAS disk).
27. **CPU path without the engine lock** (1.2.2). Without nvJPEG a camera with one phase encodes on its reader thread with `encodeJpegV210`: v210 lines to 8-bit rows, libjpeg-turbo raw 4:2:2, output written straight into the stored vector, one compressor and its row buffers per thread; the same bytes as `encodeJpeg422(unpackV210(…))` (unit test). HFR phases still go through `ingestVideo` under the lock (their interleave needs it). Playout decodes exact frames with `decodeJpegToV210`; blends decode two 16-bit frames and blend in place without the lock. libjpeg-turbo 2.1 checks its SIMD support through a thread-local variable on every call (`__tls_get_addr`, about 8 % of the CPU path); a static libjpeg-turbo build would remove that and is not done.
28. **CUDA waits block** (1.2.3). `prepareNv` sets `cudaDeviceScheduleBlockingSync` once, before the primary context exists. With CUDA's default (spin while there are spare cores) each thread's `cudaStreamSynchronize` busy-waited: 4 interpolating channels used 7.4 cores, 6.5 of them in libcuda. A blocking wait costs a wake-up of some microseconds per sync, nothing next to a 20 ms grain.
29. **Senders resolve `auto` in `/active`** (1.2.4). Each sender connection gets `[{mxl_domain_id: <output domain>, mxl_flow_id: <its flow>}]` as active parameters when it is created, and `on_resolve_auto` resolves a sender's `auto` to the same values (the flow from the IS-04 sender's `flow_id`). Receivers keep their previous `auto` handling.
23. **Buffers of removed cameras are deleted at startup** (1.2.0), before the space check, unless a clip uses that camera. Only `seg-*.bin` and 1.0.x segment files are removed, then the directory if it is empty.

## Operating points

Taken from Futatabi `flow.h`:

| Preset | Futatabi point | Finest level | Variational |
| --- | --- | --- | --- |
| `fast` | 1 | 3 (1/8) | no |
| `balanced` | 3 | 1 (half) | yes |
| `quality` | 4 | 0 (full) | yes |

Weights match Futatabi: alpha 1, delta 0.25, gamma 0.25, five SOR iterations, omega 1.8, outer iterations = pyramid level + 1.

## Process

One process. Each playout channel has a thread that renders one grain per house period on the TAI grid and publishes it when libmxl is linked. HTTP and nmos-cpp use their own threads. The recorder runs on the ingest path (MXL reader thread, or the synthetic generator).

Exit codes: 0, 75 (storage or unexpected failure), 78 (config, including a buffer that does not fit), 143 (SIGTERM).

## Tests

`./build/unit-tests` covers the scheduler, HFR interleave, protected ring, shotbox and playlist advance, audio cadence and stretch length, JPEG and v210 round trips, ANC timecode, config precedence, id derivation, the catalog, the 70 GB/h estimate, DIS on a translation, and an engine pass that records, switches angle, creates a clip, uploads a JPEG, and exports it. Since 1.2.0 also: the bytes of the segments a clip keeps, counted write failures, reads while the writer deletes segments, `/livez` and `/readyz` around the re-index with an unused camera buffer removed, recorded camera audio in live playout, and an upload after recording that plays the uploaded picture, and clip ids that continue after a restart.

`tests/integration/replay.sh` starts the binary with a synthetic camera against a stub Registration and Query API, checks `/livez`, `/readyz` (the node is listed), `/metrics`, a transport command, and the UI, then sends SIGTERM and checks exit 143, a registration DELETE of the node, and removal of the output domain when libmxl created it.

## Platform guideline G1–G14

| Item | Status | Evidence |
| --- | --- | --- |
| G1 Configuration | met | Env, then `REPLAY_CONFIG_FILE`, then `REPLAY_STATE_DIR/config.json`, then defaults (`src/config/config.cpp:510`, `src/main.cpp:83`). Unknown env ignored, invalid values exit 78. Settings table in `README.md`. State directory default `/config`. No secret settings. |
| G2 MXL domains | met | Scan path `/Volumes/mxl` (`src/config/config.cpp`). Output domain created once; a different `domain_def.json` id is logged and not overwritten (`src/domain/scan.cpp`). Mirrors are read, never used as the output domain (`src/mxl/io.cpp:63`). `MXL_HISTORY_DURATION` is written only when the domain is created. |
| G3 NMOS identity | met | UUIDv5 from `NMOS_SEED` for node, device, sources, flows, senders, receivers, and the default domain (`src/nmos/ids.cpp`). `NMOS_LABEL` and `NMOS_TAGS` (`src/nmos/node.cpp:276`). Group hints stay. |
| G4 Registry, no DNS-SD | met | `NMOS_QUERY_*` default to the registry and port + 1. `NMOS_DNS_SD=false` sets `pri`, `highest_pri`, and `authorization_highest_pri` to the maximum integer (`src/nmos/node.cpp:202`). The process starts without an Avahi daemon. |
| G5 Announce IP addresses | met | `NMOS_HOST_ADDRESS`, with `HOST_ID` kept as an alias when it is an IPv4 literal (`src/config/config.cpp:617`). `href_mode` is addresses only (`src/nmos/node.cpp:197`). The UI WebSocket uses the browser URL, which is the ingress name. |
| G6 Ports | met | `WEB_PORT`, `NMOS_PORT`, events WebSocket at `NMOS_PORT+1`. Bind failure exits 75 (`src/ops/httpserver.cpp`). |
| G7 Health and metrics | met | `/livez` always, `/readyz` checks the Query API when a registry is set (`src/ops/api.cpp:121`). Metrics prefix `mxl_replay_`. |
| G8 Clean shutdown | met | SIGTERM releases MXL, nulls NMOS resources so DELETEs are sent (`src/nmos/node.cpp:435`), removes only the matching output domain when `MXL_CLEANUP_ON_EXIT=true` (`src/main.cpp:141`), and exits 143. `SHUTDOWN_TIMEOUT_S` defaults to 10. No child processes are spawned. |
| G9 IS-05 | met | Senders carry `mxl_domain_id` and `mxl_flow_id`. Receivers take the connection PATCH. `master_enable: false` stops the reader. Active routes persist in the state directory (`src/app/engine.cpp:1215`). |
| G10 Config export and import | met | `GET /api/v1/config/export` and `POST /api/v1/config/import` (`src/ops/api.cpp:154`). No secrets to omit. |
| G11 Image and CI | met | `git-<sha7>` and `nightly-dev` on `main`; `X.Y.Z`, `X.Y`, `X` on a `vX.Y.Z` tag (`.github/workflows/container.yaml`). Runtime uid 1000. OCI labels include source, revision, licences, and `io.dmf.mxl.revision`. The example manifest uses `1.0.0`. |
| G12 Kubernetes example | met | `deploy/k8s/deployment.yaml`: pod network, standard env, probes, grace period 30, MXL hostPath, `/config`, uid 1000, `supplementalGroups`, no `hostIPC`. `runtimeClassName: nvidia` is the GPU path. |
| G13 Documentation | met | `README.md`, `CHANGELOG.md`, `SPECIFICATION.md`. |
| G14 Tests | met | Unit tests cover parsing, domain identity, export/import, and port bind. `tests/integration/replay.sh` covers ready, SIGTERM, deregistration, and domain removal when libmxl is linked. |
