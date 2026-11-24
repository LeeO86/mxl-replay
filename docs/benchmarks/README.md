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

## Lab run 2026-10-06: blocking CUDA sync (1.2.3)

Same host and procedure as below (four test-player outputs recorded as cameras 1–4, every channel `interpolate` at 0.5× or played at 1× from 10 s back, 30 s measured). `perf` on 1.2.2 with 4 channels: 84 % of the process in libcuda, the CUDA runtime spinning in `cudaStreamSynchronize` (CUDA's default scheduling spins while the machine has spare cores). 1.2.3 sets `cudaDeviceScheduleBlockingSync`.

| Image | Channels | Speed | Recorded / dropped per camera per s | Output grains/s per channel | Output latency (grains) | CPU | GPU SM |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1.2.2 | 2 | 0.5× | 56.7–56.8 / 0 | 50.0, 50.0 | 4, 4 | 3.79 cores | 92 % |
| 1.2.3 | 2 | 0.5× | 56.8 / 0 | 50.0, 49.9 | 3, 2 | 0.82 cores | 91 % |
| 1.2.2 | 4 | 0.5× | 56.2–56.5 / 0 | 50.0–50.1 | 1–7 | 7.37 cores | 99 % |
| 1.2.3 | 4 | 0.5× | 56.7 / 0 | 49.9–50.2 | 2–6 | 0.89 cores | 100 % |
| 1.2.2 | 4 | 1× | 56.7–56.8 / 0 | 50.0 | 1 | 3.29 cores | 57 % |
| 1.2.3 | 4 | 1× | 56.4 / 0 | 50.0 | 1–2 | 1.75 cores | 58 % |
| 1.2.2 | 8 | 0.5× | 28–30 / 27–29 | 49.9–50.2 | 3–13 | 12.11 cores | 97 % |
| 1.2.3 | 8 | 0.5× | 30–31 / 26–27 | 49.9–50.3 | 5–10 | 0.93 cores | 98 % |

Recorded counts above 50 per second are cameras catching up on grains still in the input ring. The CPU growth of the 2026-10-04 run below ("every camera is now actually recorded … nvJPEG's Huffman stage on the CPU") was mostly this spinning: with a blocking sync, four cameras and four interpolating channels take under a core. The GPU stays the limit for interpolation: at 8 channels it is saturated and the cameras drop about half their grains with either version.

## Lab run 2026-10-04: per-thread GPU pipeline

Same host, GPU and cameras as above; storage the OS disk. Image built from this repository with the per-thread GPU pipeline (1.0.1) against `mxl-replay:lab` (1.0.0 plus the recorder fix). Each case: the four test-player outputs routed to cameras 1–4 (all recording), every channel set to `interpolate` and played at 0.5× from 10 s back, 30 s measured. Recorded and dropped grains come from `/api/v1/status`, output grains/s and late reads from `mxl-verify` on each channel's video flow, latency from `mxl-info`, CPU from the container cgroup, SM load from `nvidia-smi dmon`.

| Image | Channels | Recorded / dropped per camera per s | Output grains/s per channel | Output latency (grains) | CPU | GPU SM |
| --- | --- | --- | --- | --- | --- | --- |
| before | 2 | 18–20 / 36–39 | 50.0, 50.0 | 0–2 | 1.2 cores | 63 % |
| before | 4 | 34 / 20–22 for cameras 1–2; cameras 3–4 none ("Too many open files") | 49.7–50.0 | 2–5 | 1.2 cores | 60 % |
| this tree | 2 | 52–53 / 0 | 50.0, 50.0 | 2 | 3.7 cores | 92 % |
| this tree | 4 | 53 / 0 | 49.9–50.1 | 1–3 | 6.2 cores | 98 % |
| this tree | 8 | 33 / 22 | 49.8–50.1 | 1–12 | 12.3 cores | 97 % |

Recorded counts above 50 per second are cameras catching up on grains still in the input ring. The CPU rises because every camera is now actually recorded (200 encodes per second instead of about 75) and the nvJPEG decoder runs its Huffman stage on the CPU on this GPU.

Result: four 1080p50 cameras record without drops while four channels interpolate, and the limit is now the GPU itself: dense optical flow (`dis-cuda` `balanced`) for one new frame pair per output pair at 0.5× keeps the GA107 at 97–98 % with four channels. Eight channels on this GPU take recording below full rate. A larger GPU (A4000, L4) is expected to scale with its SM count; that run is still open, as are 2160p50, OFA and the Futatabi comparison.

## Lab run 2026-11-24: the CPU blend in v210

Where a CPU slow motion lost its time, and the blend that works in v210 instead. The host was a 16 vCPU AMD EPYC (Genoa) VM without a GPU, with Debian sid and GCC. The pictures are `crowd_run` from SVT's 1080p50 test set (Xiph's derf collection), converted to v210.

