#!/bin/bash
# Serial flash of the captive-portal build to ota_0 (the reliable path here --
# WiFi OTA aborts mid-stream at rssi ~-79). otadata reset to boot ota_0; ota_1
# (3301a0c) is left intact as the self-test rollback. App-only: bootloader,
# partition table and assets are unchanged and are not re-sent.
B=/home/greg/docker/Projects/gorb-os-2/build   # dev2 working copy; was the gorb-os upstream clone (theme-naming-todo sec.6)
PY=/home/greg/.espressif/python_env/idf5.4_py3.14_env/bin/python

# Audit-loop guard: makes the .clinerules:165 check real instead of hopeful.
# Why: /api/v1/status's `fw` comes from esp_app_desc_t, which CMake stamps at
# CONFIGURE time and caches. `idf.py build` alone recompiles changed sources but
# keeps the old stamp, so a binary carrying uncommitted edits can report a clean
# hash it was never built from. Observed 2026-10-11: main/art/art_bank.c edited
# 14:46:10, image built 14:46:31, and the .bin still said "54094e8" with no -dirty
# for a tree that had moved twice since. Read the stamp out of the image itself --
# esp_app_desc_t.version, at byte 48 of the app .bin -- and refuse to flash unless
# it equals `git describe --always --dirty` for the tree it is meant to represent.
: > /tmp/serflash_audit.log
WANT=$(git -C "$B/.." describe --always --dirty 2>/dev/null)
HAVE=$(dd if="$B/gorb-os.bin" bs=1 skip=48 count=32 status=none | tr -d '\0')
{
  echo "AUDIT want=$WANT have=$HAVE" | tee -a /tmp/serflash_audit.log
  if [ "$HAVE" != "$WANT" ]; then
    echo "AUDIT FAIL: build/gorb-os.bin is stamped [$HAVE]; the tree is [$WANT]." | tee -a /tmp/serflash_audit.log
    echo "        The image does not correspond to the code you are about to sign off."
    echo "        Fix: (cd $(dirname $B) && idf.py reconfigure && idf.py build), re-run."
    echo "SERFLASH_DONE $(date +%T)"
    exit 1
  fi
  echo "AUDIT OK: the image's own stamp matches the working tree." | tee -a /tmp/serflash_audit.log
}

sudo "$PY" -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x20000 "$B/gorb-os.bin" 0x11000 "$B/ota_data_initial.bin" \
  > /tmp/serflash.log 2>&1
echo "EXIT=$?" >> /tmp/serflash.log
echo "SERFLASH_DONE $(date +%T)" >> /tmp/serflash.log
