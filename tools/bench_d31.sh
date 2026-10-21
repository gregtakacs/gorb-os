#!/bin/bash
# bench_d31.sh — §31 on the glass: the animated plate, flashed.
#
# Proves the plate-bank slice on the bound board — the one question the host
# suites cannot answer, because no FORMAT 3 theme so far ever drove the
# bgAnim flipper:
#   (1) the fixture (tools/bench_d31_bundle.py) projects through the SHIPPED C
#       on the host before a board is touched (verify_js_bake.py says
#       "plate bank: 3 extra keyframes");
#   (2) serflash lands the committed build (stamp audit as always);
#   (3) the bundle UPLOADS and the device's reader accepts it (stored);
#   (4) the FLIP lands it, and the boot log carries the §31 loader line with
#       the exact figures the bundle carries (plate bank: 4 keyframes @ 8 fps,
#       loop) — the device saw a bank, not a still;
#   (5) the GLASS moves: two fbgrabs inside one bank cycle differ by far more
#       pixels than hands alone can account for (the medallion is ~a third of
#       the panel; the hands, seconds included, move a few percent);
#   (6) the heap delta before/after the flip, printed for the record, and the
#       per-second clock fps on the animated face — §31.1's requested-vs-
#       delivered figure: bg_fps asked 8, the flush floor may give less, and
#       the run says which;
#   (7) sd/default puts the board back.
#
# Run DETACHED (the whole bench is minutes, not seconds):
#   setsid tools/bench_d31.sh > /tmp/d31_run.log 2>&1 < /dev/null &
set -u
SER=/tmp/d31b_ser.txt
BODY=/tmp/d31b_body.json
FIX=/tmp/d31bench.orbtheme
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
dialmax(){ awk -v e="$1" '$1+0>e && /clock: clock [0-9]+ fps/ {match($0,/clock [0-9]+ fps/); f=substr($0,RSTART+6,RLENGTH-5)+0; if(f>x)x=f} END{print x+0}' "$SER"; }

# [1] the fixture: build it, and let the shipped C project it here first.
python3 tools/bench_d31_bundle.py "$FIX" || exit 1
python3 tools/verify_js_bake.py "$FIX" 2>&1 | tee /tmp/d31b_proj.txt >/dev/null
grep -q 'plate bank: 3 extra keyframes at 434312, 868624, 1302936' /tmp/d31b_proj.txt \
  || { echo "host projection did not seat the bank:"; cat /tmp/d31b_proj.txt; exit 1; }
say "host C projection seats the bank"
ID=$(python3 -c "import sys; sys.path.insert(0,'tools'); import orbtheme; print(orbtheme.theme_id('d31-anim'))")

# [2] flash the committed build (serflash proves the MAC and the stamp)
say 'flashing the bound orb'
tools/serflash.sh >/tmp/d31b_flash.log 2>&1 || { tail -5 /tmp/d31b_flash.log; fail 'serflash'; exit 1; }
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

# [5] the STORE door — the two-stage press §26.3 rules: the flip only lands an
# id a slot already holds, and the store answers reboot:none.
T0=$(date +%s.%N)
fetch POST "$ORB/api/v1/sd/bundle?id=$ID" || { fail 'store: no body'; kill $CAP; exit 1; }
T1=$(date +%s.%N)
say "store: $(cat "$BODY") ($(python3 -c "print(round($T1-$T0,2))")s door-to-body)"
grep -q '"reboot":"none"' "$BODY" || fail 'store did not answer reboot:none'

# [6] then the FLIP: press (waiting out a busy queue), expect the boundary
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
grep -q '"active":"d31-anim"' "$BODY" || fail 'active is not d31-anim'

# [7] the projection lines, from THIS boot: the dial, and §31's bank figure
BOOT=$(awk -v r="$R" '$1+0>r' "$SER")
echo "$BOOT" | grep 'projected into a dial' | tail -1 | tee -a /dev/stdout >/dev/null
echo "$BOOT" | grep -q 'no drawable clock' && fail 'the device refused its own bench bundle'
BLINE=$(echo "$BOOT" | grep 'plate bank' | tail -1)
say "boot line: ${BLINE:-NONE}"
echo "$BLINE" | grep -q '4 keyframes @ 8 fps, loop' \
  || fail 'no §31 plate-bank line with the bundle-s own figures'

# [8] the glass moves: two grabs a bank stride apart (the ladder holds each
# keyframe ~2 ticks of a ~67 ms tick, so ~0.15 s between grabs lands inside
# one cycle of four). The medallion is ~a third of the panel: a flipping bank
# moves far more pixels than a seconds hand can.
sleep 2
python3 tools/fbgrab.py --out /tmp/d31b_g1.bmp --quiet >/dev/null 2>&1 || fail 'grab 1'
sleep 0.16
python3 tools/fbgrab.py --out /tmp/d31b_g2.bmp --quiet >/dev/null 2>&1 || fail 'grab 2'
DIFF=$(python3 tools/fbgrab.py --diff /tmp/d31b_g1.bmp /tmp/d31b_g2.bmp 2>&1 \
        | tee /tmp/d31b_diff.txt | sed -n 's/.*: \([0-9.]*\)% pixels changed.*/\1/p')
say "glass diff between grabs: ${DIFF:-?}% changed"
python3 -c "import sys; sys.exit(0 if float('${DIFF:-0}') > 3.0 else 1)" \
  || fail 'the plate did not move on the glass'

# [9] the heap question, and the tempo figure §31 asked: bg_fps 8 requested,
# what the flush floor actually delivered on the animated face.
fetch GET "$ORB/api/v1/status" && PSRAM1=$(python3 -c "import json;print(json.load(open('$BODY'))['heap_psram'])")
say "psram on the animated face: ${PSRAM1:-?} (baseline ${PSRAM0:-?})"
sleep 30
MIND=$(dial "$R"); MAXD=$(dialmax "$R")
say "clock fps across 30 s on the animated face: min $MIND max $MAXD (requested bg_fps 8)"
[ "${MIND:-0}" -ge 4 ] || fail 'the dial fell under 4 fps — a marking bug, not a missing feature'

# [10] put the board back
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
say "=== D31 ANIM BENCH $verdict ==="

