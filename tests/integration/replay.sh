#!/usr/bin/env bash
# Start the replay, wait until it is ready (including NMOS registration when a
# registry is configured), then SIGTERM it. With MXL_CLEANUP_ON_EXIT the
# process removes only its own output domain, and the registry sees a DELETE.
set -euo pipefail
BIN=${1:-./build/mxl-replay}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR=$(mktemp -d)
PORT=${REPLAY_TEST_PORT:-18150}
NMOS_PORT_VALUE=${REPLAY_TEST_NMOS_PORT:-18152}
REG_PORT=${REPLAY_TEST_REG_PORT:-18160}
QUERY_PORT=${REPLAY_TEST_QUERY_PORT:-18161}
PID=""
REGPID=""
cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" || true
  fi
  if [[ -n "${REGPID}" ]] && kill -0 "$REGPID" 2>/dev/null; then
    kill "$REGPID" 2>/dev/null || true
    wait "$REGPID" || true
  fi
  rm -rf "$DIR"
}
trap cleanup EXIT

python3 "$ROOT/tests/integration/registry_stub.py" "$REG_PORT" "$QUERY_PORT" "$DIR/registry.log" >"$DIR/registry.out" 2>&1 &
REGPID=$!
for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:${REG_PORT}/" >/dev/null; then
    break
  fi
  sleep 0.05
done
curl -sf "http://127.0.0.1:${REG_PORT}/" >/dev/null

HOST_ADDRESS=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1 || true)
if [[ -z "$HOST_ADDRESS" ]]; then
  HOST_ADDRESS=10.255.0.10
fi

REPLAY_FORMAT=64x32p50 \
REPLAY_INPUTS=1 \
REPLAY_CHANNELS=1 \
REPLAY_BUFFER_HOURS=0.01 \
REPLAY_STORAGE_DIR="$DIR/media" \
REPLAY_STATE_DIR="$DIR/state" \
REPLAY_STORAGE_MIN_MBPS=0 \
REPLAY_SYNTHETIC=true \
WEB_PORT="$PORT" \
NMOS_ENABLE=true \
NMOS_DNS_SD=false \
NMOS_PORT="$NMOS_PORT_VALUE" \
NMOS_REGISTRY_ADDRESS=127.0.0.1 \
NMOS_REGISTRY_PORT="$REG_PORT" \
NMOS_QUERY_ADDRESS=127.0.0.1 \
NMOS_QUERY_PORT="$QUERY_PORT" \
NMOS_HOST_ADDRESS="$HOST_ADDRESS" \
NMOS_SEED=ci-replay \
NMOS_LABEL="CI Replay" \
MXL_DOMAIN_SCAN_PATH="$DIR/mxl" \
MXL_OUTPUT_DOMAIN_DIR="$DIR/mxl/replay-ci" \
MXL_OUTPUT_DOMAIN_ID=44444444-4444-4444-4444-444444444444 \
MXL_CLEANUP_ON_EXIT=true \
SHUTDOWN_TIMEOUT_S=10 \
HOST_ID=ci-replay \
"$BIN" >"$DIR/log.txt" 2>&1 &
PID=$!

ready=0
for _ in $(seq 1 200); do
  if curl -sf "http://127.0.0.1:${PORT}/readyz" | grep -q ready; then
    ready=1
    break
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "replay exited before ready"
    cat "$DIR/log.txt"
    exit 1
  fi
  sleep 0.1
done
if [[ "$ready" != 1 ]]; then
  echo "readyz did not succeed"
  cat "$DIR/log.txt"
  exit 1
fi

curl -sf "http://127.0.0.1:${PORT}/livez" | grep -q ok
curl -sf "http://127.0.0.1:${PORT}/metrics" | grep -q mxl_replay_
curl -sf "http://127.0.0.1:${PORT}/api/v1/status" | grep -q cameras
curl -sf "http://127.0.0.1:${PORT}/api/v1/nmos" | grep -q "$HOST_ADDRESS"
curl -sf -X POST "http://127.0.0.1:${PORT}/api/v1/channels/1/transport" \
  -H 'content-type: application/json' -d '{"command":"pause"}' | grep -q channels
NODE_ID=$(curl -sf "http://127.0.0.1:${PORT}/api/v1/nmos" | sed -n 's/.*"node_id":"\([^"]*\)".*/\1/p')
test -n "$NODE_ID"
if ! grep -q mxl_disabled "$DIR/log.txt"; then
  test -f "$DIR/mxl/replay-ci/domain_def.json"
fi

kill -TERM "$PID"
set +e
wait "$PID"
code=$?
set -e
PID=""
if [[ "$code" != 143 ]]; then
  echo "expected exit 143, got $code"
  cat "$DIR/log.txt"
  exit 1
fi
if ! grep -q mxl_disabled "$DIR/log.txt"; then
  if [[ -d "$DIR/mxl/replay-ci" ]]; then
    echo "own domain was not removed"
    cat "$DIR/log.txt"
    exit 1
  fi
fi
if ! grep -q "DELETE /x-nmos/registration/v1.3/resource/nodes/${NODE_ID}" "$DIR/registry.log"; then
  echo "node was not deregistered"
  cat "$DIR/registry.log" || true
  cat "$DIR/log.txt"
  exit 1
fi
echo "integration ok"
