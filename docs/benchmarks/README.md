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
