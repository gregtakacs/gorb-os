#!/bin/bash
# bench_d32.sh — D32 ruling R-H on the glass: the carriage and the window, flashed.
#
# Proves the slice-2 projection on the bound board:
#   (1) serflash lands the committed build (its stamp audit refuses an image
#       built from uncommitted edits — the log line this bench greps is part of
#       what must be on the board);
#   (2) the d32-bench bundle (tools/bench_d32_bundle.py, host-verified through
#       the shipped C first) UPLOADS and the device's own reader accepts it
#       ("state":"stored");
#   (3) the FLIP door lands it: reboot boundary, live=slot-B, active=d32-bench,
#       and the boot log carries the loader's projection line with the exact
#       figures the bundle carries (1 comp row, tourbillon_n 8 @ 12 fps,
#       start 4500);
#   (4) the heap question the ruling reserved for the bench: heap_psram before
#       and after the flip, printed for the record;
#   (5) the dial keeps ticking (min clock fps over 30 s on the new face — the
#       carriage swaps at 12 fps while the second hand lives, R-C), then
#       sd/default puts the board back.
#
# Run DETACHED (the whole bench is minutes, not seconds):
#   setsid tools/bench_d32.sh > /tmp/d32_run.log 2>&1 < /dev/null &
set -u
SER=/tmp/d32b_ser.txt
BODY=/tmp/d32b_body.json
FIX=/tmp/d32bench.orbtheme
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$REPO" || exit 1
. tools/orb_bind.sh
orb_require
ORB=${ORB:-http://$ORB_IP}
[ "$ORB" = "http://$ORB_IP" ] || orb_die "ORB=$ORB is not the bound orb $ORB_LABEL"
say(){ echo "[$(date +%T)] $*"; }
verdict=PASS
fail(){ say "FAIL: $*"; verdict=FAIL; }
fetch(){ local m=$1 u=$2; shift 2
  rm -f "$BODY"
  CODE=$(curl -s -m 240 -X "$m" "$@" -o "$BODY" -w '%{http_code}:%{size_download}' "$u")
  [ "${CODE#*:}" != ":0" ] && [ "${CODE##*:}" -gt 0 ] 2>/dev/null
}
dial(){ awk -v e="$1" '$1+0>e && /clock: clock [0-9]+ fps/ {match($0,/clock [0-9]+ fps/); f=substr($0,RSTART+6,RLENGTH-5)+0; if(m==0||f<m)m=f} END{print m+0}' "$SER"; }

# [1] the fixture, verified again right here: the reader, and the name rule.
python3 - <<'PY' || exit 1
import sys; sys.path.insert(0, "tools")
import orbbundle as ob, orbtheme as ot
h, _ = ob.read_bundle("/tmp/d32bench.orbtheme")
assert h["name"] == "d32-bench", h["name"]
print("fixture ok: id", ot.theme_id(h["name"]), "art", h["art_size"])
PY
ID=$(python3 -c "import sys; sys.path.insert(0,'tools'); import orbtheme; print(orbtheme.theme_id('d32-bench'))")

# [2] flash the committed build (serflash proves the MAC and the stamp)
say 'flashing the bound orb'
tools/serflash.sh >/tmp/d32b_flash.log 2>&1 || { tail -5 /tmp/d32b_flash.log; fail 'serflash'; exit 1; }
say 'flash done'

# [3] capture from the open-reset boot; wait for the lease
rm -f "$SER"
setsid python3 tools/capf.py 900 "$SER" >/dev/null 2>&1 &
CAP=$!
for i in $(seq 1 150); do sleep 1; grep -q 'sta ip' "$SER" 2>/dev/null && break; done
grep -q 'sta ip' "$SER" || { fail 'no lease in 150 s'; kill $CAP; exit 1; }
say "lease up; capture at $SER"
fetch GET "$ORB/api/v1/status" && PSRAM0=$(python3 -c "import json;print(json.load(open('$BODY'))['heap_psram'])")
say "psram baseline on the base face: ${PSRAM0:-?}"

# [4] upload, and confirm the card lists the id it derives from the name
fetch POST "$ORB/api/v1/sd/rescan" && say "rescan: $(head -c 200 "$BODY")"
fetch POST "$ORB/api/v1/sd/theme?overwrite=1" --data-binary @"$FIX" -H 'Content-Type: application/octet-stream' \
  || fail 'upload returned no body'
say "upload: $(cat "$BODY")"
grep -q '"state":"stored"' "$BODY" || fail 'upload not stored'
sleep 2
fetch GET "$ORB/api/v1/sd"
grep -q "\"id\":\"$ID\"" "$BODY" || fail 'the card does not list the fixture id'

# [5] the flip: press (waiting out a busy queue), expect the boundary
for r in $(seq 1 25); do
  fetch POST "$ORB/api/v1/sd/flip?id=$ID" || { fail 'flip: no body'; exit 1; }
  grep -q 'already running' "$BODY" || break
  say 'flip: queue busy — waiting'
  sleep 2
done
say "flip: $CODE -> $(cat "$BODY")"
grep -q '"state":"flipping"' "$BODY" || fail 'flip did not say flipping'
E=$(date +%s)
R=""
for j in $(seq 1 45); do sleep 1
  R=$(awk -v e="$E" '$1+0>(e-2) && /rst:0x/ {print $1; exit}' "$SER"); [ -n "$R" ] && break
done
[ -n "$R" ] || { fail 'flip promised a boundary and none came'; kill $CAP; exit 1; }
say "reboot boundary at $R — press to boundary ${j}s"
for i in $(seq 1 90); do sleep 1; awk -v r="$R" '$1+0>r && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
sleep 3
fetch GET "$ORB/api/v1/sd" || { fail 'no body after re-lease'; exit 1; }
say "after flip: $(head -c 300 "$BODY")"
grep -q '"live":"slot-B"' "$BODY" || fail 'live is not slot-B'
grep -q '"active":"d32-bench"' "$BODY" || fail 'active is not d32-bench'
say "refill/bundle cost: $(python3 -c "import json;d=json.load(open('$BODY'));print(d['refill_ms'],'ms /',d['refill_bytes'],'B | bundle',d['bundle_ms'],'ms /',d['bundle_bytes'],'B')" 2>/dev/null)"

# [6] the projection line, from THIS boot: exactly the figures the bundle carries
BOOT=$(awk -v r="$R" '$1+0>r' "$SER")
echo "$BOOT" | grep 'projected into a dial' | tail -1 | tee -a /dev/stdout \
  | grep -q '1 complication row' || fail 'no projection line, or not one comp row'
echo "$BOOT" | grep -q 'tourbillon_n 8 @ 12 fps start 4500' \
  || fail 'the projection line does not name the carriage the bundle carries'
echo "$BOOT" | grep -q 'no drawable clock' && fail 'the device refused its own bench bundle'

# [7] the heap question, and the glass keeping time
fetch GET "$ORB/api/v1/status" && PSRAM1=$(python3 -c "import json;print(json.load(open('$BODY'))['heap_psram'])")
say "psram on the carriage face: ${PSRAM1:-?} (baseline ${PSRAM0:-?})"
sleep 30
MIND=$(dial "$R")
say "min clock fps across 30 s on the new face: $MIND"
[ "${MIND:-0}" -ge 4 ] || fail 'the dial fell under 4 fps — a marking bug, not a missing feature'

# [8] put the board back
fetch POST "$ORB/api/v1/sd/default" || fail 'sd/default returned no body'
E3=$(date +%s)
for j in $(seq 1 45); do sleep 1
  awk -v e="$E3" '$1+0>(e-2) && /rst:0x/ {f=1} END{exit !f}' "$SER" && break
done
for i in $(seq 1 90); do sleep 1; awk -v e="$E3" '$1+0>(e-2) && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
sleep 3
fetch GET "$ORB/api/v1/sd" && say "after default: $(head -c 200 "$BODY")"
grep -q '"live":"default"' "$BODY" || fail 'flip-back did not land on live=default'
kill $CAP 2>/dev/null
say "=== D32 BENCH $verdict ==="

