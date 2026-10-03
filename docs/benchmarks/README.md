# Benchmarks

Hardware numbers for the A4000 and L4 are filled in by a run on that GPU. This tree does not invent them.

## What to measure

For `dis-cuda` `balanced` and for `ofa` (when `VK_NV_optical_flow` is present):

- Concurrent 1080p50 `interpolate` channels before an output grain is late.
- Whether 2160p50 stays inside the same budget.
- Per output frame, GPU time of decode + flow + interpolate + pack. The target is under half of the frame period at the channel count above, and zero late grains over one hour.
- SM, memory, and OFA utilisation per channel.

Publish the table in this directory next to the harness report.

## Harness

`mxl-replay-harness a.ppm b.ppm 0.5 out.ppm` runs the port.

`mxl-replay-harness --compare reference.ppm port.ppm` prints PSNR and SSIM and exits 0 when PSNR is at least 40 dB.

The original Futatabi OpenGL build is not part of the default image. Build it from the Nageru 2.3.4 tarball in a separate job with EGL, run both on the same pairs, and store the report here. The synthetic translation test in the unit suite is the gate that runs without a GPU; it is not a substitute for the Futatabi comparison.

## Storage

At start the process writes 1 MiB sequentially and exposes `mxl_replay_write_bytes_per_second`. The full-load check is all configured cameras recording while two channels play. Warn when the result is below `REPLAY_STORAGE_MIN_MBPS` (default 100).

Sizing used by the free-space check: `width * height * (quality / 92) * 0.1875` bytes per frame. At 1920×1080, quality 92, 50 fps that is about 70 GB/h per camera. HFR multiplies by the frame-rate factor. Audio adds 48 kHz stereo float32.

## Lab run 2026-10-03: NVIDIA A16

Not a target GPU: one GA107 of an A16 (PCIe Gen4 x4), driver 595.84, 2× Xeon Gold 6136, image built from this repository (1.0.0 plus the recorder fix in the CHANGELOG). Cameras were mxl-test-player 1080p50 outputs (bars with burn-in), routed by IS-05; storage was the operating-system disk (`mxl_replay_write_bytes_per_second` 27.9 MB/s, below `REPLAY_STORAGE_MIN_MBPS`), buffer 0.05 h.

| Case | Result |
| --- | --- |
| 1.0.0, 4 cameras recording | about 9.3 recorded grains/s per camera, `dropped` 0 (grains skipped without being counted) |
| Fixed, 1 camera recording | 49.9 grains/s, no drops |
| Fixed, 4 cameras recording | about 20 grains/s per camera, about 30 drops/s per camera. The one recorder thread encodes about 80 frames/s in total; `gpuEncodeV210` holds one global nvJPEG state behind a mutex |
| 2 channels, `dis-cuda` `balanced`, `interpolate` at 0.5× (1 camera recording) | output head 3 grains behind the current index (`mxl-info` latency 35–55 ms), SM 69 % |
| 4 channels, same | output heads 3–8 grains behind (up to 160 ms), SM 69–76 %, process 1.1 cores |

Two 1080p50 interpolate channels keep up on this GPU; four do not. The GPU is not saturated in either case. Late output grains are not counted anywhere (no metric), so the lag was read from the output flows with `mxl-info`. GPU time per stage (`frame_gpu_seconds`) is described in the README but not exported. 2160p50, OFA and the Futatabi comparison were not run.
