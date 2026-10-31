#!/bin/bash
# Theme-store check for a release candidate: a slot write while the radio is busy,
# under a serial capture, on the bound orb. v1.1.41 shipped without one and a hub
# send panicked mid-slot-write (2026-10-31); run every mode before a firmware ships.
#
#   quiet     store a card theme with nothing else talking to the orb
#   status    the same, with /status polled every 200 ms through the write
#   previews  the same, with preview planes streaming through the write
#   upload    the hub's send: raw-body upload to the card (the orb stores it by
#             itself) with the hub's 700 ms progress poll. The card copy must not
#             already exist, or the orb answers "unchanged" and writes nothing;
#             delete it first unless it is the active theme (that reboots the dial).
#
# Holds the cable's .press flag (the hub declines a pressed cable), opens the port
# (which resets the board once), stores, and reports the store time and any panic
# from the capture. Clears the clock's fault count afterwards, because the capture's
# reset is an unexpected one and two of them mark the clock failed.
#
# Usage: tools/bench/store_check.sh <mode> <theme-id | file.orbtheme> [capture-file]
set -u
MODE=${1:?mode: quiet|status|previews|upload}
WHAT=${2:?theme id, or the .orbtheme file for upload}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${3:-/tmp/store-check-$MODE.txt}
IP=$(sed -n 's/^ORB_IP=//p' "$ROOT/.orb-target")
SERIAL=$(sed -n 's/^ORB_SERIAL="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$ROOT/.orb-target")
PRESS="/tmp/orb-port-$(basename "$(readlink -f "$SERIAL")").press"
[ -n "$IP" ] && [ -n "$SERIAL" ] || { echo "no ORB_IP/ORB_SERIAL in $ROOT/.orb-target" >&2; exit 2; }

run() {
  cd "$ROOT"
  python3 tools/capf.py 150 "$OUT" >/dev/null 2>&1 &
  local cp=$!
  for _ in $(seq 1 60); do      # wait for the capture's own reset to land
    u=$(curl -s -m 3 "http://$IP/api/v1/status" | python3 -c 'import json,sys;print(json.load(sys.stdin)["idle"]["since_s"])' 2>/dev/null)
    [ -n "$u" ] && [ "$u" -lt 60 ] && break
    sleep 2
  done
  sleep 12
  local tp=""
  case "$MODE" in
    status)
      ( end=$((SECONDS+40)); while [ $SECONDS -lt $end ]; do
          curl -s -m 2 -o /dev/null "http://$IP/api/v1/status"; sleep 0.2; done ) &
      tp=$! ;;
    previews)
      ids=$(curl -s -m 20 "http://$IP/api/v1/sd" | python3 -c 'import json,sys;print(" ".join(t["id"] for t in json.load(sys.stdin).get("themes",[])))')
      ( end=$((SECONDS+40)); while [ $SECONDS -lt $end ]; do
          for t in $ids; do curl -s -m 10 -o /dev/null "http://$IP/api/v1/sd/preview?id=$t"; done; done ) &
      tp=$!; sleep 0.3 ;;
    upload)
      ( end=$((SECONDS+90)); while [ $SECONDS -lt $end ]; do
          curl -s -m 3 -o /dev/null "http://$IP/api/v1/sd"; sleep 0.7; done ) &
      tp=$! ;;
  esac
  if [ "$MODE" = upload ]; then
    echo "upload: $(curl -s -m 120 -H 'Expect:' -H 'Content-Type: application/octet-stream' \
      --data-binary @"$WHAT" -w ' [http %{http_code}, %{time_total}s]' "http://$IP/api/v1/sd/theme?overwrite=1")"
  else
    echo "store: $(curl -s -m 30 -X POST "http://$IP/api/v1/sd/bundle?id=$WHAT")"
  fi
  [ -n "$tp" ] && wait "$tp"
  wait "$cp"
}

exec 9>"$PRESS"
flock -n 9 || { echo "$PRESS is held: another flash or capture owns the cable" >&2; exit 2; }
run
curl -s -m 10 -X POST "http://$IP/api/v1/apps/clear?id=clock" -o /dev/null
ms=$(sed -n 's/.*in slot in \([0-9]*\) ms.*/\1/p' "$OUT" | head -1)
panics=$(grep -cE "Guru Meditation|abort\(\) was called|Backtrace:" "$OUT")
staged=$(curl -s -m 20 "http://$IP/api/v1/sd" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("staged_why",""))' 2>/dev/null)
echo "store_check $MODE: store ${ms:-none} ms, panic lines $panics, staged '${staged}', capture $OUT"
[ -n "$ms" ] && [ "$panics" = 0 ] && [ -z "$staged" ]
