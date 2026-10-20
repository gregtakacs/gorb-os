#!/bin/bash
# bench_powercut.sh — P4's power-cut-mid-install pass line, cut with the board's own
#   reboot door. Renamed from m5_powercut.sh: the M5-era name outlived the M5
#   comparison it recorded; the bench itself and its verdict stand unchanged.
# The mechanism (plan §8 P4; D9 in orb_app_install.c:429): h_reboot arms a fixed
# 750 ms one-shot timer and does NOT drain orb_jobs (orb_net.c:3688) — a blunt
# instrument, which is exactly what a power cut is. Arming the cut BEFORE the
# install POST puts rst at ≈T0+750 ms while the worker is still in
# erase→copy→read-back (≈T0+300..700 ms for the 4 KB hsb_color blob with its
# three pre-erase card passes), so a tear lands between the blob erase and the
# last-write header — the state a pulled plug leaves.
#
# Pass line as witnessed:
#   (1) the cut lands inside the window: serial shows this attempt's rst BEFORE
#       any `installed "hsb_color" ... into app slot` line after it armed;
#   (2) the dial boots: art bank mapped + clock fps lines after that rst;
#   (3) the torn app is ABSENT, never half (D9): /api/v1/apps installed [] while
#       the card row stays listed ok:true;
#   (4) recovery: re-install lands the worker line and the ledger row.
# The plan's `failed`-mark half is reported for what it is: try_note is RAM-only
# (orb_app_install.c:169), so a cut erases the note too; the reachable device
# `failed` marks are C2's fault counter and init() refusal — both need a second
# behaviour kind, the same blocker as the `*`-on-the-ladder line.
#
# Run DETACHED:
#   cd /home/greg/docker/Projects/gorb-os-2
#   setsid tools/bench_powercut.sh > /tmp/m5p_run.log 2>&1 < /dev/null &
set -u
SER=/tmp/m5p_ser.txt
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

# [0] two fixtures from the one sanctioned writer: the 4 KB canonical hsb_color
# (recovery copy) and a pad-geometry sibling — same slug, geometry at the
# DEVICE's slot boundary. The device's rule is the SUM geom_off + geom_size ≤
# 0x40000 (orb_app_install.c:61; == cap accepted), and hsb_color's bare header
# puts geom_off at 0xB0, so geometry 0x3FE00 lands the blob at 0x3FEB0 — inside
# it. `orbapp.write_container` now enforces that sum itself (2026-10-15, plan
# §21), so the 0x40000 pad that §19.3 saw the dial refuse pre-erase ("262320 B:
# a slot holds 262144 B") is now refused on the host, in the same words. The
# accepted pad widens erase→header from ~200 ms to a few seconds so the 750 ms
# fuse lands inside it.
mkdir -p /tmp/m5p/small /tmp/m5p/pad
python3 - <<'PY' || exit 1
import sys; sys.path.insert(0, "tools")
import orbapp
n = orbapp.write_container("/tmp/m5p/small/hsb_color.orbapp", "hsb_color", "HSB Colour",
                           caps=0, behaviour_kind=1)
m = orbapp.write_container("/tmp/m5p/pad/hsb_color.orbapp", "hsb_color", "HSB Colour",
                           caps=0, behaviour_kind=1, geometry=b"\xa5" * 0x3FE00)
print("wrote small", n, "B and padded", m, "B")
PY

# [1] capture, lease
rm -f "$SER"
setsid python3 tools/capf.py 1200 "$SER" >/dev/null 2>&1 &
CAP=$!
for i in $(seq 1 150); do sleep 1; grep -q 'sta ip' "$SER" 2>/dev/null && break; done
grep -q 'sta ip' "$SER" || { fail 'no lease in 150 s'; exit 1; }
say "lease up; capture at $SER"
curl -s -m 20 -X POST "$ORB/api/v1/sd/rescan" >/dev/null

# [1b] put the pad on the card — the row must list ok:true or this lever is dead
UP=$(curl -s -m 120 -X POST --data-binary @/tmp/m5p/pad/hsb_color.orbapp \
     -H 'Content-Type: application/octet-stream' -o /tmp/m5p_up.json \
     -w 'code=%{http_code} size=%{size_download}' "$ORB/api/v1/sd/app?overwrite=1")
sleep 2
ROW=$(curl -s -m 15 "$ORB/api/v1/apps" | grep -o '"stem":"hsb_color"[^}]*' | head -1)
say "padded upload: $UP -> ${ROW:-no-row}"
echo "$ROW" | grep -q '"ok":true' || { fail 'padded fixture refused by the reader — no cut lever'; kill $CAP 2>/dev/null; say "=== M5_POWERCUT $verdict ==="; exit 1; }

