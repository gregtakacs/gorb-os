#!/bin/bash
# m5_flip.sh — M5 Arm A measured at the board; Arm B's halves priced on existing
# doors (plan §16 checklist steps 1-4, §6 M5). D30's ruling stays the owner's:
# this script pays its two timings and states no verdict.
#
# Arms, same bundles on the card:
#   Arm A (today's rule, D11): POST /api/v1/sd/install?id=X copies card->inactive
#     slot, verifies from flash, marks live, reboots. Timed request -> first dial
#     line after the reboot boundary, plus association-to-page-answers after.
#   Arm B (lazy, unshipped): boot cost = PSRAM footprint of the same payload read
#     off the status heap line before/after an install; flush interference = the
#     fps floor of the `clock:` lines that fall INSIDE Arm A's copy window (the
#     worker copy IS the arena copy's card-read half; §14's M3 arena number
#     carries the memcpy half). Both halves are read, not simulated.
#
# Preconditions: /tmp/m5_avkit.orbtheme (name av-kit) and /tmp/m5_avkit_b.orbtheme
# (name av-kit-b, same payload re-packed by the sanctioned writer) exist and pass
# orbtheme.py --inspect; the card is in; the port is free. Every response body
# goes to a freshly deleted file and is only believed if size_download > 0 — the
# §16.1 rule: a 200 header is not a response.
#
# Run DETACHED:
#   cd /home/greg/docker/Projects/gorb-os-2
#   setsid tools/m5_flip.sh > /tmp/m5f_run.log 2>&1 < /dev/null &
set -u
ORB=http://192.168.2.32
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

# [1] fixtures, host-verified before the radio carries anything
python3 - <<'PY' || exit 1
import sys; sys.path.insert(0, "tools")
import orbtheme
for p in ("/tmp/m5_avkit.orbtheme", "/tmp/m5_avkit_b.orbtheme"):
    h, geom, art = orbtheme.read_container(p)
    print(p, "name", repr(h["name"]), "id", orbtheme.theme_id(h["name"]),
          "art", h["art_size"], "fmt", h.get("format", 2))
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

flip(){ # flip ID LABEL — one Arm A flip, fully timed
  local id=$1 label=$2
  local t0=$(date +%s.%N)
  fetch POST "$ORB/api/v1/sd/install?id=$id" || { fail "install $label: no body"; return; }
  say "install($label): $CODE -> $(cat "$BODY")"
  grep -q '"state":"installing"' "$BODY" || { fail "install $label refused: $(cat "$BODY")"; return; }
  local t1=$(date +%s.%N)
  local E=$(grep -E 'rst:0x' "$SER" | tail -1 | cut -d' ' -f1)
  local R=""
  # (a) the boundary: a NEW rst:0x after the pre-install boot's own (open-reset) one
  for j in $(seq 1 90); do
    sleep 1
    R=$(awk -v e="$E" '$1+0>e+3 && /rst:0x/ {print $1; exit}' "$SER")
    [ -n "$R" ] && break
  done
  [ -n "$R" ] || { fail "flip $label: no reboot boundary within 90 s"; return; }
  # (b) first dial line AFTER that boundary — pre-reboot clock lines must not count
  for j in $(seq 1 60); do
    sleep 1
    awk -v e="$R" '$1+0>e && /clock: clock [0-9]+ fps/ {f=1} END{exit !f}' "$SER" && break
  done
  local t2=$(date +%s.%N)
  # (c) the config page answering again after that boundary
  for k in $(seq 1 90); do
    sleep 1
    awk -v e="$R" '$1+0>e && /config server listening/ {f=1} END{exit !f}' "$SER" && break
  done
  local t3=$(date +%s.%N)
  say "--- flip $label ---"
  awk -v e="$E" '$1+0>e-8' "$SER" | grep -E 'install of|slot-|art bank|rst:0x|sta ip|config server|clock: clock [0-9]+ fps' | head -24
  say "flip $label: post-to-first-dial $(echo "$t2-$t1" | bc)s  post-to-page-answers $(echo "$t3-$t1" | bc)s"
  say "flip $label: min dial fps in the copy window (Arm B flush interference): $(dial "${E%.*}")"
}

# [5] Arm A, both directions: to av-kit-b, back to av-kit (a flip, not an install)
flip "$IDC" "to-avkit-b"
sleep 5
flip "$IDB" "back-to-avkit"

# [6] Arm B's PSRAM half: footprint after the copy, off the same status line
fetch GET "$ORB/api/v1/status" && PSRAM1=$(python3 -c "import json;print(json.load(open('$BODY'))['heap_psram'])")
say "psram after flips: ${PSRAM1:-?} (baseline ${PSRAM0:-?}) — lazy residency would cost the payload's page bytes here, and §14's M3 arena number (per-byte copy) prices the rest"

kill $CAP 2>/dev/null
say "=== M5_FLIP $verdict ==="
