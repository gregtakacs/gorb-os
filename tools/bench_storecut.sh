#!/bin/bash
# bench_storecut.sh — the power cut inside the FORMAT 3 STORE's commit window, the
#   §20.5(3) gate §26's ping-pong owes. Cut with the board's own reboot door — the
#   bench_powercut idiom, D9's blunt instrument: h_reboot arms a fixed 750 ms one-shot
#   and does NOT drain orb_jobs, so a tear lands exactly where a pulled plug would.
#
#   Why a new script instead of a flag on bench_powercut.sh: that bench cuts the
#   APP-install window and its pass line is D9's "torn app is absent". The theme
#   store's window has different witnesses — the `staged` word, the surviving old
#   header, and a flip that must fail safe at boot's adopt — so it gets its own file.
#
#   The window, from the code: orb_theme_install_bundle brackets the copy with
#   staged_mark() and writes the slot HEADER LAST (orb_theme.c: staged_mark ->
#   copy_bundle3 erase+write+readback -> header-sector erase -> build_slot_hdr3 ->
#   staged_clear). This bench stores av-kit-b into slot B while B still holds a
#   cleanly-committed av-kit, so a cut before the last write leaves the av-kit header
#   standing over erased art — the state a real cut leaves and the one that must not
#   theme the dial.
#
# Pass line (§20.5(3)):
#   (1) the cut lands inside the window: rst:0x after the store press, no
#       "stored, and not yet chosen" line for it, and the ledger's staged_id naming
#       the never-committed id;
#   (2) the board boots alive and rolls back: lease back, live=default, source=slot-A,
#       clock fps lines after the cut — the ruled first-boot face, unharmed;
#   (3) the `staged` word speaks: staged_why non-empty on the row;
#   (4) the torn copy cannot theme the glass: the flip door accepts the surviving
#       id (it reads headers only — by design), boot's adopt verify refuses the art,
#       damage_why names re-store, and the default is drawn. Failing safe is the gate.
#   Recovery closes the loop: re-store, flip, sd/default — the board ends exactly
#   where it started, because the next bench run inherits it.
#
# Run DETACHED:
#   cd /home/greg/docker/Projects/gorb-os-2
#   setsid tools/bench_storecut.sh > /tmp/m5sc_run.log 2>&1 < /dev/null &
set -u
SER=/tmp/m5sc_ser.txt
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$REPO" || exit 1
# The clone is the binding: the HTTP target comes from THIS clone's .orb-target —
# the same file capf.py and serflash.sh read; an ORB override may only name the
# bound board (split-board verdicts mean nothing).
. tools/orb_bind.sh
orb_require
ORB=${ORB:-http://$ORB_IP}
[ "$ORB" = "http://$ORB_IP" ] || orb_die "ORB=$ORB is not the bound orb $ORB_LABEL (http://$ORB_IP). Unset ORB or re-bind .orb-target."
say(){ echo "[$(date +%T)] $*"; }
verdict=PASS
fail(){ say "FAIL: $*"; verdict=FAIL; }
IDB=$(python3 -c "import sys; sys.path.insert(0,'tools'); import orbtheme; print(orbtheme.theme_id('av-kit'))")
IDC=$(python3 -c "import sys; sys.path.insert(0,'tools'); import orbtheme; print(orbtheme.theme_id('av-kit-b'))")
say "ids: committed=$IDB (av-kit)  torn-under-test=$IDC (av-kit-b)"

# [0] fixtures: the same two FORMAT 3 bundles bench_flip uses.
for F in /tmp/m5_avkit.orbtheme /tmp/m5_avkit_b.orbtheme; do
  [ -f "$F" ] || { say "REFUSE: no $F (rebake_bundle.py it first)"; exit 1; }
done
rm -f "$SER"
setsid python3 tools/capf.py 2400 "$SER" >/dev/null 2>&1 &
CAP=$!
for i in $(seq 1 150); do sleep 1; grep -q 'sta ip' "$SER" 2>/dev/null && break; done
grep -q 'sta ip' "$SER" || { say 'REFUSE: no lease in 150 s'; kill $CAP 2>/dev/null; exit 1; }
say "lease up"

# [1] baseline. The ledger must start honest: live default, and slot B holding a
# cleanly-committed av-kit — the header the torn store must fail to replace. If B
# does not advertise an ok av-kit row, prime one clean.
curl -s -m 20 -X POST "$ORB/api/v1/sd/rescan" >/dev/null
BASE=$(curl -s -m 15 "$ORB/api/v1/sd")
echo "$BASE" | grep -q '"live":"default"' || { kill $CAP 2>/dev/null; say "REFUSE: baseline live is not default — $(echo "$BASE" | head -c 120)"; exit 1; }
LIVE_ID=$IDB
if ! echo "$BASE" | grep -qE '"name":"av-kit"[^}]*"ok":true'; then
  say "slot B holds no clean av-kit — priming one (the tear needs a committed header to spare)"
  curl -s -m 60 -X POST --data-binary @/tmp/m5_avkit.orbtheme \
    -H 'Content-Type: application/octet-stream' -o /dev/null "$ORB/api/v1/sd/theme?overwrite=1"
  curl -s -m 30 -X POST "$ORB/api/v1/sd/bundle?id=$IDB" >/dev/null
  for k in $(seq 1 20); do
    sleep 3
    BM=$(curl -s -m 15 "$ORB/api/v1/sd" | python3 -c "import json,sys;print(json.load(sys.stdin).get('bundle_ms',0))" 2>/dev/null)
    [ "${BM:-0}" != "0" ] && break
  done
fi

# [2] trial loop: press the store, light the fuse 50 ms later. The store ack returns
# at ≈30 ms and the copy runs ≈4.5 s (bench_flip measured bundle_ms 4587/4787), so
# rst lands at ≈T0+0.8 s — inside the widest headerless span the store offers. A cut
# before the worker starts (no staged word) is a miss like any other; retry, 4 tries.
# A COMPLETED trial replaces the survivor: the header then names av-kit-b, and the
# next tear must fail safe against THAT id — LIVE_ID tracks it.
TORN=0
for try in 1 2 3 4; do
  E=$(date +%s)
  curl -s -m 10 -X POST "$ORB/api/v1/sd/bundle?id=$IDC" -o /tmp/m5sc_store.json
  say "trial $try: store press -> $(cat /tmp/m5sc_store.json)"
  grep -q '"state":"storing"' /tmp/m5sc_store.json || { say 'retry: door refused (job busy)'; sleep 6; continue; }
  curl -s -m 10 -X POST "$ORB/api/v1/reboot" >/dev/null
  say "trial $try: fuse armed"
  sleep 30
  for i in $(seq 1 60); do sleep 2; curl -s -m 3 "$ORB/api/v1/status" -o /dev/null && break; done
  sleep 2
  SC=$(curl -s -m 15 "$ORB/api/v1/sd")
  DONE=$(awk -v e="$E" '$1+0>e && /stored, and not yet chosen/ {f=1} END{print f+0}' "$SER")
  RSTN=$(awk -v e="$E" '$1+0>(e-2) && /rst:0x/ {c++} END{print c+0}' "$SER")
  STG=$(echo "$SC" | python3 -c "import json,sys;print(json.load(sys.stdin).get('staged_id',''))" 2>/dev/null)
  say "trial $try: stored-line=$DONE rst=$RSTN staged=${STG:-none}"
  if [ "$DONE" = "0" ] && [ "$RSTN" -ge 1 ] && [ "$STG" = "$IDC" ]; then TORN=1; break; fi
  [ "$DONE" = "1" ] && LIVE_ID=$IDC
  sleep 4
done
[ "$TORN" = 1 ] || fail 'no trial landed the cut inside the store window'

# [3] the rollback witnesses. A torn store must have changed nothing a dial reads.
if [ "$TORN" = 1 ]; then
  STW=$(echo "$SC" | python3 -c "import json,sys;print(json.load(sys.stdin).get('staged_why',''))" 2>/dev/null)
  [ -n "$STW" ] || fail 'staged names the torn store but staged_why is empty — the witness must speak'
  echo "$SC" | grep -q '"live":"default"' || fail 'the torn store moved the live word — a store must never choose itself'
  echo "$SC" | grep -q '"source":"slot-A"' || fail 'the board is not drawing slot-A after the cut'
  FPS=$(awk -v e="$E" '$1+0>e && /clock: clock [0-9]+ fps/' "$SER" | tail -1)
  [ -n "$FPS" ] || fail 'no clock fps line after the cut — the dial is not alive'
  say "witness: staged=$STG (\"$STW\") live=default source=slot-A — rollback held"
  say "dial after the cut: ${FPS:0:90}"
fi

# [4] the torn copy cannot theme the glass. The surviving header still parses and
# names its theme, so the flip door takes the press (headers only — by design);
# boot's adopt must then refuse the art the torn store erased and draw the default
# with a damage sentence on the row. A refusal that speaks is the PASS.
if [ "$TORN" = 1 ]; then
  for r in $(seq 1 25); do
    curl -s -m 20 -X POST "$ORB/api/v1/sd/flip?id=$LIVE_ID" -o /tmp/m5sc_flip.json
    grep -q 'already running' /tmp/m5sc_flip.json || break
    sleep 2
  done
  say "flip onto the torn copy (survivor $LIVE_ID): $(cat /tmp/m5sc_flip.json)"
  grep -q '"reboot":"automatic"' /tmp/m5sc_flip.json || fail 'the flip door refused an id the surviving header advertised'
  E2=$(date +%s)
  for j in $(seq 1 45); do sleep 1; awk -v e="$E2" '$1+0>(e-2) && /rst:0x/ {f=1} END{exit !f}' "$SER" && break; done
  for i in $(seq 1 90); do sleep 1; awk -v e="$E2" '$1+0>(e-2) && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
  sleep 3
  SC2=$(curl -s -m 15 "$ORB/api/v1/sd")
  DW=$(echo "$SC2" | python3 -c "import json,sys;print(json.load(sys.stdin).get('damage_why',''))" 2>/dev/null)
  SRC=$(echo "$SC2" | python3 -c "import json,sys;print(json.load(sys.stdin).get('source',''))" 2>/dev/null)
  say "after the flip: source=${SRC:-?} damage_why=\"$DW\""
  echo "$DW" | grep -qiE 'verify|re-store|no clock section' || fail 'the torn copy reached the glass without a damage sentence'
  [ "${SRC:-}" = "slot-A" ] || fail 'the refused flip left the board off the default leg'
  awk -v e="$E2" '$1+0>e' "$SER" | grep -E 'would not load|did not verify' | tail -2
fi

# [5] recovery: re-store av-kit clean, flip onto it, sd/default back. The board must
# end where it started, because the next bench run inherits it.
curl -s -m 60 -X POST --data-binary @/tmp/m5_avkit.orbtheme \
  -H 'Content-Type: application/octet-stream' -o /dev/null "$ORB/api/v1/sd/theme?overwrite=1"
for r in $(seq 1 25); do
  curl -s -m 20 -X POST "$ORB/api/v1/sd/bundle?id=$IDB" -o /tmp/m5sc_st2.json
  grep -q 'already running' /tmp/m5sc_st2.json || break
  sleep 2
done
grep -q '"state":"storing"' /tmp/m5sc_st2.json || fail 'recovery re-store refused'
for k in $(seq 1 30); do
  sleep 3
  SC3=$(curl -s -m 15 "$ORB/api/v1/sd")
  BM=$(echo "$SC3" | python3 -c "import json,sys;print(json.load(sys.stdin).get('bundle_ms',0))" 2>/dev/null)
  STG3=$(echo "$SC3" | python3 -c "import json,sys;print(json.load(sys.stdin).get('staged_id',''))" 2>/dev/null)
  [ "${BM:-0}" != "0" ] && [ "$BM" != "None" ] && [ -z "$STG3" ] && break
done
{ [ "${BM:-0}" != "0" ] && [ -z "$STG3" ]; } || fail "recovery store did not land clean (bundle_ms=$BM staged='${STG3-unset}')"
say "recovery store landed (bundle_ms=$BM, staged cleared)"
R4=$(curl -s -m 20 -X POST "$ORB/api/v1/sd/flip?id=$IDB")
echo "$R4" | grep -q '"state":"flipping"' || fail "the healthy re-stored copy refused a flip: $R4"
E4=$(date +%s)
for j in $(seq 1 45); do sleep 1; awk -v e="$E4" '$1+0>(e-2) && /rst:0x/ {f=1} END{exit !f}' "$SER" && break; done
for i in $(seq 1 90); do sleep 1; awk -v e="$E4" '$1+0>(e-2) && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
sleep 3
SC4=$(curl -s -m 15 "$ORB/api/v1/sd")
echo "$SC4" | grep -q '"live":"slot-B"' || fail 'recovery flip did not land on slot-B'
echo "$SC4" | grep -q '"source":"slot-B"' || fail 'the re-stored copy did not theme the dial'
say "the re-stored copy themes the dial: $(echo "$SC4" | head -c 140)…"
curl -s -m 15 -X POST "$ORB/api/v1/sd/default" >/dev/null
E5=$(date +%s)
# strictly-after-press stamps only: the flip's boot lines sit seconds behind this
# press, and a (e-2) margin would let the waits break on them (this race produced
# a false FAIL on the first run of this bench, 2026-10-19).
for j in $(seq 1 45); do sleep 1; awk -v e="$E5" '$1+0>e && /rst:0x/ {f=1} END{exit !f}' "$SER" && break; done
for i in $(seq 1 90); do sleep 1; awk -v e="$E5" '$1+0>e && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
sleep 3
for g in 1 2 3; do
  curl -s -m 15 "$ORB/api/v1/sd" | grep -q '"live":"default"' && break
  sleep 5
done
curl -s -m 15 "$ORB/api/v1/sd" | grep -q '"live":"default"' || fail 'sd/default did not restore the board'
say "board restored to live=default"

kill $CAP 2>/dev/null
say "=== BENCH_STORECUT $verdict ==="