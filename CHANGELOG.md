# Changelog

## 1.2.0

- Camera audio is recorded and played out. 1.1.0 accepted connections on the `<camera> Audio` receivers but never read them, so playout audio was silent. The recorder now reads, for every video grain, that frame's 48 kHz samples from the camera's audio flow and stores them with the frame (first two channels, mono doubled; the receiver reports `unsupported` for another sample rate). HFR cameras take their audio with phase 1. The playout audio flow is written at the sample index, one ring per channel. Before, it was written at the video grain index into the first channel's ring only.
- Uploads are stored again when the cameras record. Since 1.1.0 an upload went to camera 1's buffer at TAI 1 ns onward, and that buffer only appends, so once camera 1 had recorded anything the frames were dropped while the clip was still created. Uploads now go to their own library buffer (`REPLAY_STORAGE_DIR/library/frames`, camera `0` in the clip list), which keeps a segment only while a clip uses it. Clips uploaded with 1.1.0 keep camera 1.
- The start no longer waits for the re-index before HTTP. `/livez` answers at once; `/readyz` and the API answer 503 (`{"status":"indexing"}`) until the retained segments are indexed, and NMOS and MXL start after that. 4 cameras × 1.5 h are about a million small reads.
- `REPLAY_PROTECT_MAX_PCT` counts the segments that clips keep. A clip keeps whole segments, but the cap counted only the protected frames, so clips could keep much more than the cap.
- Frames that cannot be written (full disk or I/O error) are counted per camera in `mxl_replay_storage_write_failed_total`, and the log has one `segment_write_failed` per run of failures and a `segment_write_recovered` with the count. On a full disk every frame also left an empty segment file behind; it is removed now.
- Disk I/O runs outside the engine lock. Each camera buffer has its own lock that is never held during a read or write, and the engine lock is released while recording writes, while a channel reads its frames and audio, and while a clip is exported. A slow disk used to stall every channel. A channel playing at 100 % reads one frame per output frame instead of two, and the audio no longer reads the frame's JPEG.
- The recorder no longer stalls at every segment rotation. Starting the writeback of the finished segment (`sync_file_range`) blocked the recording thread for 0.7–1.0 s on the lab's SAS disk, every 10 s and on all cameras at once, close to the 1 s input history. Writeback, the page-cache drop and deletions now run on a background thread per buffer.
- Playout renders each output grain for its own time on the TAI grid. It rendered for the wake-up time, so a live channel sat between two recorded frames: it interpolated them (or blended, or repeated one of them), the audio repeated or skipped a frame when the wake-up time drifted across the half frame, and a grain index could be written twice. Live output is now the recorded frame two grains back, exactly.
- A camera's audio that is committed a little after its video is waited for (up to one frame) instead of being stored as silence, as long as the audio keeps arriving.
- Clip, upload and playlist ids continue after the catalog's highest id. They started at 1 on every start, so the first clips made after a restart replaced the clips that already had those ids (`clip-1`, `upload-1` …), while their ranges stayed protected.
- The buffers of cameras that are no longer configured are deleted at startup unless a clip uses that camera (`removed_camera_deleted`, or `removed_camera_kept` with the bytes).
- `/metrics` has the per-camera series the README listed: `record_frames_total`, `record_dropped_total`, `phase_missing_total`, `protected_bytes`, `disk_bytes`, and the new `storage_write_failed_total`.

## 1.1.0

- The replay buffer lives on `REPLAY_STORAGE_DIR` instead of in RAM. 1.0.x kept every JPEG frame of the buffer in memory (4 cameras × 1.5 h is about 420 GB) and wrote segment files that were never read. Frames now go to the segments only; memory holds an index of about 32 bytes per frame and a few open files, and playback reads the frames back from the segments.
- Disk use stays at the buffer duration: segments that are older than the camera's `BUFFER_HOURS` behind its newest frame are deleted, unless a clip touches them.
- A restart keeps the buffer and the clips: the existing segments are indexed again and the clips in the catalog protect their frames again. 1.0.x renumbered its segments from 0 on every start and overwrote them; those files are removed at the first start.
- The startup space check counts the buffer's own segments as available, so a second start with the storage kept no longer exits 78.
- NMOS senders and receivers carry their own labels (`<channel label> Video`, `Audio`, `Data`; `<camera label> Video`, `Audio`) instead of the node label, so a controller can address them by label. Default channel labels are unique: `PGM`, `PVW`, `CH3`, `CH4` … (every channel after the first was `PVW`).
- Camera status reports `disk_bytes` and `segments`.
- `REPLAY_ODIRECT` is ignored with a warning.

