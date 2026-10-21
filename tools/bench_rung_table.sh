#!/bin/bash
# bench_rung_table.sh — P8.1's one detached rung sweep (§8's P8 row, §31's open item).
#
# Measures DELIVERED fps across the plate-bank ladder at the dial's own pose, and
# states the requested-vs-delivered gap per rung. Delivered is read off the serial
# `clock N fps` line (flick_bench.py's own regex); requested is the boot `plate bank
# : N keyframes @ F fps` line. Nothing is inferred from a 200 or a size.
#
# Rungs the generator refuses (N=1 still, N=8/16 over D29's cap) are recorded as
# refusals with the byte it tripped on — the refusal is the finding (§32's idiom),
# never a faked measurement. The still (N=1) is benched off the on-card av-kit
# still plate; the animated rungs are uploaded fresh at bg_fps 16 to expose the gap.
#
# Run DETACHED (the sweep is minutes, and a -75 upload can abort):
#   setsid tools/bench_rung_table.sh </dev/null >/tmp/rung_sweep.log 2>&1 &
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
SER=/tmp/rung_ser.txt
BODY=/tmp/rung_body.json
FIXDIR=/tmp/rung_fix
mkdir -p "$FIXDIR"
cd "$REPO" || { echo "EXIT=1"; exit 1; }
. tools/orb_bind.sh
orb_require
ORB=http://$ORB_IP
[ "$ORB" = "http://$ORB_IP" ] || { echo "refusing: bound orb mismatch"; echo EXIT=1; exit 1; }
say(){ echo "[$(date +%T)] $*"; }
fetch(){ local m=$1 u=$2; shift 2
  rm -f "$BODY"
  CODE=$(curl -s -m 300 -X "$m" "$@" -o "$BODY" -w '%{http_code}:%{size_download}' "$u")
  say "  $m ${u##*/} -> $CODE"
  [ "${CODE##*:}" -gt 0 ] 2>/dev/null
}
dialmin(){ awk -v e="$1" '$1+0>(e+3) && /clock: clock [0-9]+ fps/ {match($0,/clock [0-9]+ fps/); f=substr($0,RSTART+6,RLENGTH-5)+0; if(m==0||f<m)m=f} END{print m+0}' "$SER"; }
dialmed(){ awk -v e="$1" '$1+0>(e+3) && /clock: clock [0-9]+ fps/ {match($0,/clock [0-9]+ fps/); a[++n]=substr($0,RSTART+6,RLENGTH-5)+0} END{asort(a); print n%2 ? a[(n+1)/2] : (n ? (a[n/2]+a[n/2+1])/2 : 0)}' "$SER"; }
dialmax(){ awk -v e="$1" '$1+0>(e+3) && /clock: clock [0-9]+ fps/ {match($0,/clock [0-9]+ fps/); f=substr($0,RSTART+6,RLENGTH-5)+0; if(f>x)x=f} END{print x+0}' "$SER"; }
motion(){ # two grabs a bank stride apart: did ink move at all
  python3 tools/fbgrab.py --out /tmp/rung_g1.bmp --quiet >/dev/null 2>&1 || { echo "?"; return; }
  sleep 0.16
  python3 tools/fbgrab.py --out /tmp/rung_g2.bmp --quiet >/dev/null 2>&1 || { echo "?"; return; }
  python3 tools/fbgrab.py --diff /tmp/rung_g1.bmp /tmp/rung_g2.bmp 2>&1 \
    | sed -n 's/.*: \([0-9.]*\)% pixels changed.*/\1/p'
}

# ---- the refusals, computed before any board is touched ----------------------
say "=== rung ladder: asks and refusals (host arithmetic, §32 idiom) ==="
python3 tools/bench_rung_table.py --planes 1 --fps 16 --out "$FIXDIR/rung_1.orbtheme" 2>&1 | tee -a /tmp/rung_ask.txt
python3 tools/bench_rung_table.py --planes 8 --fps 16 --out "$FIXDIR/rung_8.orbtheme" 2>&1 | tee -a /tmp/rung_ask.txt
python3 tools/bench_rung_table.py --planes 16 --fps 16 --out "$FIXDIR/rung_16.orbtheme" 2>&1 | tee -a /tmp/rung_ask.txt

