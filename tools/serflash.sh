#!/bin/bash
# Serial-flash THIS clone's build into THIS clone's bound orb, and only that orb.
#
# Device selection is never positional. The port comes from the untracked
# .orb-target at the clone root (tools/orb_bind.sh), esptool read_mac confirms
# the chip's own MAC before anything is written, and flock holds the port so a
# second writer fails loudly instead of rebooting the board mid-stream. The old
# version hardcoded /dev/ttyACM0 and gorb-os-2/build -- either clone could flash
# whichever orb enumerated first, which is the accident this now refuses.
#
# Writes the app (0x20000) and otadata to boot ota_0; ota_1 stays as the
# rollback. bootloader, partition table and assets are unchanged, not re-sent.
# Serial is the reliable path here: WiFi OTA aborts mid-stream at rssi ~-79.
#
# The stamp audit is the second guard. /api/v1/status's fw comes from
# esp_app_desc_t, which CMake stamps at CONFIGURE time and caches: idf.py build
# alone recompiles changed sources but keeps the old stamp, so an image carrying
# uncommitted edits can report a clean hash it was never built from (observed
# 2026-10-11: art_bank.c edited 14:46:10, image built 14:46:31, stamp still
# plain 54094e8). Read the stamp out of the image itself -- version at byte 48
# of the app .bin -- and refuse the flash unless it equals
# `git describe --always --dirty` for this tree.
# Fix a mismatch with:  (cd "$ORB_ROOT" && idf.py reconfigure && idf.py build)
#
# Usage: tools/serflash.sh [--check]
#   --check  run binding, gates, lock and audit, then stop without esptool.
set -u
TOOLS="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=./orb_bind.sh
. "$TOOLS/orb_bind.sh"
orb_require

PY="$ORB_ESPTOOL_PY"
B="$ORB_ROOT/build"

orb_resolve_port
orb_lock_port "$ORB_PORT"
orb_gate_mac "$ORB_PORT"
[ -f "$B/gorb-os.bin" ] || orb_die "no $B/gorb-os.bin -- build first:
(cd $ORB_ROOT && idf.py reconfigure && idf.py build)"

: > /tmp/serflash_audit.log
WANT=$(git -C "$ORB_ROOT" describe --always --dirty 2>/dev/null)
HAVE=$(dd if="$B/gorb-os.bin" bs=1 skip=48 count=32 status=none | tr -d '\0')
{
  echo "AUDIT orb=$ORB_LABEL port=$ORB_PORT mac=$ORB_BASE_MAC want=$WANT have=$HAVE" | tee -a /tmp/serflash_audit.log
  if [ "$HAVE" != "$WANT" ]; then
    echo "AUDIT FAIL: $B/gorb-os.bin is stamped [$HAVE]; the tree is [$WANT]." | tee -a /tmp/serflash_audit.log
    echo "        The image does not correspond to the code you are about to sign off."
    echo "        Fix: (cd $ORB_ROOT && idf.py reconfigure && idf.py build), re-run."
    echo "SERFLASH_DONE $(date +%T)"
    exit 1
  fi
  echo "AUDIT OK: the image's own stamp matches the working tree." | tee -a /tmp/serflash_audit.log
}

if [ "${1:-}" = "--check" ]; then
  echo "CHECK OK: $ORB_LABEL bound, port gated, stamp audited; nothing was flashed."
  exit 0
fi

sudo "$PY" -m esptool --chip esp32s3 -p "$ORB_PORT" -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x20000 "$B/gorb-os.bin" 0x11000 "$B/ota_data_initial.bin" \
  > /tmp/serflash.log 2>&1
echo "EXIT=$?" >> /tmp/serflash.log
echo "SERFLASH_DONE $(date +%T)" >> /tmp/serflash.log
