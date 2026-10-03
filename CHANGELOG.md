# Changelog

## Unreleased

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
