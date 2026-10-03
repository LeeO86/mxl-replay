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
8. **Query API port is `NMOS_REGISTRY_PORT + 1`.** Same as the other media functions.
9. **Unknown environment variables are ignored.** The config file still rejects unknown keys, so a stray `NMOS_CPP_REF` in the CI environment does not exit 78.
10. **Segments are one MXLR file per 10 s**, not one file per JPEG. The write is sequential. `REPLAY_ODIRECT=true` asks for `O_DIRECT` and falls back to buffered IO when the filesystem refuses it (tmpfs does).
11. **Export is a JPEG sequence plus WAV**, not ProRes 422 HQ. Upload conversion does use FFmpeg and stores the result as the same JPEG + PCM clips the buffer uses.
12. **Phase order is receiver order.** Phase k is the k-th video receiver of the camera. A custom permutation list is not a separate key.
13. **One image, GPU when the toolkit injects a device.** `docker/Dockerfile` builds on `nvidia/cuda:12.8.2-devel` and the runtime stage carries `libcudart` and `libnvjpeg` only. `libcuda` comes from the NVIDIA container toolkit (`--gpus all`, or `runtimeClassName: nvidia`). Without a device the same binary records and plays with libjpeg, repeat, and blend. CI compiles that same CUDA binary (`ldd` must show `libnvjpeg`); it does not publish a second CPU image. On the device path the host sees the JPEG bitstream and the finished v210 grain. Search, refinement, blend, and packing stay in device memory, and the flow pair is cached there. `.github/workflows/container.yaml` publishes `ghcr.io/leeo86/mxl-replay:nightly-dev` from `main` (the rolling dev image the siblings use), `git-<sha>` on every push, and `X.Y.Z` / `latest` only from a `vX.Y.Z` tag.

## Operating points

Taken from Futatabi `flow.h`:

| Preset | Futatabi point | Finest level | Variational |
| --- | --- | --- | --- |
| `fast` | 1 | 3 (1/8) | no |
| `balanced` | 3 | 1 (half) | yes |
| `quality` | 4 | 0 (full) | yes |

Weights match Futatabi: alpha 1, delta 0.25, gamma 0.25, five SOR iterations, omega 1.8, outer iterations = pyramid level + 1.

## Process

One process. The playout loop wakes on the house period, renders every channel, and publishes MXL grains when libmxl is linked. HTTP and nmos-cpp use their own threads. The recorder runs on the ingest path (MXL reader thread, or the synthetic generator).

Exit codes: 0, 75 (storage or unexpected failure), 78 (config, including a buffer that does not fit), 143 (SIGTERM).

## Tests

`./build/unit-tests` covers the scheduler, HFR interleave, protected ring, shotbox and playlist advance, audio cadence and stretch length, JPEG and v210 round trips, ANC timecode, config precedence, id derivation, the catalog, the 70 GB/h estimate, DIS on a translation, and an engine pass that records, switches angle, creates a clip, uploads a JPEG, and exports it.

`tests/integration/replay.sh` starts the binary with a synthetic camera and checks `/livez`, `/readyz`, `/metrics`, a transport command, and the UI.
