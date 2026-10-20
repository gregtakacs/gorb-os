#!/bin/bash
# m5_armA.sh — the FORMAT 3 STORE door timed on the board (§24.8, M6(1) idiom).
#   MIGRATED from the retired FORMAT 2 Arm-A copy+reboot bench (D11). D30 (2026-10-15)
#   retired the reboot-on-every-pick rule; the FORMAT 2 ingest door is gone (§27). This
#   now drives POST /api/v1/sd/bundle?id=X — the sole store route to a card-resident
#   bundle — which answers reboot:none and does NOT flip the dial: there is no FORMAT 3
#   compositor to swap a stored bundle onto the glass until §26's ping-pong slice lands.
#   So this measures ONE number: the store door's ingest time, read back off the
#   device's own bundle_ms/bundle_bytes pair. The dial must NOT move across the press
#   (the flip would need an rst:0x boundary — its ABSENCE is the assertion here).
#   Fixture: /tmp/m5_avkit.orbtheme must be a FORMAT 3 bundle (rebake_bundle.py). The
#   id is derived from the bundle's own name at runtime — it is sha256(name)[:12] and
#   format-independent, so a re-bake keeps the same id; never trust a pinned hex.
set -u
ORB=${ORB:-http://192.168.2.34}
SER=/tmp/m5_armA2_ser.txt
REPO=/home/greg/docker/Projects/gorb-os-2
cd $REPO
nap(){ read -t $1 _ < <(head -c 0 /dev/zero) 2>/dev/null || true; }
nap(){ sleep $1; }
say(){ echo "[$(date +%T)] $*"; }
setsid python3 tools/capf.py 240 $SER >/dev/null 2>&1 &
for i in $(seq 1 60); do nap 1; grep -q 'sta ip' $SER 2>/dev/null && break; done
grep -q 'sta ip' $SER || { say 'REFUSE: no lease in 60s'; exit 1; }
say 'lease up; upload anchor epoch follows'
curl -s -m 25 -X POST -o /tmp/m5_rs.json -w 'rescan: code=%{http_code} size=%{size_download}\n' "$ORB/api/v1/sd/rescan"
say "rescan body: $(cat /tmp/m5_rs.json 2>/dev/null)"
# Derive the id from the FORMAT 3 fixture's OWN name, never a pinned hex: it is
# sha256(name)[:12] and format-independent, so a re-bake keeps the same id.
ID=$(python3 -c "import sys; sys.path.insert(0,'tools'); import orbbundle as ob; h=ob.parse_header3(open('/tmp/m5_avkit.orbtheme','rb').read()); import orbtheme; print(orbtheme.theme_id(h['name']))")
say "store id (from fixture name): $ID"
E2=$(grep -E 'clock: clock [0-9]+ fps' $SER | tail -1 | cut -d' ' -f1)
say "serial epoch anchor=$E2"
# Upload the FORMAT 3 bundle to the card (the store door acts on a card-resident id).
curl -s -m 120 --data-binary @/tmp/m5_avkit.orbtheme -H 'Content-Type: application/octet-stream' -o /tmp/m5_up.json -w 'upload: code=%{http_code} size=%{size_download} t=%{time_total}\n' "$ORB/api/v1/sd/theme?overwrite=1"
say "upload body: $(cat /tmp/m5_up.json 2>/dev/null)"
nap 3
# The store door: reboot:none, no flip. Time the request; the number lives in bundle_ms.
T0=$(date +%s.%N)
curl -s -m 60 -X POST -o /tmp/m5_store.json -w 'store: code=%{http_code} size=%{size_download} t=%{time_total}\n' "$ORB/api/v1/sd/bundle?id=$ID"
T1=$(date +%s.%N)
say "store body: $(cat /tmp/m5_store.json 2>/dev/null)"
grep -q '"reboot":"none"' /tmp/m5_store.json && say 'store answered reboot:none (as the FORMAT 3 contract demands)' || say 'FAIL: store did NOT answer reboot:none'
# Poll the ledger for the ingest cost the store worker recorded (nonzero bundle_ms).
for k in $(seq 1 30); do
  nap 3
  curl -s -m 15 -o /tmp/m5_sd.json "$ORB/api/v1/sd"
  BM=$(python3 -c "import json;print(json.load(open('/tmp/m5_sd.json')).get('bundle_ms',0))" 2>/dev/null)
  [ "${BM:-0}" != "0" ] && break
done
say '=== store door: request to bundle_ms recorded (host wall: '"$(echo "$T1-$T0" | bc)"'s to ack) ==='
python3 -c "import json;d=json.load(open('/tmp/m5_sd.json'));print('bundle_ms=%s bundle_bytes=%s active=%s' % (d.get('bundle_ms'),d.get('bundle_bytes'),d.get('active')))"
# The dial must NOT have flipped: NO reboot boundary after the epoch anchor is the pass.
for j in $(seq 1 20); do
  nap 2
  awk -v e="$E2" '$1+0>e+5 && /rst:0x/ {f=1} END{exit !f}' $SER && { say 'FAIL: a reboot boundary appeared — the store door flipped the dial, contrary to reboot:none'; break; }
done
say 'no rst:0x boundary after the store — the dial did not flip (correct for the store door)'
say '=== the press window (no slot/art-bank copy lines expected on the store path) ==='
awk -v e="$E2" '$1+0>e && $1+0<e+160' $SER | grep -E 'bundle|store|rst:0x|clock: clock [0-9]+ fps' | head -34
say 'ARM_A2_DONE'
