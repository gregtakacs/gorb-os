#!/bin/bash
# Serial-flash THIS clone's build into THIS clone's bound orb, and only that orb.
#
# Device selection is never positional. The port comes from the untracked
# .orb-target at the clone root (tools/orb_bind.sh), esptool read_mac confirms
# the chip's own MAC before anything is written, and a per-orb sidecar flock
# fails a second writer loudly instead of rebooting the board mid-stream. The old
# version hardcoded /dev/ttyACM0 and gorb-os-2/build -- either clone could flash
# whichever orb enumerated first, which is the accident this now refuses.
#
# Writes the app (0x20000) and otadata to boot ota_0; ota_1 stays as the
# rollback. bootloader, partition table and assets are unchanged, not re-sent.
# Serial is the reliable path here: WiFi OTA aborts mid-stream at rssi ~-79.
#
# A verified build has to be re-flashed, so the images are kept for exactly that,
# per orb and two deep: /tmp/orb-last-<label>.bin is what this script last wrote to
# that board, /tmp/orb-previous-<label>.bin the write before it. A real flash
# promotes last into previous only when the bytes actually differ — re-flashing the
# same image must not silently destroy the only older image there is.
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
# Usage: tools/serflash.sh [--check] [--rollback]
#   --check     run the binding, gates, lock and audit, then stop without esptool.
#   --rollback  write the previous image back instead of the build. Its stamp is
#               deliberately NOT audited -- a rollback exists for exactly the case
#               where the tree has moved past the build being undone -- but it is
#               logged, so the artifact still names what went into the board.
set -u
MODE="flash"
CHECK=0
for arg in "$@"; do
  case "$arg" in
    --check) CHECK=1 ;;
    --rollback) MODE="rollback" ;;
    *) echo "unknown argument '$arg': usage tools/serflash.sh [--check] [--rollback]" >&2; exit 2 ;;
  esac
done
TOOLS="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=./orb_bind.sh
. "$TOOLS/orb_bind.sh"
orb_require

LOGFILE="/tmp/serflash-$ORB_LABEL.log"
PY="$ORB_ESPTOOL_PY"
B="$ORB_ROOT/build"
LAST="/tmp/orb-last-$ORB_LABEL.bin"
PREV="/tmp/orb-previous-$ORB_LABEL.bin"
APP="$B/gorb-os.bin"

# The rollback image is chosen before the port work: discovering there is nothing to
# roll back to must not cost a MAC read or take a lock on a run that flashes nothing.
if [ "$MODE" = "rollback" ]; then
  [ -f "$PREV" ] || orb_die "no $PREV -- this orb has never had a second image written by
this script, so there is nothing verified to roll back to. Nothing was written. (The
snapshot only fills in on a real flash whose bytes differ from the last one.)"
  APP="$PREV"
fi

orb_resolve_port
orb_gate_mac "$ORB_PORT"

if [ "$MODE" = "flash" ]; then
  [ -f "$B/gorb-os.bin" ] || orb_die "no $B/gorb-os.bin -- build first:
(cd $ORB_ROOT && idf.py reconfigure && idf.py build)"
fi
if [ "$CHECK" != 1 ]; then
  [ -f "$B/ota_data_initial.bin" ] || orb_die "no $B/ota_data_initial.bin -- the boot to
ota_0 needs it; build first: (cd $ORB_ROOT && idf.py reconfigure && idf.py build)"
fi

: > "/tmp/serflash-audit-$ORB_LABEL.log"
AUDLOG="/tmp/serflash-audit-$ORB_LABEL.log"
WANT=$(git -C "$ORB_ROOT" describe --always --dirty 2>/dev/null)
HAVE=$(dd if="$APP" bs=1 skip=48 count=32 status=none | tr -d '\0')
{
  echo "AUDIT orb=$ORB_LABEL mode=$MODE app=$APP port=$ORB_PORT mac=$ORB_BASE_MAC want=$WANT have=$HAVE" | tee -a "$AUDLOG"
  if [ "$MODE" = "flash" ] && [ "$HAVE" != "$WANT" ]; then
    echo "AUDIT FAIL: $B/gorb-os.bin is stamped [$HAVE]; the tree is [$WANT]." | tee -a "$AUDLOG"
    echo "        The image does not correspond to the code you are about to sign off."
    echo "        Fix: (cd $ORB_ROOT && idf.py reconfigure && idf.py build), re-run."
    echo "SERFLASH_DONE $(date +%T)"
    exit 1
  fi
  if [ "$MODE" = "flash" ]; then
    echo "AUDIT OK: the image's own stamp matches the working tree." | tee -a "$AUDLOG"
  else
    # Not a refusal, an accounting line: the stamp of a rollback image is EXPECTED to
    # differ from the tree, and saying so here is what keeps the audit honest instead
    # of quietly absent.
    echo "AUDIT rollback: writing the snapshot stamped [$HAVE] while the tree is [$WANT]; the mismatch is the point." | tee -a "$AUDLOG"
  fi
}

if [ "$CHECK" = 1 ]; then
  echo "CHECK OK: $ORB_LABEL bound, port gated, $MODE image audited; nothing was flashed."
  exit 0
fi

# Promoted BEFORE the write, never after: a flash that dies halfway still replaced
# what the board was running, and the image it ran must already be safe on disk. An
# identical re-flash must not burn the only older image, hence the cmp.
if [ "$MODE" = "flash" ]; then
  if [ -f "$LAST" ] && ! cmp -s "$LAST" "$B/gorb-os.bin"; then
    cp "$LAST" "$PREV" || orb_die "cannot write $PREV -- refusing to flash without the rollback in place"
  fi
  cp "$B/gorb-os.bin" "$LAST" || orb_die "cannot write $LAST -- refusing to flash without the snapshot in place"
  echo "SNAPSHOT last=$LAST previous=$PREV" | tee -a "$AUDLOG"
fi

sudo "$PY" -m esptool --chip esp32s3 -p "$ORB_PORT" -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x20000 "$APP" 0x11000 "$B/ota_data_initial.bin" \
  > "$LOGFILE" 2>&1
echo "EXIT=$?" >> "$LOGFILE"
echo "SERFLASH_DONE $(date +%T)" >> "$LOGFILE"
