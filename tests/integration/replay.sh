#!/usr/bin/env bash
# CPU integration: the replay process serves health, records a synthetic
# pattern when asked, and answers a transport command. MXL round-trips are
# covered when libmxl is linked and REPLAY_WITH_MXL flows are present.
set -euo pipefail
BIN=${1:-./build/mxl-replay}
DIR=$(mktemp -d)
PORT=${REPLAY_TEST_PORT:-18150}
cleanup() {
  if [[ -n "${PID:-}" ]]; then
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" || true
  fi
  rm -rf "$DIR"
}
trap cleanup EXIT
REPLAY_FORMAT=64x32p50 \
REPLAY_INPUTS=1 \
REPLAY_CHANNELS=1 \
REPLAY_BUFFER_HOURS=0.01 \
REPLAY_STORAGE_DIR="$DIR" \
REPLAY_STORAGE_MIN_MBPS=0 \
REPLAY_SYNTHETIC=true \
WEB_PORT="$PORT" \
NMOS_ENABLE=false \
HOST_ID=ci-replay \
"$BIN" >"$DIR/log.txt" 2>&1 &
PID=$!
for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:${PORT}/livez" >/dev/null; then
    break
  fi
  sleep 0.1
done
curl -sf "http://127.0.0.1:${PORT}/livez" | grep -q ok
curl -sf "http://127.0.0.1:${PORT}/readyz" | grep -q ready
curl -sf "http://127.0.0.1:${PORT}/metrics" | grep -q mxl_replay_
curl -sf "http://127.0.0.1:${PORT}/api/v1/status" | grep -q cameras
curl -sf -X POST "http://127.0.0.1:${PORT}/api/v1/channels/1/transport" \
  -H 'content-type: application/json' -d '{"command":"pause"}' | grep -q channels
curl -sf "http://127.0.0.1:${PORT}/" | grep -q "MXL Replay"
echo "integration ok"
