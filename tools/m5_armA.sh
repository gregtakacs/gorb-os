#!/bin/bash
set -u
ORB=http://192.168.2.32
SER=/tmp/m5_armA2_ser.txt
REPO=/home/greg/docker/Projects/gorb-os-2
ID=5eb1ed3ae931
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
E2=$(grep -E 'clock: clock [0-9]+ fps' $SER | tail -1 | cut -d' ' -f1)
say "serial epoch anchor=$E2"
curl -s -m 120 --data-binary @/tmp/m5_avkit.orbtheme -H 'Content-Type: application/octet-stream' -o /tmp/m5_up.json -w 'upload: code=%{http_code} size=%{size_download} t=%{time_total}\n' "$ORB/api/v1/sd/theme?overwrite=1"
say "upload body: $(cat /tmp/m5_up.json 2>/dev/null)"
nap 3
curl -s -m 60 -X POST -o /tmp/m5_inst.json -w 'install: code=%{http_code} size=%{size_download} t=%{time_total}\n' "$ORB/api/v1/sd/install?id=$ID"
say "install body: $(cat /tmp/m5_inst.json 2>/dev/null)"
for j in $(seq 1 60); do
  nap 2
  awk -v e="$E2" '$1+0>e+5 && /rst:0x/ {f=1} END{exit !f}' $SER && break
done
say 'reboot boundary seen'
for k in $(seq 1 45); do
  nap 2
  awk -v e="$E2" '$1+0>e+5 && /clock: clock [0-9]+ fps/ {f=1} END{exit !f}' $SER && break
done
say '=== install request to first dial (serial, epoch-windowed) ==='
awk -v e="$E2" '$1+0>e && $1+0<e+160' $SER | grep -E 'install of|slot-|art bank|rst:0x|sta ip|clock: clock [0-9]+ fps|httpd_start|heap_init|post-wifi|post-httpd|card|esp-aes|sntp' | grep -viE 'blend|phases' | head -34
say 'ARM_A2_DONE'
