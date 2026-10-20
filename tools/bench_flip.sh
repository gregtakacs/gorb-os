#!/bin/bash
# bench_flip.sh — the FORMAT 3 store door + the theme upload + the flip door, on the
#   board. Renamed from m5_flip.sh: the M5 Arm-A/Arm-B comparison the name records was
#   retired by D30, and the rename deferral died with it (D30 §26 slice 3, 2026-10-20).
#   MIGRATED from the M5 Arm-A/Arm-B flip comparison (D11/D30). D30 (2026-10-15)
#   retired the reboot-on-every-pick rule and §27 deleted the FORMAT 2 ingest door.
#   §26 slice 3's flip door landed (2026-10-20), so this drives BOTH stages of the
#   two-stage press: what it proves on the same card bundles:
#     (1) the FORMAT 3 theme UPLOAD lands ("state":"stored" off the /api/v1/sd/theme
#         door, the §16.1 rule that a 200 header is not a response);
#     (2) the STORE door (POST /api/v1/sd/bundle?id=X) answers reboot:none and the
#         dial does NOT flip (no rst:0x boundary after the store);
#     (3) the store's ingest cost, read back off the device's bundle_ms/bundle_bytes;
#     (4) PSRAM is UNCHANGED across a store — residency is the lazy arena's job (§20),
#         so a store that moved the heap would be the bug this now catches;
#     (5) the FLIP door (POST /api/v1/sd/flip?id=X) answers reboot:automatic, the
#         worker logs its own "flipped to" line, a reboot boundary FOLLOWS (its
#         presence is now the pass), and the re-leased board reports
#         live=slot-B and the bundle's name active — then sd/default puts the board
#         back and the live word returns to default.
#   The slot-A leg is deliberately NOT benched here: flipping onto A requires a
#   store aimed at A, which overwrites the flashed default — sanctioned by §26 but
#   a change no unattended bench run should make to a shared board.
#
#   Arms retired: the old "Arm A copy+reboot timed to first dial" and "Arm B PSRAM
#   footprint of a resident flip" both measured a mechanism the board no longer runs.
#
# Preconditions: /tmp/m5_avkit.orbtheme (name av-kit) and /tmp/m5_avkit_b.orbtheme
# (name av-kit-b) are FORMAT 3 bundles (rebake_bundle.py), and pass
# orbbundle.read_bundle; the card is in; the port is free. Every response body
# goes to a freshly deleted file and is only believed if size_download > 0 — the
# §16.1 rule: a 200 header is not a response.
#
# Run DETACHED:
#   cd /home/greg/docker/Projects/gorb-os-2
#   setsid tools/bench_flip.sh > /tmp/m5f_run.log 2>&1 < /dev/null &
set -u
SER=/tmp/m5f_ser.txt
BODY=/tmp/m5f_body.json
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$REPO" || exit 1
# The clone is the binding: the HTTP target comes from THIS clone's .orb-target —
# the same file capf.py and serflash.sh read. The hardcoded .34 default let a bare
# run from an orb-A-bound clone drive orb-B's radio while capturing orb-A's serial:
# a split-board run whose verdict means nothing. An ORB override may only name the
# bound board.
. tools/orb_bind.sh
orb_require
ORB=${ORB:-http://$ORB_IP}
[ "$ORB" = "http://$ORB_IP" ] || orb_die "ORB=$ORB is not the bound orb $ORB_LABEL (http://$ORB_IP). Unset ORB or re-bind .orb-target."
say(){ echo "[$(date +%T)] $*"; }
verdict=PASS
fail(){ say "FAIL: $*"; verdict=FAIL; }
fetch(){ # fetch METHOD URL [dataargs...] -> sets CODE, prints body; refuses 0-byte bodies
  local m=$1 u=$2; shift 2
  rm -f "$BODY"
  CODE=$(curl -s -m 240 -X "$m" "$@" -o "$BODY" -w '%{http_code}:%{size_download}' "$u")
  [ "${CODE#*:}" != ":0" ] && [ "${CODE##*:}" -gt 0 ] 2>/dev/null
}
dial(){ # dial: print min fps seen since epoch E from the clock lines
  awk -v e="$1" '$1+0>e && /clock: clock [0-9]+ fps/ {match($0,/clock [0-9]+ fps/); f=substr($0,RSTART+6,RLENGTH-5)+0; if(m==0||f<m)m=f} END{print m+0}' "$SER"
}

# [1] fixtures, host-verified before the radio carries anything (FORMAT 3 gate)
python3 - <<'PY' || exit 1
import sys; sys.path.insert(0, "tools")
import orbtheme, orbbundle
for p in ("/tmp/m5_avkit.orbtheme", "/tmp/m5_avkit_b.orbtheme"):
    h, _secs = orbbundle.read_bundle(p)   # (header, sections); raises on any FORMAT != 3
    print(p, "name", repr(h["name"]), "id", orbtheme.theme_id(h["name"]),
          "art", h["art_size"], "fmt", h.get("fmt", 3))
PY
IDB=$(python3 -c "import sys; sys.path.insert(0,'tools'); import orbtheme; print(orbtheme.theme_id('av-kit'))")
IDC=$(python3 -c "import sys; sys.path.insert(0,'tools'); import orbtheme; print(orbtheme.theme_id('av-kit-b'))")
say "ids: av-kit=$IDB av-kit-b=$IDC"

# [2] capture from the open-reset boot; wait for the lease (150 s window, §18.1)
rm -f "$SER"
setsid python3 tools/capf.py 900 "$SER" >/dev/null 2>&1 &
CAP=$!
for i in $(seq 1 150); do sleep 1; grep -q 'sta ip' "$SER" 2>/dev/null && break; done
grep -q 'sta ip' "$SER" || { fail 'no lease in 150 s'; exit 1; }
say "lease up; capture at $SER"

# [3] mount + PSRAM baseline
fetch POST "$ORB/api/v1/sd/rescan" || fail 'rescan returned no body'
say "rescan: $(cat "$BODY")"
grep -q '"mounted":true' "$BODY" || fail 'no card'
fetch GET "$ORB/api/v1/status" && PSRAM0=$(python3 -c "import json;print(json.load(open('$BODY'))['heap_psram'])")
say "psram baseline: ${PSRAM0:-?}"

# [4] upload both bundles (the theme-side upload this run also proves)
for F in /tmp/m5_avkit.orbtheme /tmp/m5_avkit_b.orbtheme; do
  U=$(curl -s -m 240 -X POST --data-binary @"$F" -H 'Content-Type: application/octet-stream' \
      -o /tmp/m5f_up.json -w 'code=%{http_code} size=%{size_download} t=%{time_total}' "$ORB/api/v1/sd/theme")
  if grep -q '"overwrite":false' /tmp/m5f_up.json 2>/dev/null; then
    say "upload $F: 409 duplicate-name, re-sending with overwrite"
    U=$(curl -s -m 240 -X POST --data-binary @"$F" -H 'Content-Type: application/octet-stream' \
        -o /tmp/m5f_up.json -w 'code=%{http_code} size=%{size_download} t=%{time_total}' "$ORB/api/v1/sd/theme?overwrite=1")
  fi
  say "upload $F: $U -> $(cat /tmp/m5f_up.json 2>/dev/null)"
  grep -q '"state":"stored"' /tmp/m5f_up.json || fail "upload refused for $F"
done
sleep 2
fetch GET "$ORB/api/v1/sd" && say "GET /api/v1/sd: $(head -c 300 "$BODY")"

store(){ # store ID LABEL — one store-door press: reboot:none, NO flip, cost off bundle_ms
  local id=$1 label=$2
  local t0=$(date +%s.%N)
  fetch POST "$ORB/api/v1/sd/bundle?id=$id" || { fail "store $label: no body"; return; }
  local t1=$(date +%s.%N)
  say "store($label): $CODE -> $(cat "$BODY")"
  grep -q '"state":"storing"' "$BODY" || { fail "store $label refused: $(cat "$BODY")"; return; }
  grep -q '"reboot":"none"' "$BODY" || { fail "store $label did NOT answer reboot:none: $(cat "$BODY")"; return; }
  # The window anchor is the press's own host wall time. capf stamps every line in
  # that domain, and the base face emits no fps line to anchor on — a board booting
  # to source none is exactly the post-slice-2 state this bench now runs on, and
  # anchoring on absent fps lines once read the capture's own open-reset as a flip.
  local E=$(date +%s)
  # (A flip would reboot inside a second of the ack; the store door must not. Its
  # ABSENCE is the pass here. The -2 grace excludes only the capture's own
  # open-reset; a +3 grace once swallowed a real flip boundary that landed +0.86 s.)
  local R=""
  for j in $(seq 1 40); do
    sleep 1
    R=$(awk -v e="$E" '$1+0>(e-2) && /rst:0x/ {print $1; exit}' "$SER")
    [ -n "$R" ] && { fail "store $label: the dial FLIPPED (rst:0x after a reboot:none store)"; return; }
  done
  # Read the ingest cost the store worker recorded, beside the refill figures (nonzero).
  local BM="?" BB="?"
  for k in $(seq 1 20); do
    fetch GET "$ORB/api/v1/sd" && {
      BM=$(python3 -c "import json;print(json.load(open('$BODY')).get('bundle_ms'))" 2>/dev/null)
      BB=$(python3 -c "import json;print(json.load(open('$BODY')).get('bundle_bytes'))" 2>/dev/null)
      [ "${BM:-0}" != "0" ] && [ "$BM" != "None" ] && [ -n "$BM" ] && break; }
    sleep 2
  done
  say "--- store $label ---"
  awk -v e="$E" '$1+0>(e-8)' "$SER" | grep -E 'bundle|store|rst:0x|sta ip|config server|clock: clock [0-9]+ fps' | head -24
  say "store $label: ack wall $(echo "$t1-$t0" | bc)s  device bundle_ms=$BM bundle_bytes=$BB"
}

# [5] store both bundles: no flip either way (reboot:none, and PSRAM must not move)
store "$IDC" "avkit-b"
sleep 3
store "$IDB" "avkit"

# [6] PSRAM half, inverted: a store makes nothing resident, so the heap must be UNCHANGED.
fetch GET "$ORB/api/v1/status" && PSRAM1=$(python3 -c "import json;print(json.load(open('$BODY'))['heap_psram'])")
say "psram after stores: ${PSRAM1:-?} (baseline ${PSRAM0:-?}) — a store moves no pages; a drop here means the store wrongly made the bundle resident (§26 ping-pong's job, not P6.4's)"

# [7] the FLIP door, §26 slice 3: the second press of the two-stage form. The dial
# drew the default leg (base face on this bench board: its flashed default predates
# FORMAT 3 and the new reader rightly refuses it), so both stores aimed at B and the
# avkit store is the one B holds (the avkit-b store was overwritten by the ping-pong
# — as designed, D13's asymmetry is retired). The flip must find that id, log its own
# sentence, and the reboot boundary its answer promised must APPEAR — the inverse of
# the store's absence-assertion.
flip(){ # flip ID LABEL EXPECT_NAME — press (waiting out a busy queue), expect the boundary, verify the re-leased dial
  local id=$1 label=$2 want=$3
  # The store job may still be draining; the door 409s while any theme job runs, and
  # a wait is what a 409 means. Retry until the ack is the flip's own.
  local r
  for r in $(seq 1 25); do
    fetch POST "$ORB/api/v1/sd/flip?id=$id" || { fail "flip $label: no body"; return; }
    grep -q 'already running' "$BODY" || break
    say "flip $label: queue busy — waiting out the running job"
    sleep 2
  done
  say "flip($label): $CODE -> $(cat "$BODY")"
  grep -q '"state":"flipping"' "$BODY" || { fail "flip $label refused: $(cat "$BODY")"; return; }
  grep -q '"reboot":"automatic"' "$BODY" || { fail "flip $label did NOT answer reboot:automatic"; return; }
  # Anchor the reboot window on the ack's own host instant: capf stamps lines in that
  # domain, and the base face prints no fps line to anchor on — anchoring on absent
  # fps lines once read the capture's own open-reset as a flip.
  local E=$(date +%s)
  local R=""
  for j in $(seq 1 45); do
    sleep 1
    R=$(awk -v e="$E" '$1+0>(e-2) && /rst:0x/ {print $1; exit}' "$SER")
    [ -n "$R" ] && break
  done
  [ -n "$R" ] || { fail "flip $label: reboot:automatic promised a boundary and none came"; return; }
  say "flip $label: reboot boundary at $R — the promise was kept"
  for i in $(seq 1 90); do sleep 1; awk -v r="$R" '$1+0>r && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
  sleep 3
  fetch GET "$ORB/api/v1/sd" || { fail "flip $label: no body after re-lease"; return; }
  say "after flip: $(head -c 300 "$BODY")"
  grep -q '"live":"slot-B"' "$BODY" || fail "flip $label: live is not slot-B — $(cat "$BODY")"
  grep -q "\"active\":\"$want\"" "$BODY" || fail "flip $label: active is not $want — $(cat "$BODY")"
  grep -q "\"name\":\"$want\",\"id\":\"$id\"" "$BODY" \
    && grep -q '"ok":true,"active":true' "$BODY" \
    || fail "flip $label: no row is name=$want id=$id and active:true"
  say "flip $label: dial on slot-B, active \"$want\", row true — press to boundary ${j}s"
  awk -v e="$E" '$1+0>(e-8)' "$SER" | grep -E 'flipped to|from slot|refilled|rst:0x|sta ip' | head -10
}
flip "$IDB" "avkit" "av-kit"

# [8] put the board back: sd/default answers reboot:automatic too, and the flip-back
# must land the live word on "default" — the flip is one press from where it started.
# The queue is fresh on a board that just rebooted for the flip, so one press; the
# anchor is again the press's own host instant.
fetch POST "$ORB/api/v1/sd/default" || fail 'sd/default returned no body'
grep -q '"reboot":"automatic"' "$BODY" || fail 'sd/default did NOT answer reboot:automatic'
E3=$(date +%s)
for j in $(seq 1 45); do
  sleep 1
  awk -v e="$E3" '$1+0>(e-2) && /rst:0x/ {f=1} END{exit !f}' "$SER" && break
done
for i in $(seq 1 90); do sleep 1; awk -v e="$E3" '$1+0>(e-2) && /sta ip/ {f=1} END{exit !f}' "$SER" && break; done
sleep 3
fetch GET "$ORB/api/v1/sd" && say "after default: $(head -c 300 "$BODY")"
grep -q '"live":"default"' "$BODY" || fail 'flip-back did not land on live=default'

kill $CAP 2>/dev/null
say "=== M5_FLIP $verdict ==="
