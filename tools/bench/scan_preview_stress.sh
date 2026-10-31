#!/bin/bash
# Reproduce the preview stall from 2026-10-31: a fresh Wi-Fi scan answered
# "scanning" and kept the radio off-channel while the next preview streamed, and
# the stream's unsent bytes pinned the internal RAM the radio needed to send
# them. Each round: fresh scan, a preview straight behind it, a card listing,
# then the heap. One request at a time, like the hub's lane.
#
# Run beside tools/bench/heap_watch.py, after the orb has booted.
# Usage: scan_preview_stress.sh <orb-ip> [rounds] [theme-id]
IP=${1:?orb ip}
ROUNDS=${2:-12}
ID=${3:-clock-skeleton-209c630ff17d}

heap() {
  curl -s -m 8 "http://$IP/api/v1/status" |
    python3 -c 'import json,sys;d=json.load(sys.stdin);print("free=%d largest=%d" % (d["heap_internal"],d["heap_internal_largest"]))' 2>/dev/null ||
    echo "status: no answer"
}

whole=0; torn=0
for i in $(seq 1 "$ROUNDS"); do
  scan=$(curl -s -m 10 "http://$IP/api/v1/wifi/scan?fresh=1")
  pv=$(curl -s -m 40 -o /dev/null -w '%{http_code} %{size_download} %{time_total}' "http://$IP/api/v1/sd/preview?id=$ID")
  sd=$(curl -s -m 30 -o /dev/null -w '%{http_code} %{time_total}' "http://$IP/api/v1/sd")
  set -- $pv
  if [ "$1" = 200 ] && [ "$2" = 107652 ]; then whole=$((whole+1)); else torn=$((torn+1)); fi
  echo "$(date +%T) round $i: scan=${scan:0:24} preview=[$pv] sd=[$sd] $(heap)"
  sleep 2
done
echo "$(date +%T) previews whole=$whole torn=$torn of $ROUNDS"
