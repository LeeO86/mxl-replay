#!/usr/bin/env bash
# The state directory's config.json (1.8.2). 1: with NMOS_TAGS (JSON with quotes) in the environment, a
# settings save writes only the UI's key, and the replay starts again from that file. 2: a 1.8.1 file with
# the environment's keys loses them (state_config_platform_keys_dropped). 3: a config.json that cannot be
# read is moved aside to config.json.bad, logged (state_config_invalid), and the replay starts.
# CURL may name another curl (a lab wrapper); the default is curl.
set -euo pipefail
BIN=${1:-./build/mxl-replay}
CURL=${CURL:-curl}
DIR=$(mktemp -d)
PORT=${REPLAY_TEST_PORT:-18170}
PID=""
cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" || true
  fi
  rm -rf "$DIR"
}
trap cleanup EXIT

TAGS='{"urn:x-example:function":["replay"],"urn:x-example:area":["studio \"1\"","café"]}'

start() {
  REPLAY_FORMAT=64x32p50 \
  REPLAY_INPUTS=1 \
  REPLAY_CHANNELS=1 \
  REPLAY_BUFFER_HOURS=0.01 \
  REPLAY_STORAGE_DIR="$DIR/media" \
  REPLAY_STATE_DIR="$DIR/state" \
  REPLAY_STORAGE_MIN_MBPS=0 \
  REPLAY_SYNTHETIC=true \
  WEB_PORT="$PORT" \
  NMOS_ENABLE=false \
  NMOS_HOST_ADDRESS=10.255.0.10 \
  NMOS_TAGS="$TAGS" \
  MXL_DOMAIN_SCAN_PATH="$DIR/mxl" \
  MXL_OUTPUT_DOMAIN_DIR="$DIR/mxl/replay-ci" \
  MXL_OUTPUT_DOMAIN_ID=44444444-4444-4444-4444-444444444445 \
  HOST_ID=ci-replay \
  "$BIN" >>"$DIR/log.txt" 2>&1 &
  PID=$!
  for _ in $(seq 1 200); do
    if "$CURL" -sf "http://127.0.0.1:${PORT}/readyz" | grep -q ready; then
      return 0
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
      set +e
      wait "$PID"
      echo "replay exited before ready (exit $?)"
      set -e
      PID=""
      cat "$DIR/log.txt"
      exit 1
    fi
    sleep 0.1
  done
  echo "readyz did not succeed"
  cat "$DIR/log.txt"
  exit 1
}

stop() {
  kill -TERM "$PID"
  wait "$PID" || true
  PID=""
}

# 1. Save a setting with NMOS_TAGS in the environment, then start again.
start
"$CURL" -sf -X POST "http://127.0.0.1:${PORT}/api/v1/channels/1/play-on-tally" \
  -H 'content-type: application/json' -d '{"enable":false}' >/dev/null
grep -q CH1_PLAY_ON_TALLY "$DIR/state/config.json"
! grep -q NMOS_TAGS "$DIR/state/config.json" # only what the UI set is saved
stop
start
"$CURL" -sf "http://127.0.0.1:${PORT}/api/v1/status" | grep -q '"play_on_tally":false'
test ! -e "$DIR/state/config.json.bad"
stop

# 2. A 1.8.1 state file with the environment's keys in it: they are dropped (logged), the UI's stay.
printf '{\n  "NMOS_TAGS": "{}",\n  "HOST_ID": "old",\n  "CH1_PLAY_ON_TALLY": "false"\n}\n' >"$DIR/state/config.json"
start
grep -q '"state_config_platform_keys_dropped".*"keys":"HOST_ID,NMOS_TAGS"' "$DIR/log.txt"
grep -q CH1_PLAY_ON_TALLY "$DIR/state/config.json"
! grep -q -e NMOS_TAGS -e HOST_ID "$DIR/state/config.json"
"$CURL" -sf "http://127.0.0.1:${PORT}/api/v1/status" | grep -q '"play_on_tally":false'
stop

# 3. A state file that cannot be read (cut off: no closing brace).
printf '{\n  "CH1_LABEL": "say \\"hi\\"",\n  "CH1_PLAY_ON_TALLY": "false"\n' >"$DIR/state/config.json"
cp "$DIR/state/config.json" "$DIR/expected.bad"
start
cmp "$DIR/state/config.json.bad" "$DIR/expected.bad"
test ! -e "$DIR/state/config.json"
grep -q '"state_config_invalid"' "$DIR/log.txt"
"$CURL" -sf "http://127.0.0.1:${PORT}/api/v1/status" | grep -q '"play_on_tally":true'
stop
echo "state config ok"
