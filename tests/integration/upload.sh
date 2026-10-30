#!/usr/bin/env bash
# Uploads over HTTP (1.8.2): the body goes to REPLAY_STORAGE_DIR/incoming as it arrives and is converted
# frame by frame. A 30 s 25p YUV4MPEG2 file (about 33 MB) becomes a 1500-frame clip in a 50p house,
# a WAV an audio item, a body over REPLAY_UPLOAD_MAX_MB is answered 413, /livez answers during an
# upload, a damaged JPEG is answered 400 (the process stays), and no body file is left behind. CURL may name another curl (a lab wrapper); the default is curl.
set -euo pipefail
BIN=${1:-./build/mxl-replay}
CURL=${CURL:-curl}
DIR=$(mktemp -d)
PORT=${REPLAY_TEST_PORT:-18171}
PID=""
cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" || true
  fi
  rm -rf "$DIR"
}
trap cleanup EXIT

# 750 frames of 320×180 at 25p (30 s), luma rising per frame; 2 s of a 48 kHz stereo WAV.
python3 - "$DIR" <<'EOF'
import struct, sys, math
d = sys.argv[1]
w, h = 320, 180
with open(f"{d}/clip.y4m", "wb") as f:
    f.write(f"YUV4MPEG2 W{w} H{h} F25:1 Ip A1:1 C420jpeg\n".encode())
    for i in range(750):
        f.write(b"FRAME\n" + bytes([16 + i % 200]) * (w * h) + bytes([128]) * (w * h // 2))
n = 96000
data = b"".join(struct.pack("<hh", int(8000 * math.sin(i / 10)), 0) for i in range(n))
with open(f"{d}/tone.wav", "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16))
    f.write(b"data" + struct.pack("<I", len(data)) + data)
with open(f"{d}/damaged.jpg", "wb") as f:
    f.write(b"\xff\xd8" + b"\x42" * 500)
with open(f"{d}/big.bin", "wb") as f:
    f.write(b"\0" * (3 * 1024 * 1024))
EOF

REPLAY_FORMAT=320x180p50 \
REPLAY_INPUTS=1 \
REPLAY_CHANNELS=1 \
REPLAY_BUFFER_HOURS=0.01 \
REPLAY_STORAGE_DIR="$DIR/media" \
REPLAY_STATE_DIR="$DIR/state" \
REPLAY_STORAGE_MIN_MBPS=0 \
REPLAY_SYNTHETIC=true \
REPLAY_UPLOAD_MAX_MB=2 \
WEB_PORT="$PORT" \
NMOS_ENABLE=false \
NMOS_HOST_ADDRESS=10.255.0.10 \
MXL_DOMAIN_SCAN_PATH="$DIR/mxl" \
MXL_OUTPUT_DOMAIN_DIR="$DIR/mxl/replay-ci" \
MXL_OUTPUT_DOMAIN_ID=44444444-4444-4444-4444-444444444447 \
HOST_ID=ci-replay \
"$BIN" >"$DIR/log.txt" 2>&1 &
PID=$!
for _ in $(seq 1 200); do
  if "$CURL" -sf "http://127.0.0.1:${PORT}/readyz" | grep -q ready; then
    break
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "replay exited before ready"
    cat "$DIR/log.txt"
    exit 1
  fi
  sleep 0.1
done

# Over the limit: 413, before any conversion.
code=$("$CURL" -s -o "$DIR/big.json" -w '%{http_code}' -X POST -H 'content-type: application/octet-stream' \
  --data-binary "@$DIR/big.bin" "http://127.0.0.1:${PORT}/api/v1/uploads?name=big")
test "$code" = 413
grep -q REPLAY_UPLOAD_MAX_MB "$DIR/big.json"
kill -TERM "$PID"
wait "$PID" || true
PID=""

# Without the limit: the clip, with /livez answered while it converts.
REPLAY_FORMAT=320x180p50 REPLAY_INPUTS=1 REPLAY_CHANNELS=1 REPLAY_BUFFER_HOURS=0.01 \
REPLAY_STORAGE_DIR="$DIR/media" REPLAY_STATE_DIR="$DIR/state" REPLAY_STORAGE_MIN_MBPS=0 REPLAY_SYNTHETIC=true \
WEB_PORT="$PORT" NMOS_ENABLE=false NMOS_HOST_ADDRESS=10.255.0.10 MXL_DOMAIN_SCAN_PATH="$DIR/mxl" \
MXL_OUTPUT_DOMAIN_DIR="$DIR/mxl/replay-ci" MXL_OUTPUT_DOMAIN_ID=44444444-4444-4444-4444-444444444447 HOST_ID=ci-replay \
"$BIN" >>"$DIR/log.txt" 2>&1 &
PID=$!
for _ in $(seq 1 200); do
  if "$CURL" -sf "http://127.0.0.1:${PORT}/readyz" | grep -q ready; then
    break
  fi
  sleep 0.1
done
"$CURL" -sf -o "$DIR/clip.json" -X POST -H 'content-type: application/octet-stream' \
  --data-binary "@$DIR/clip.y4m" "http://127.0.0.1:${PORT}/api/v1/uploads?name=long" &
UPLOAD=$!
during=0
for _ in $(seq 1 100); do
  if ! kill -0 "$UPLOAD" 2>/dev/null; then
    break
  fi
  if "$CURL" -sf -m 2 "http://127.0.0.1:${PORT}/livez" | grep -q ok; then
    during=$((during + 1))
  fi
  sleep 0.05
done
wait "$UPLOAD"
grep -q '"kind":"clip"' "$DIR/clip.json"
echo "livez answered $during times during the upload"
"$CURL" -sf "http://127.0.0.1:${PORT}/api/v1/clips" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
clips = doc if isinstance(doc, list) else doc["clips"]
long = [c for c in clips if c["name"] == "long"]
assert long, clips
frames = (long[0]["out_ns"] - long[0]["in_ns"]) // 20000000 + 1
assert frames == 1500, frames
'
"$CURL" -sf -X POST -H 'content-type: application/octet-stream' --data-binary "@$DIR/tone.wav" \
  "http://127.0.0.1:${PORT}/api/v1/uploads?name=tone" | grep -q '"kind":"audio"'
# A damaged JPEG: 400, and the replay keeps running (libjpeg's default error handler called exit()).
code=$("$CURL" -s -o "$DIR/damaged.json" -w '%{http_code}' -X POST -H 'content-type: image/jpeg' \
  --data-binary "@$DIR/damaged.jpg" "http://127.0.0.1:${PORT}/api/v1/uploads?name=damaged")
test "$code" = 400
kill -0 "$PID"
"$CURL" -sf "http://127.0.0.1:${PORT}/livez" | grep -q ok
test -z "$(ls -A "$DIR/media/incoming")"
kill -TERM "$PID"
wait "$PID" || true
PID=""
echo "upload ok"
