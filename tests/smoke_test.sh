#!/usr/bin/env bash
# Smoke test for the event-driven load balancer.
# Starts an echo backend + lb, then uses the concurrent_client tester to open
# N simultaneous connections and verify each round-trips correctly through lb.
set -u

LB_PORT=18080
BE_PORT=19001
N=${1:-100}

HERE="$(cd "$(dirname "$0")/.." && pwd)"
LB="$HERE/build/lb"
ECHO="$HERE/build/echo_backend"
CC="$HERE/build/concurrent_client"

for bin in "$LB" "$ECHO" "$CC"; do
  [ -x "$bin" ] || { echo "missing $bin (build first with cmake --build build)"; exit 2; }
done

cleanup() {
  [ -n "${LPID:-}" ] && kill "$LPID" 2>/dev/null
  [ -n "${EPID:-}" ] && kill "$EPID" 2>/dev/null
}
trap cleanup EXIT

"$ECHO" "$BE_PORT" >/dev/null 2>&1 &
EPID=$!
"$LB" "$LB_PORT" "127.0.0.1:$BE_PORT" >/dev/null 2>&1 &
LPID=$!
sleep 1

echo "opening $N simultaneous connections through lb:$LB_PORT ..."
if "$CC" 127.0.0.1 "$LB_PORT" "$N" 2>/dev/null; then
  echo "PASS"
  exit 0
else
  echo "FAIL"
  exit 1
fi
