# Changelog

## 1.3.1

- Grains a source never wrote no longer count as dropped. A camera that connects before its writer writes (the test player creates its flows first, then starts its outputs; a source that restarts) waits at its first grain. 1.3.0 then counted every grain up to the writer's first one, one by one as each never-written slot left the flow's one-second history, and started recording a second late from the oldest grain, where the first encodes cost a few real grains. That was the "dropped 50 at start" on the platform. Now an unwritten grain behind the writer's head is skipped at once, and a grain that left the history while the reader waited for it is not counted. A source that stops for more than a second no longer counts either. A recorder that falls behind its source still counts every grain it missed. Lab, 2 cameras on a test player, per camera: player started after the replay 3–15 → 0 drops (5 starts), player restarted 2–10 → 0, player frozen 3 s 106–107 → 0, replay frozen 2 s 57–65 → 55–64 (real, still counted). No settings change.

## 1.3.0

- New web UI in the look of the other LeeO86 media functions (header with recording, storage, GPU and connection pills, banners for errors, indexing and lost updates, tabs with `#hash` routing). Every API function has a control:
  - **Shotbox**: large buttons for clips and playlists with thumbnail, name, length, camera and speed; cued yellow, playing green with a progress bar, paused, ended grey; the clip colour is a stripe. Banks follow the window size. Big *Back to live*, speed presets, target channel picture. Keys: 1–0 select a button, Enter cues/plays it, Space play/pause, PgUp/PgDn bank.
  - **LSM**: channel monitor with timecode, camera, state and speed; camera row with live pictures and recording state (click or 1–9 switches the angle); IN, OUT, Go IN, Go OUT, ±1 frame, ±1 s, Play, Pause, Live; speed fader 0–200 % with presets, reverse and mouse wheel; motion, audio and timecode mode; lock to the first channel; buffer timeline with clips and marks (drag to scrub, wheel ±1 frame, Shift ±1 s, zoom 30 s – all); *Create clip* with name, all angles, colour and tags, and *Create anyway* when the protection cap is reached.
  - **Library**: search by name, tag, camera or timecode; clip inspector (IN/OUT frame by frame or from the channel's marks, speed, modes, end action, name, colour, tags); export (shows the path), consolidate, delete (deleting did nothing before); upload with name, progress and a converting state.
  - **Playlists**: create, rename, add clips, reorder, per-entry speed, end action and auto-advance, delete, play on the selected channel with its state.
  - **Cameras** (picture, routes, recording, buffer, drops, disk), **NMOS** (node, receivers with their routes, senders and flows), **Settings** (export as formatted JSON with Download and Copy, import from a file or text, current settings, keyboard map kept in the browser, WebHID ShuttleXpress / ShuttlePRO jog and shuttle).
  - The UI draws the status at most 10 times a second, reconnects, and shows *Indexing the buffer…* instead of an empty page while the API answers 503.
- The WebSocket `/api/v1/events` sends the status 10 times a second instead of every frame (50 times at 50p), and never waits for a client: one that cannot take a message at once (a sleeping laptop, a slow link) is disconnected and its UI reconnects. Before, one such client held up the status push and then the whole HTTP server. Status messages over 64 KB are framed correctly.
- New API: `PATCH /api/v1/clips/{id}` (name, `in_ns`, `out_ns`, speed, motion, audio, end, colour, tags). A new IN or OUT needs a recorded frame within one frame period and lands on it (400 otherwise, also when OUT then comes before IN); the protected range moves with it, and a longer range at the protection cap answers 409 unless `force` is set. Clip and playlist-entry speeds are 0.01–2 (at 0 or backwards a clip never reaches OUT). `PUT` keeps entries whose clip was deleted (they are skipped); a channel playing the playlist stays on its entry. `GET`/`PUT`/`DELETE /api/v1/playlists/{id}` (the entries with clip, speed, end action and auto-advance), `GET /api/v1/cameras/{n}/preview.jpg`, `GET /api/v1/clips/{id}/thumbnail.jpg`. `POST /api/v1/clips` takes `colour` and `tags`. `POST /api/v1/playlists` takes `clips` as a JSON array (or a comma list) or `entries`.
- Status: channel `target_speed` (the speed set; `speed` is where the ramp is), `timecode` (the last output grain's ANC timecode), `position_tc`, `in_tc`, `out_tc`, `tc_mode`, `has_in`, `has_out`, `shot_id`, `playlist`, `playlist_index`, `black`; camera `oldest_ns`, `newest_ns`, `buffer_hours`, `recording`, `video_states`, `audio_state`; `label`, `tai_ns`, `frame_ns`, `mxl`, `nmos_cpp`. Clips list `in_tc` and `out_tc`; playlists `duration_ns`. `/api/v1/nmos` lists the receivers with their routes and the senders with their flows.
- Previews are made when the UI asks for them: the channel picture is 640 pixels wide from the last output grain; camera pictures and clip thumbnails are decoded from the stored JPEG at a quarter size (libjpeg DCT scaling, 480 × 270 at 1080p). Channel pictures are kept for 150 ms, camera pictures for a second, thumbnails until the clip's IN changes, so several UIs share them. Lab (1080p, four cameras recording, buffer on a SAS disk), median request time: camera picture 12.5 → 5.1 ms when made, 0.4 ms when kept; a clip thumbnail's first request 12.9 → 10.3 ms (mostly reading the frame from disk), then kept. A thumbnail needs the IN frame (404 when it is gone). Before, every rendered frame made a 32×16 thumbnail (the GPU path copied the top-left 32×16 pixels, so the monitor showed a flat colour), and cameras made none.
- Playlists play as configured: each entry's speed, end action and auto-advance apply (they were stored and ignored; the clip's own speed and end action were used). An entry that ends with *next* plays the next clip with auto-advance, otherwise it cues it. Deleted clips in a playlist are skipped.
- Shotbox: a playlist can be a shotbox button; a click on a paused shot resumes it instead of restarting at IN; a cue shows the clip's own camera at IN (it kept the channel's camera).
- A new clip keeps the speed that was set on the channel. It took the ramp's current value, which does not move while paused: a speed set before *Create clip* on a paused channel was lost.
- *Live* after an upload played follows camera 1. The channel stayed on the library, which has no live picture, and showed the upload's last frame.
- Request bodies are read as JSON. A missing, non-numeric or out-of-range value (speed outside −1…2, an unknown camera, mode, mark or command) answers 400 instead of ending the process (`std::stod` on a missing field threw) or being ignored; an unknown channel, clip or playlist answers 404; a handler that throws answers 500. `POST /api/v1/clips` answers 400 without IN/OUT and 409 only at the protection cap. Names and tags with quotes or control characters are escaped in every JSON answer.
- `POST /api/v1/uploads` takes the clip name from `?name=` (it was searched for inside the file's bytes).
- Two clips with the same range on the same camera: deleting one keeps the other's frames protected (it removed both ranges). A clip edit protects its new range before it releases the old one. A consolidated clip keeps no range: consolidating twice, then deleting or editing it, no longer releases another clip's range, and a restart no longer protects it again. `POST /api/v1/config/import` protects imported clips at once (they were protected from the next start).
- Config import reads the document with the same JSON reader: names and tags with quotes, tabs or `\uXXXX` come back as exported.
- A clip is looked up by its id in the catalog instead of reading every clip (shotbox clicks, edits, thumbnails, and the end of each playlist entry on the playout thread).

## 1.2.5

- A new output `domain_def.json` carries `description` and `tags`, as BCP-007-03 requires (`id`, `label`, `description`, `tags`). Replay wrote only `id` and `label`, and mxl-st2110-gateway 1.0.2 skipped such domains. An existing file is still not rewritten.

## 1.2.4

- Senders report their real `mxl_domain_id` and `mxl_flow_id` in IS-05 `/active`. Until now `/active` said `auto` from the start (nmos-cpp's `make_connection_mxl_sender` leaves it, and its own comment says the caller must resolve it), and a PATCH with `auto` resolved to an all-zero domain and a `null` flow. A controller that copies a sender's active parameters into a receiver's PATCH (the platform's crosspoint does) sent `auto`, which receivers reject: on the platform's test-all production the PGM → multiviewer route failed. Now the output domain and the sender's own flow, from the start and after a PATCH with `auto` (BCP-007-03). Receivers are unchanged. No settings change.

## 1.2.3

- The GPU path waits for the GPU in a blocking sync instead of spinning a CPU core. CUDA's default scheduling spins while the machine has spare cores, so every `cudaStreamSynchronize` of every channel and camera thread held a core busy. Lab host (2× Xeon Gold 6136, A16), 4 cameras recording, channels interpolating at 0.5×: 2 channels 3.79 → 0.82 cores, 4 channels 7.37 → 0.89 (6.5 cores had been inside libcuda), 8 channels 12.11 → 0.93; 4 channels at 1× 3.29 → 1.75. Output 50 grains/s per channel, no recording drops and the same output latency as before. No settings change.

## 1.2.2

- Without a GPU the replay records every frame. The CPU JPEG encode ran under the engine lock, so the cameras waited for each other: on the lab (2× Xeon Gold 6136, 4 cameras 1080p50, no GPU) each camera recorded about 11.5 and dropped about 45 grains per second. A camera with one phase now encodes on its own reader thread, straight from the v210 grain into the stored JPEG (no 16-bit frame, codec and buffers kept per thread). 4 cameras + 2 channels at 1×: every grain recorded, 3.8 cores; 4 cameras + 4 channels blending at 0.5×: every grain recorded, 50 grains/s per output, 6.0 cores.
- CPU playout: an exact frame (live, 1×, repeat) is decoded straight into the output grain; only a blend decodes two 16-bit frames (before, every frame decoded frame B as well). The blend runs without the engine lock, in integer steps of 1/1024, in place.
- Idle and black channels use one cached black grain, and thumbnails of the CPU path are refreshed every 10th frame: two idle channels went from 0.90 to 0.30 cores (with a GPU).
- Documentation: a dedicated local NVMe for `REPLAY_STORAGE_DIR` is a requirement (README, specification §4.3 and §10). The OS disk, rotating disks and network storage are not supported.
- The unit test "disk ring reads while it writes" no longer fails when the writer finishes before the reader thread has run.

## 1.2.1

- A live channel waits up to half a frame for its frame when the recorder stores it late (while the camera records close to live). Without the frame, the channel showed M−3 and then jumped to M−1. Measured on the lab after the release: the late frames come from the storage. With the buffer in RAM, 1.2.0 showed none in 60 windows of 80 frames. On the lab's SAS disk, where one `write()` can block the recorder for 20–30 ms, 1.2.0 and 1.2.1 both showed a few (57/60 and 56–58/60 clean windows), so the wait does not cover a slow disk. Keep the buffer on a dedicated NVMe.
- After a missed camera audio read, the recorder keeps waiting for the audio for three more frames before it falls back to one waiting read per 50 frames. One late packet no longer costs up to a second of audio.

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
