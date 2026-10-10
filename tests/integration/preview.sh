#!/usr/bin/env bash
# Preview integration (SPECIFICATION.md §8.6, §8.7) with synthetic cameras, no NMOS, no GPU needed:
#  1. REPLAY_PREVIEW_MODE unset: JPEG pictures, no MediaMTX started, nothing published.
#  2. webrtc, own mode: the built-in MediaMTX (`mediamtx` from PATH) gets the H.264 mosaic (RTSP DESCRIBE and
#     HLS), the JPEG pictures are refused, the tile map, /widgets with CORS and the widget pages' CSP;
#     SIGTERM stops MediaMTX.
#  3. webrtc, shared mode: a separate MediaMTX gets <prefix>/mosaic and none is started here; that MediaMTX
#     goes away (error) and comes back (publishing).
set -euo pipefail

BIN=${1:-./build/mxl-replay}
MEDIAMTX=${MEDIAMTX_BIN:-/tmp/mediamtx/mediamtx}
if [[ ! -x "$BIN" || ! -x "$MEDIAMTX" ]]; then
  echo "missing binary: replay=$BIN mediamtx=$MEDIAMTX" >&2
  exit 1
fi

WORK=$(mktemp -d)
PID=""
SHARED_PID=""
cleanup() {
  local status=$?
  if [[ -n "$PID" ]]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
  if [[ -n "$SHARED_PID" ]]; then kill "$SHARED_PID" 2>/dev/null || true; wait "$SHARED_PID" 2>/dev/null || true; fi
  if [[ "$status" -ne 0 ]]; then
    for log in "$WORK"/*.log; do echo "== $log" >&2; tail -40 "$log" >&2 || true; done
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

WEB_PORT=${REPLAY_TEST_PORT:-18170}
RTSP_PORT=18854
WHEP_PORT=18689
HLS_PORT=18688
ICE_PORT=18489
SHARED_RTSP_PORT=28854
SHARED_HLS_PORT=28688
HOST_ADDRESS=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1 || true)
HOST_ADDRESS=${HOST_ADDRESS:-10.255.0.10}
# Own mode starts `mediamtx` from PATH (the image has it in /usr/local/bin).
export PATH="$(dirname "$MEDIAMTX"):$PATH"

start_replay() { # <name> [KEY=value ...]
  local name=$1
  shift
  mkdir -p "$WORK/$name"
  env REPLAY_FORMAT=64x32p50 REPLAY_INPUTS=2 REPLAY_CHANNELS=2 REPLAY_BUFFER_HOURS=0.01 REPLAY_STORAGE_MIN_MBPS=0 REPLAY_SYNTHETIC=true \
    REPLAY_STORAGE_DIR="$WORK/$name/media" REPLAY_STATE_DIR="$WORK/$name/state" MXL_DOMAIN_SCAN_PATH="$WORK/$name/mxl" \
    MXL_OUTPUT_DOMAIN_DIR="$WORK/$name/mxl/replay" NMOS_ENABLE=false NMOS_HOST_ADDRESS="$HOST_ADDRESS" HOST_ID=ci-preview WEB_PORT="$WEB_PORT" \
    MEDIAMTX_RTSP_PORT="$RTSP_PORT" MEDIAMTX_WHEP_PORT="$WHEP_PORT" MEDIAMTX_HLS_PORT="$HLS_PORT" MEDIAMTX_ICE_UDP_PORT="$ICE_PORT" \
    WIDGET_FRAME_ANCESTORS="'self' http://designer.test" "$@" \
    "$BIN" >"$WORK/$name.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 100); do
    if curl -sf "http://127.0.0.1:${WEB_PORT}/readyz" >/dev/null; then return 0; fi
    if ! kill -0 "$PID" 2>/dev/null; then echo "$name: the replay exited early" >&2; exit 1; fi
    sleep 0.2
  done
  echo "$name: /readyz did not answer" >&2
  exit 1
}

stop_replay() {
  kill -TERM "$PID"
  local code=0
  wait "$PID" || code=$?
  PID=""
  if [[ "$code" != 143 ]]; then echo "exit code $code, expected 143" >&2; exit 1; fi
}

statusz() { # <python expression on the preview object `p`>
  curl -sf "http://127.0.0.1:${WEB_PORT}/statusz" | python3 -c "import json,sys; p=json.load(sys.stdin)['preview']; print($1)"
}

wait_state() { # <state> <seconds>
  for _ in $(seq 1 $(($2 * 4))); do
    if [[ "$(statusz "p.get('state')")" == "$1" ]]; then return 0; fi
    sleep 0.25
  done
  echo "preview state is not $1: $(statusz p)" >&2
  exit 1
}

# RTSP DESCRIBE: 200 with an H.264 track once the path has a publisher.
describe() { # <port> <path>
  python3 - "$1" "$2" <<'PY'
import socket, sys
port, path = int(sys.argv[1]), sys.argv[2]
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=2)
    s.sendall(f"DESCRIBE rtsp://127.0.0.1:{port}/{path} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n".encode())
    reply = s.recv(4096).decode(errors="replace")
except OSError:
    sys.exit(1)
sys.exit(0 if reply.startswith("RTSP/1.0 200") and "H264" in reply else 1)
PY
}

wait_describe() { # <port> <path> <seconds>
  for _ in $(seq 1 $(($3 * 2))); do
    if describe "$1" "$2"; then return 0; fi
    sleep 0.5
  done
  echo "rtsp://127.0.0.1:$1/$2 has no H.264 publisher" >&2
  exit 1
}

closed() { # <port>: nothing listens there
  ! python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1)" "$1" 2>/dev/null
}

code() { # <url>
  curl -s -o /dev/null -w '%{http_code}' "$1"
}

# ---- 1. JPEG (default): pictures on request, nothing published ------------------------------------
start_replay jpeg
for _ in $(seq 1 40); do
  if [[ "$(code "http://127.0.0.1:${WEB_PORT}/api/v1/cameras/2/preview.jpg")" == 200 ]]; then break; fi
  sleep 0.25
done
[[ "$(code "http://127.0.0.1:${WEB_PORT}/api/v1/cameras/2/preview.jpg")" == 200 ]] || { echo "jpeg: no camera picture" >&2; exit 1; }
[[ "$(code "http://127.0.0.1:${WEB_PORT}/api/v1/channels/1/preview.jpg")" == 200 ]] || { echo "jpeg: no channel picture" >&2; exit 1; }
[[ "$(statusz "p")" == "{'mode': 'jpeg'}" ]] || { echo "jpeg: statusz $(statusz p)" >&2; exit 1; }
closed "$RTSP_PORT" || { echo "jpeg: a MediaMTX runs" >&2; exit 1; }
METRICS=$(curl -sf "http://127.0.0.1:${WEB_PORT}/metrics")
for line in 'mxl_replay_preview_mode{mode="jpeg"} 1' 'mxl_replay_output_resyncs_total{channel="2"}'; do
  grep -qF "$line" <<<"$METRICS" || { echo "jpeg: metric $line" >&2; exit 1; }
done
stop_replay
echo "jpeg mode: ok"

# ---- 2. WebRTC, own MediaMTX ----------------------------------------------------------------------
start_replay own REPLAY_PREVIEW_MODE=webrtc
wait_state publishing 30
wait_describe "$RTSP_PORT" mxl-replay/mosaic 10
[[ "$(statusz "(p['publish'], p['mediamtx']['running'], p['encoder'] in ('nvenc', 'x264'), p['streams'][0]['state'])")" == "('own', True, True, 'publishing')" ]] || {
  echo "own: statusz $(statusz p)" >&2
  exit 1
}
[[ "$(code "http://127.0.0.1:${WEB_PORT}/api/v1/channels/1/preview.jpg")" == 404 ]] || { echo "own: a channel JPEG is served" >&2; exit 1; }
[[ "$(code "http://127.0.0.1:${WEB_PORT}/api/v1/cameras/1/preview.jpg")" == 404 ]] || { echo "own: a camera JPEG is served" >&2; exit 1; }
MAP=$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/preview/map")
python3 - "$MAP" "$HOST_ADDRESS" "$WHEP_PORT" <<'PY'
import json, sys
m = json.loads(sys.argv[1])
assert m["mode"] == "webrtc" and m["width"] == 1280 and m["height"] == 630 and m["fps"] == 25, m
assert m["whep"] == f"http://{sys.argv[2]}:{sys.argv[3]}/mxl-replay/mosaic/whep", m
assert [(t["id"], t["x"], t["y"], t["w"], t["h"]) for t in m["tiles"]] == [
    ("ch1", 0, 0, 640, 360), ("ch2", 640, 0, 640, 360), ("cam1", 0, 360, 480, 270), ("cam2", 480, 360, 480, 270)], m
PY
hls=0
for _ in $(seq 1 60); do
  # MediaMTX 1.20 answers the first request with a redirect (cookie check).
  if curl -sfL "http://127.0.0.1:${HLS_PORT}/mxl-replay/mosaic/index.m3u8" | grep -q '#EXTM3U'; then hls=1; break; fi
  sleep 0.5
done
[[ "$hls" == 1 ]] || { echo "own: no HLS playlist" >&2; exit 1; }
curl -sf -D "$WORK/widget.headers" -o /dev/null "http://127.0.0.1:${WEB_PORT}/widget/transport?channel=2&theme=transparent"
grep -qi "^content-security-policy: frame-ancestors 'self' http://designer.test" "$WORK/widget.headers" || { echo "own: widget CSP" >&2; exit 1; }
! grep -qi "^x-frame-options" "$WORK/widget.headers" || { echo "own: widget X-Frame-Options" >&2; exit 1; }
[[ "$(code "http://127.0.0.1:${WEB_PORT}/widget/transport?channel=3")" == 400 ]] || { echo "own: widget channel=3" >&2; exit 1; }
[[ "$(code "http://127.0.0.1:${WEB_PORT}/widget/clip-list")" == 200 ]] || { echo "own: clip-list widget" >&2; exit 1; }
curl -sf -D "$WORK/widgets.headers" -H 'Origin: http://designer.test' "http://127.0.0.1:${WEB_PORT}/widgets" |
  python3 -c 'import json,sys; assert [w["id"] for w in json.load(sys.stdin)] == ["transport", "clip-list"]'
grep -qi "^access-control-allow-origin: http://designer.test" "$WORK/widgets.headers" || { echo "own: /widgets CORS" >&2; exit 1; }
METRICS=$(curl -sf "http://127.0.0.1:${WEB_PORT}/metrics")
for line in 'preview_mode{mode="webrtc"} 1' 'preview_publish_mode{mode="own"} 1' 'preview_publish_state{state="publishing",stream="mxl-replay/mosaic"} 1'; do
  grep -qF "mxl_replay_${line}" <<<"$METRICS" || { echo "own: metric $line" >&2; exit 1; }
done
stop_replay
sleep 0.5
closed "$RTSP_PORT" || { echo "own: MediaMTX still runs after SIGTERM" >&2; exit 1; }
echo "own mode: ok"

# ---- 3. WebRTC, shared MediaMTX ------------------------------------------------------------------
cat >"$WORK/shared.yml" <<EOF
logLevel: warn
api: false
rtsp: true
rtspAddress: 127.0.0.1:${SHARED_RTSP_PORT}
rtspTransports: [tcp]
rtmp: false
srt: false
webrtc: false
hls: true
hlsAddress: 127.0.0.1:${SHARED_HLS_PORT}
paths:
  all_others:
EOF
start_shared() {
  "$MEDIAMTX" "$WORK/shared.yml" >>"$WORK/shared-mediamtx.log" 2>&1 &
  SHARED_PID=$!
}
start_shared
start_replay shared REPLAY_PREVIEW_MODE=webrtc PREVIEW_PUBLISH_URL="rtsp://127.0.0.1:${SHARED_RTSP_PORT}" PREVIEW_PATH_PREFIX=test-all/rp1
wait_state publishing 30
wait_describe "$SHARED_RTSP_PORT" test-all/rp1/mosaic 10
[[ "$(statusz "(p['publish'], 'mediamtx' in p, p['publish_url'], p['path'])")" == "('shared', False, 'rtsp://127.0.0.1:${SHARED_RTSP_PORT}', 'test-all/rp1/mosaic')" ]] || {
  echo "shared: statusz $(statusz p)" >&2
  exit 1
}
closed "$RTSP_PORT" || { echo "shared: an own MediaMTX runs" >&2; exit 1; }
kill "$SHARED_PID"
wait "$SHARED_PID" 2>/dev/null || true
SHARED_PID=""
wait_state error 15
start_shared
wait_state publishing 30
wait_describe "$SHARED_RTSP_PORT" test-all/rp1/mosaic 10
stop_replay
echo "shared mode: ok"
echo "preview integration ok"
