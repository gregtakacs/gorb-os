#!/bin/bash
# Flight-feed viability bench (docs/flight-tracker-design.md §6).
#
# One serial capture from a fresh boot, then timed arms switched over the debug
# door, with a /api/v1/debug/flights snapshot at every boundary. No hub runs:
# the scenario is the standalone orb, the clock on the glass, the feed polling.
# tools/bench/flights_report.py reads the output directory.
#
#   tools/bench/flights_bench.sh OUTDIR [scale]
#
# scale multiplies every arm's minutes (default 1; 0.2 for a quick shake-out).
set -u
OUT=${1:?outdir}; SCALE=${2:-1}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tools/orb_bind.sh" >/dev/null 2>&1 || true
IP=${ORB_IP:-192.168.2.32}
mkdir -p "$OUT"; : > "$OUT/arms.tsv"

# name|minutes|query (empty = off)
ARMS=(
  "settle|2|"
  "off|15|on=0"
  "ka10|20|on=1&ka=1&period=10000&src=0&lat=33.45&lon=-112.07&nm=40"
  "hs10|15|on=1&ka=0&period=10000&src=0&lat=33.45&lon=-112.07&nm=40"
  "fi10|5|on=1&ka=1&period=10000&src=1&lat=33.45&lon=-112.07&nm=40"
  "stress5|10|on=1&ka=0&period=5000&src=0&lat=33.94&lon=-118.40&nm=100"
  "off2|10|on=0"
)
total=0
for a in "${ARMS[@]}"; do IFS='|' read -r _ m _ <<<"$a"; total=$(python3 -c "print($total+$m*$SCALE)"); done
secs=$(python3 -c "print(int($total*60+900))")   # + boot, + a slow reach (above)

if pgrep -f 'orb-manager' >/dev/null; then echo "a hub is running; stop it first" >&2; exit 1; fi
python3 "$ROOT/tools/capf.py" "$secs" "$OUT/serial.txt" >/dev/null 2>&1 &
CAP=$!
# Opening the port resets the board: let that land before asking whether it is
# up, or the answer comes from the image still running before the reset.
sleep 8
# The orb can be associated and serving while the host still cannot reach it
# (2026-10-31: ~4 min after a boot that touched one mesh node before settling
# on another). So every call retries until it is answered, and an arm's clock
# starts only once its config has landed.
until curl -s -m 3 -o /dev/null "http://$IP/api/v1/status"; do sleep 2; done
retry() { for _ in $(seq 1 60); do "$@" && return 0; sleep 3; done; return 1; }
post() { curl -sf -m 6 -X POST -H 'Content-Length: 0' "http://$IP/api/v1/debug/flights?$1" -o /dev/null; }
snap1() { curl -sf -m 6 "http://$IP/api/v1/debug/flights" -o "$OUT/snap-$1.json" && [ -s "$OUT/snap-$1.json" ]; }
snap() { retry snap1 "$1" || echo "snapshot $1 never answered" >&2; }
for a in "${ARMS[@]}"; do
  IFS='|' read -r name m q <<<"$a"
  if [ -n "$q" ]; then
    retry post "$q" || echo "arm $name: config never landed" >&2
  fi
  snap "$name-start"
  printf '%s\t%s\t%s\n' "$name" "$(date +%s.%N)" "$q" >> "$OUT/arms.tsv"
  sleep "$(python3 -c "print(int($m*$SCALE*60))")"
  snap "$name-end"
done
printf 'end\t%s\t\n' "$(date +%s.%N)" >> "$OUT/arms.tsv"
curl -s -m 6 -X POST -H 'Content-Length: 0' "http://$IP/api/v1/debug/flights?on=0" -o /dev/null
kill $CAP 2>/dev/null; wait $CAP 2>/dev/null
echo "BENCH_DONE $OUT"