# [2] trial loop: queue the install, THEN arm the 750 ms fuse. Pre-erase card
# verifies run ≈1 s for the 266 KB pad; the 65-sector erase that follows runs
# ≈3 s — the widest headerless window the ladder offers. Install-first lands the
# fuse inside that erase (fuse-first fired rst mid-verify, header untouched:
# trial 1 of the first padded run).
TORN=0
for try in 1 2 3 4 5; do
  RST_E=$(grep -E 'rst:0x' "$SER" | tail -1 | cut -d' ' -f1)
  II=$(curl -s -m 10 -X POST "$ORB/api/v1/apps/install?id=hsb_color" -w '%{http_code}')
  sleep 0.05  # fuse lit at once fires at ≈install+0.8 s: the accepted-install
              # window measured [header-erased .. header-at-906..1346 ms] —
              # this puts rst inside it and takes the ~50% odds five times
  curl -s -m 10 -X POST "$ORB/api/v1/reboot" >/dev/null   # 750 ms fuse, lights out
  say "trial $try: install queued (code $II), fuse armed"
  sleep 30
  # boot-complete by HTTP, not serial: capf opens the port once and a mid-run
  # re-enumeration can strand its fd (learned 14:32); the ledger is the judge.
  for i in $(seq 1 60); do sleep 2; curl -s -m 3 "$ORB/api/v1/status" -o /dev/null && break; done
  DONE=$(awk -v e="$RST_E" '$1+0>e' "$SER" 2>/dev/null | grep -c 'into app slot')
  INST=$(curl -s -m 15 "$ORB/api/v1/apps" | grep -o '"installed":\[[^]]*\]' | head -c 120)
  say "trial $try ledger: ${INST:-no-body} (install-complete serial line seen: $DONE)"
  if [ "$DONE" = 0 ] && echo "$INST" | grep -q '\[\]'; then TORN=1; say "trial $try TORN the slot — D9's state reached"; break; fi
  sleep 4
done
[ "$TORN" = 1 ] || { fail 'no trial landed the cut inside the write window (all completed cleanly or ledger never emptied)'; }

# [3] the torn state's witnesses (best effort — a dead capture is reported, not hidden)
if [ "$TORN" = 1 ]; then
  grep -E 'rst:0x' "$SER" | tail -2
  tail -400 "$SER" 2>/dev/null | grep -E 'art bank mapped|clock: clock [0-9]+ fps|appinstall' | tail -8
  CARDROW=$(curl -s -m 15 "$ORB/api/v1/apps" | grep -o '"stem":"hsb_color"[^}]*"ok":[a-z]*' | head -1)
  say "card row after the tear: ${CARDROW:-no-body}"
  echo "$CARDROW" | grep -q '"ok":true' || fail 'card row not listed ok after the tear'
  tail -200 "$SER" 2>/dev/null | grep -E 'resolved [0-9]+ installed app|art bank mapped' | tail -2
  [ "$(($(date +%s) - $(stat -c %Y "$SER" 2>/dev/null || echo 0)))" -gt 120 ] && say "NOTE: serial capture stranded mid-run; the ledger carried the verdict"
fi

# [4] recovery: put the canonical 4 KB fixture back on the card, re-install clean,
# re-enable, back to the starting state
curl -s -m 60 -X POST --data-binary @/tmp/m5p/small/hsb_color.orbapp \
     -H 'Content-Type: application/octet-stream' -o /dev/null "$ORB/api/v1/sd/app?overwrite=1" >/dev/null
sleep 2
curl -s -m 15 -X POST "$ORB/api/v1/apps/install?id=hsb_color" >/dev/null
for i in $(seq 1 40); do sleep 1; tail -60 "$SER" | grep -q 'into app slot' && break; done
tail -30 "$SER" | grep -E 'installed "hsb_color"|into app slot' | tail -2
sleep 2
curl -s -m 15 -X POST "$ORB/api/v1/apps/enable?id=hsb_color&on=1" >/dev/null
curl -s -m 15 "$ORB/api/v1/apps" -o /tmp/m5p_final.json
grep -o '"installed":\[.*\]' /tmp/m5p_final.json | head -c 300
grep -q '"slug":"hsb_color"' /tmp/m5p_final.json || fail 'recovery install missing from the ledger'
grep -q '"enabled":true' /tmp/m5p_final.json || fail 're-enable did not stick'

kill $CAP 2>/dev/null
say "=== M5_POWERCUT $verdict ==="