# ---- build the reachable animated rungs at bg_fps 16 (exposes the gap) --------
for N in 2 4; do
  say "building rung N=$N"
  python3 tools/bench_rung_table.py --planes "$N" --fps 16 --out "$FIXDIR/rung_$N.orbtheme" || { say "FAIL build N=$N"; continue; }
done

# ---- one capture for the whole sweep; the flip re-lights the dial -------------
rm -f "$SER"
setsid python3 tools/capf.py 1200 "$SER" --no-reset >/dev/null 2>&1 &
CAP=$!
wait_lease(){ for i in $(seq 1 60); do sleep 1; grep -q 'sta ip' "$SER" 2>/dev/null && return 0; done; return 1; }

measure_one(){ # $1=label $2=id  (assumes bundle already stored on a slot)
  local label="$1" ID="$2"
  local E=$(date +%s); local R=""
  fetch POST "$ORB/api/v1/sd/flip?id=$ID" || true
  if grep -q 'already running' "$BODY"; then
    for r in $(seq 1 25); do sleep 2
      fetch POST "$ORB/api/v1/sd/flip?id=$ID" || true
      grep -q 'already running' "$BODY" || break
    done
  fi
  for j in $(seq 1 45); do sleep 1
    R=$(awk -v e="$E" '$1+0>(e-2) && /rst:0x/ {print $1; exit}' "$SER"); [ -n "$R" ] && break
  done
  [ -n "$R" ] || { say "$label: NO BOUNDARY — flip did not land"; return 1; }
  for i in $(seq 1 90); do sleep 1; awk -v r="$R" '$1+0>r && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
  sleep 3
  fetch GET "$ORB/api/v1/screen?to=clock" || true
  say "$label: sampling delivered fps for 35 s"
  sleep 35
  local BLINE=$(echo "$SER" | awk -v r="$R" '$1+0>r' | grep 'plate bank' | tail -1)
  local MIND=$(dialmin "$R") MEDD=$(dialmed "$R") MAXD=$(dialmax "$R")
  local DIFF=$(motion)
  say "$label: boot-line[$(echo "$BLINE" | sed 's/.*plate bank/plate bank/')] delivered min $MIND med $MEDD max $MAXD glass-diff ${DIFF:-?}%"
  echo "RESULT $label min=$MIND med=$MEDD max=$MAXD diff=${DIFF:-?} boot='$BLINE'" >> /tmp/rung_results.txt
}

: > /tmp/rung_results.txt
if wait_lease; then
  say "lease up; capture live"
  # ---- N=1 STILL: the on-card av-kit still plate (bg_frames 0) ----------------
  say "=== rung N=1 (still, on-card av-kit id c38cd33b1c35) ==="
  fetch POST "$ORB/api/v1/sd/rescan" >/dev/null
  measure_one "N=1-still" c38cd33b1c35
  # ---- N=2 and N=4 animated @ bg_fps 16: upload, store, flip, measure ---------
  for N in 2 4; do
    say "=== rung N=$N @ bg_fps 16 (upload over -75) ==="
    FIX="$FIXDIR/rung_$N.orbtheme"
    ID=$(python3 -c "import sys; sys.path.insert(0,'$REPO/tools'); import orbtheme; print(orbtheme.theme_id('rung-%02d' % $N))")
    fetch POST "$ORB/api/v1/sd/theme?overwrite=1" --data-binary @"$FIX" -H 'Content-Type: application/octet-stream' || { say "N=$N upload no body — radio abort at -75?"; continue; }
    if ! grep -q '"state":"stored"' "$BODY"; then say "N=$N upload not stored: $(head -c 120 "$BODY")"; continue; fi
    sleep 2
    fetch POST "$ORB/api/v1/sd/bundle?id=$ID" || { say "N=$N store no body"; continue; }
    measure_one "N=$N-fps16" "$ID"
  done
  # put the board back
  fetch POST "$ORB/api/v1/sd/default" >/dev/null; sleep 8
else
  say "NO LEASE in 60 s — device unreachable, recording refusal"
fi
kill $CAP 2>/dev/null
say "=== RUNG SWEEP COMPLETE ==="
echo "EXIT=$?"

