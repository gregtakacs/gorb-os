#!/bin/bash
# Verify the captive-portal responder against the one invariant that matters on
# this bench (no WiFi radio to join the AP): opening the AP must (a) bring the
# responder up and (b) NOT disturb the station or its SNTP -- the whole point of
# binding to the AP address instead of 0.0.0.0. Serial capture runs across the
# open so we see the responder's own log lines, then the AP is closed.
#
# Relocated from /tmp and bound (2026-10-11): every curl targets the clone's
# ORB_IP after orb_gate_ip proves the address answers as the bound orb's MAC.
set -u
TOOLS="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=./orb_bind.sh
. "$TOOLS/orb_bind.sh"
orb_require
orb_gate_ip
IP="$ORB_RESOLVED_IP"

LOG=/tmp/ap_cp.log
"$ORB_ESPTOOL_PY" "$TOOLS/capf.py" 11 "$LOG" --no-reset &
CAP=$!
sleep 1
echo '=== open AP (station currently has a lease) ==='
curl -s -m 6 -X POST "http://$IP/api/v1/wifi/ap" -d '{}'; echo
sleep 2
echo '=== LAN status mid-AP: station must still be connected + synced ==='
curl -s -m 6 "http://$IP/api/v1/status" | tr ',' '\n' | grep -E 'state|rssi|synced|assoc_fails|ap"|wifi_reason'
echo '=== the 5 captive-portal probe endpoints must all 302 -> / ==='
for u in /generate_204 /hotspot-detect.html /favicon.ico /gen_204 /connect/test.php; do
  printf '%-22s ' "$u"
  curl -s -m 4 -o /dev/null -w '%{http_code} loc=%{redirect_url}\n' "http://$IP$u"
done
echo '=== close the AP ==='
curl -s -m 6 -X POST "http://$IP/api/v1/wifi/ap" -d '{"on":"off"}'; echo
wait $CAP
echo '=== serial during the open (want: captive DNS answering ... then stopped) ==='
tr '\r' '\n' < "$LOG" | grep -aiE 'captive DNS|SoftAP|DHCP|disassoc|associated|sntp|retrying' | sed 's/^/  /'