### Where the time went

`mxl-replay-blend-bench` ran on 40 pictures. It times one blended grain of a 0.5× slow motion (frames k and k+1 at 50 %), stage by stage, in ms on one thread, with JPEG q92.

| Stage | ms |
| --- | --- |
| **Before (1.8.7)**, all on the output thread | |
| decode A into a 16-bit frame | 9.54 |
| decode B into a 16-bit frame | 9.59 |
| blend the 16-bit frames (`blendInto`) | 1.68 |
| pack to v210 (`packV210`) | 2.34 |
| copy to the channel's last grain | 0.32 |
| **output thread** | **23.47** |
| **After** | |
| the worker decodes B to v210 | 9.70 |
| B out of the worker's slot (copy) | 0.31 |
| blend A and B into the grain (`blendV210`) | 0.49 |
| copy to the channel's last grain | 0.28 |
| **output thread** | **1.08** |
| **CPU in all** | **10.78** |

- The old path needed more than a 50p grain (20 ms) on the output thread for every blended grain. The two decodes were 82 % of that, and it decoded each frame twice, as B of one grain and again as the exact frame of the next.
- Unpacking into 16-bit frames and packing back cost another 4 ms, and the blend itself 1.7 ms. v210 needs neither step.
- Now the decode worker decodes each frame once, straight into v210. The output thread copies it out of the slot and blends. `blendV210` takes 0.5 ms with AVX2 and 2.0 ms in the baseline x86-64 build, which can't multiply 32-bit lanes, hence `target_clones`. A blended grain now costs less than half the CPU in all, and the output thread about 1 ms.
- What remains is libjpeg-turbo's decode, 9.7 ms. Decoding straight to v210 costs the same as decoding to 16 bits, and the faster line packing (`line8ToV210` without its per-pixel branch) made no difference.

### The replay

The whole process ran without MXL. Every camera recorded real 1080p50 pictures and channel n played camera n (channels 5–8 played cameras 1–4 again). Each case ran 25 s, before and after the change in one job.

| Case | Before: output grains/s, resyncs, render ms, CPU | After |
| --- | --- | --- |
| 4 cameras, 4 channels, 0.5× blend | 38.9, 371, 25.5 ms, 5.60 cores | 50.0, 0, 3.4 ms, 4.02 cores |
| 4 cameras, 8 channels, 0.5× blend | 50.0, 3, 17.6 ms, 9.37 | 50.0, 0, 4.4 ms, 6.60 |
| 12 cameras, 8 channels, 1× repeat | 49.9, 6, 5.4 ms, 10.56 | 49.8, 8, 5.9 ms, 10.35 |

- Slow motion on the CPU now keeps up with 8 channels on 4 cameras and takes about a quarter less CPU. Before, 4 channels fell behind, and the 8-channel run that kept up had 17.6 ms renders, close to the limit.
- Playout at 1× is unchanged. It takes the same path, and the few resyncs at 12 cameras and 8 channels occur before and after.
