#!/bin/bash
# m5_upload_proof.sh — open item 1 of docs/app-platform-plan.md §17.4, benched.
#
# The claim under test: with the tile-ring loan (829d8ca + f7481db), a browser
# upload reaches the card on a boot with the radio associated, and every refusal
# answers with the truth (a DMA-starved write never answers 413 — fixed this
# session). Full ladder: lease -> rescan -> .orbapp upload -> card row listed ->
# install (worker copy card->flash, LIVE by design — the row appears on the dial
# without a reboot; cold-boot resolve is the P4a bench's already-proven half) ->
# row on the dial -> enable. Serial is the witness: the §17.6 rule
# says read the serial line and the body file's mtime before believing any
# HTTP body, so every response is written to a freshly deleted file.
#
# Run DETACHED (it exceeds any 30 s tool cap by design):
#   cd /home/greg/docker/Projects/gorb-os-2
#   setsid tools/m5_upload_proof.sh > /tmp/m5p_run.log 2>&1 < /dev/null &
# Prereq: the board flashed with the bundle this session packed, card in socket,
# nothing else holding the port (capf.py opens it, and the open resets the board
# — that reset IS the clean boot this bench wants, plan §17.6).
set -u
ORB=http://192.168.2.32
REPO=/home/greg/docker/Projects/gorb-os-2
SER=/tmp/m5p_ser.txt
BODY=/tmp/m5p_body.json
FIX=/tmp/m5p/hsb_color.orbapp
SLUG=hsb_color
cd "$REPO" || exit 1
say(){ echo "[$(date +%T)] $*"; }
verdict=PASS
fail(){ say "FAIL: $*"; verdict=FAIL; }

# [1] fresh fixture from the one sanctioned writer (C11)
mkdir -p /tmp/m5p
python3 - <<'PY' || exit 1
import sys; sys.path.insert(0, "tools")
import orbapp
n = orbapp.write_container("/tmp/m5p/hsb_color.orbapp", "hsb_color", "HSB Colour",
                           caps=0, behaviour_kind=1)
print("wrote /tmp/m5p/hsb_color.orbapp", n, "bytes")
PY
SZ=$(stat -c %s "$FIX")

# [2] capture from the open-reset boot; wait for the DHCP lease
rm -f "$SER"
setsid python3 tools/capf.py 600 "$SER" >/dev/null 2>&1 &
CAP=$!
for i in $(seq 1 150); do sleep 1; grep -q 'sta ip' "$SER" 2>/dev/null && break; done
grep -q 'sta ip' "$SER" || { fail 'no lease in 150 s'; exit 1; }
E0=$(grep -E 'rst:0x' "$SER" | head -1 | cut -d' ' -f1)
say "boot witness at t=${E0:-?}; lease up"
grep -E 'tile ring|heap_init|post-httpd|post-wifi|config server' "$SER" | head -8

# [3] mount door
rm -f "$BODY"
curl -s -m 25 -X POST -o "$BODY" -w 'rescan: code=%{http_code} size=%{size_download}\n' "$ORB/api/v1/sd/rescan"
say "rescan: $(cat "$BODY" 2>/dev/null)"
grep -q '"mounted":true' "$BODY" || fail 'no card at the rescan door'

# [4] the upload — the claim this item exists to prove
rm -f "$BODY"
UP=$(curl -s -m 180 -X POST --data-binary @"$FIX" -H 'Content-Type: application/octet-stream' \
     -o "$BODY" -w 'code=%{http_code} size=%{size_download} t=%{time_total}' "$ORB/api/v1/sd/app")
say "upload(${SZ} B): $UP -> $(cat "$BODY" 2>/dev/null)"
if grep -q '"state":"stored"' "$BODY"; then
  say 'upload landed first try'
elif echo "$UP" | grep -q 'code=409'; then
  rm -f "$BODY"
  UP=$(curl -s -m 180 -X POST --data-binary @"$FIX" -H 'Content-Type: application/octet-stream' \
       -o "$BODY" -w 'code=%{http_code} t=%{time_total}' "$ORB/api/v1/sd/app?overwrite=1")
  say "upload(overwrite): $UP -> $(cat "$BODY" 2>/dev/null)"
  grep -q '"state":"stored"' "$BODY" || fail 'overwrite upload did not store'
else
  fail "upload refused: $UP"
fi
# the honest-mapping witness: 413 on a refused WRITE is the mis-map still living
echo "$UP" | grep -q 'code=413' && fail 'a fresh upload answered 413 — mis-map still alive'

# [5] loan and refusal lines off serial
say '--- serial witnesses: did the loan run, did a dma refusal recur? ---'
grep -nE 'tile ring borrowed|tile ring not reclaimed|esp_dma_capable_malloc|card write failed|appinstall:|app upload' "$SER" | head -14

# [6] the card lists it
rm -f "$BODY"
curl -s -m 25 -o "$BODY" "$ORB/api/v1/apps" && say "GET /api/v1/apps: $(head -c 400 "$BODY")"
grep -q "$SLUG" "$BODY" || fail 'the uploaded app is not listed on the card'

# [7] install: worker copy card->flash, header last, reboot automatic (D9)
rm -f "$BODY"
IN=$(curl -s -m 60 -X POST -o "$BODY" -w 'code=%{http_code} t=%{time_total}' "$ORB/api/v1/apps/install?id=$SLUG")
say "install: $IN -> $(cat "$BODY" 2>/dev/null)"
echo "$IN" | grep -q 'code=200' || fail 'install door refused'
# The install's witness is the WORKER LINE, not an rst:0x — a card-app install
# lands live (orb_app_install.c carries no esp_restart; the reboot belongs to the
# theme path under D11). First bench run of this script burned 120 s waiting on a
# reboot that the design never promises.
INSTALLED_NO=0
for j in $(seq 1 30); do sleep 1; grep -q 'into app slot' "$SER" && { INSTALLED_NO=1; break; }; done
[ "$INSTALLED_NO" = 1 ] || fail 'the install worker line never arrived'
say "install witness: $(grep -m1 'into app slot' "$SER")"
grep -q 'job done' "$SER" && say 'worker stood down clean'
say '--- install to dial ledger ---'
awk -v e="${E0:-0}" '$1+0>e' "$SER" | grep -E 'appinstall:|jobs:' | head -8
sleep 2
rm -f "$BODY"
curl -s -m 15 -o "$BODY" "$ORB/api/v1/apps" || fail 'the ledger read after install refused'
grep -q "\"slot\":0" "$BODY" || fail 'the installed tier is empty after the worker line'

# [8] enable — the P4a confirm, one door past the item-1 claim
sleep 3
rm -f "$BODY"
curl -s -m 25 -X POST -o "$BODY" "$ORB/api/v1/apps/enable?id=$SLUG&on=1" && say "enable: $(cat "$BODY")"
grep -q '"state":"enabled"' "$BODY" || fail 'enable door refused the freshly installed row'

kill $CAP 2>/dev/null
say "=== UPLOAD_PROOF $verdict ==="
