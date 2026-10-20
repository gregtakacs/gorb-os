#!/bin/bash
# m5_flip.sh — the FORMAT 3 store door + the theme upload, on the board.
#   MIGRATED from the M5 Arm-A/Arm-B flip comparison (D11/D30). D30 (2026-10-15)
#   retired the reboot-on-every-pick rule and §27 deleted the FORMAT 2 ingest door,
#   so this script no longer times a flip — the store door answers reboot:none and
#   swaps nothing onto the glass until §26's ping-pong compositor lands. What it
#   still proves, on the same card bundles:
#     (1) the FORMAT 3 theme UPLOAD lands ("state":"stored" off the /api/v1/sd/theme
#         door, the §16.1 rule that a 200 header is not a response);
#     (2) the STORE door (POST /api/v1/sd/bundle?id=X) answers reboot:none and the
#         dial does NOT flip (no rst:0x boundary after the store);
#     (3) the store's ingest cost, read back off the device's bundle_ms/bundle_bytes;
#     (4) PSRAM is UNCHANGED across a store — residency is the ping-pong slice's job,
#         so a store that moved the heap would be the bug this now catches.
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
#   setsid tools/m5_flip.sh > /tmp/m5f_run.log 2>&1 < /dev/null &
set -u
ORB=${ORB:-http://192.168.2.34}
REPO=/home/greg/docker/Projects/gorb-os-2
SER=/tmp/m5f_ser.txt
BODY=/tmp/m5f_body.json
cd "$REPO" || exit 1
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
  local E=$(grep -E 'clock: clock [0-9]+ fps' "$SER" | tail -1 | cut -d' ' -f1)
  # The dial must NOT flip: assert NO new rst:0x boundary after the store press.
  # (A flip would reboot; the store door must not. Its ABSENCE is the pass here.)
  local R=""
  for j in $(seq 1 40); do
    sleep 1
    R=$(awk -v e="$E" '$1+0>e+3 && /rst:0x/ {print $1; exit}' "$SER")
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
  awk -v e="$E" '$1+0>e-8' "$SER" | grep -E 'bundle|store|rst:0x|sta ip|config server|clock: clock [0-9]+ fps' | head -24
  say "store $label: ack wall $(echo "$t1-$t0" | bc)s  device bundle_ms=$BM bundle_bytes=$BB"
}

# [5] store both bundles: no flip either way (reboot:none, and PSRAM must not move)
store "$IDC" "avkit-b"
sleep 3
store "$IDB" "avkit"

# [6] PSRAM half, inverted: a store makes nothing resident, so the heap must be UNCHANGED.
fetch GET "$ORB/api/v1/status" && PSRAM1=$(python3 -c "import json;print(json.load(open('$BODY'))['heap_psram'])")
say "psram after stores: ${PSRAM1:-?} (baseline ${PSRAM0:-?}) — a store moves no pages; a drop here means the store wrongly made the bundle resident (§26 ping-pong's job, not P6.4's)"

kill $CAP 2>/dev/null
say "=== M5_FLIP $verdict ==="