## 1.0.1

- GPU work no longer runs behind one lock. Every recorder input and every playout channel has its own thread, CUDA stream and nvJPEG state; the engine lock is released while a channel decodes, interpolates and downloads. Before, one thread encoded every camera and one loop rendered every channel in turn, both behind a single mutex. On an NVIDIA A16 (one GA107) four cameras now record at full rate while four 1080p50 channels interpolate at 0.5×; before, two channels left each camera at about 19 of 50 grains per second ([docs/benchmarks/README.md](docs/benchmarks/README.md)).
- Fewer GPU stalls and copies: device memory comes from the stream-ordered pool instead of per-frame `cudaMalloc`/`cudaFree` (which waits for the whole device), the v210 kernels run one thread per 6-pixel group instead of one per row, the MXL grain is page-locked for its upload, and a source frame still decoded from the previous output frame is not decoded again.
- The process raises its open-file limit to the hard limit. MXL keeps one descriptor per grain, and four cameras with four channels passed Docker's default of 1024 ("Too many open files"; cameras 3 and 4 never recorded).
- The MXL recorder keeps one reader per camera phase while its route stays the same and reads every grain since the last one. Before, it scanned the domains and created a new reader for each grain, recorded about 9 of 50 grains per camera at 1080p50, and counted none as dropped. Grains that leave the input ring before they are read now count in the camera's `dropped`. Each recorded frame carries the timestamp of the grain that was read (it was two grains later).

## 1.0.0

First stable contract for the platform. Settings, the HTTP API, and the shutdown behaviour below stay compatible until 2.0.0.

### Platform settings

New settings. Existing names still work.

- `REPLAY_STATE_DIR` (default `/config`) holds the catalog, imported settings, and IS-05 routes. A catalog found in `REPLAY_STORAGE_DIR` is copied here once.
- `NMOS_LABEL` sets the node label and the device label. When it is unset, the node label stays `HOST_ID` and the device label stays `MXL Replay`.
- `NMOS_TAGS` is a JSON object of string arrays added to the node and the device.
- `NMOS_HOST_ADDRESS` is the IPv4 address announced to other systems. `HOST_ID` remains the seed identity. If `HOST_ID` is an IPv4 literal and `NMOS_HOST_ADDRESS` is unset, that literal is still the announced address.
- `NMOS_QUERY_ADDRESS` defaults to `NMOS_REGISTRY_ADDRESS`. `NMOS_QUERY_PORT` defaults to `NMOS_REGISTRY_PORT + 1`.
- `MXL_HISTORY_DURATION` (nanoseconds, default `2000000000`) is written when the output domain is created.
- `MXL_CLEANUP_ON_EXIT` (default `false`) removes only this function's output domain on shutdown.
- `SHUTDOWN_TIMEOUT_S` (default `10`).

`NMOS_DNS_SD=false` still disables DNS-SD browsing and mDNS advertisement.

The default `MXL_OUTPUT_DOMAIN_ID`, when unset, is now the same UUIDv5 as the NMOS domain id (`mxl-replay/<NMOS_SEED>/domain`).

### API

- `GET /api/v1/config/export` returns one JSON document. There are no secret settings, so `include_secrets=true` adds nothing.
- `POST /api/v1/config/import` restores settings, clips, and playlists from that document.
- `/readyz` is 200 only after the node is listed on the Query API when a registry address is configured.
- A `WEB_PORT` or `NMOS_PORT` that cannot be bound exits 75.
- SIGTERM exits 143 after MXL readers and writers are released and the node is removed from the registry.

### Image

`ghcr.io/leeo86/mxl-replay:1.0.0` (also `1.0` and `1`). The image runs as uid 1000. It contains this GPL-3.0-or-later program plus NVIDIA `libcudart` and `libnvjpeg`. See `README.md` for the VRAM estimate.
