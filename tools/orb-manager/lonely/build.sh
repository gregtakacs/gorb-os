#!/bin/bash
# Build the lonely image and stage it for the hub's go:embed.
# Run from anywhere; requires ESP-IDF v5.4 (the same tree the firmware builds with).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. /home/greg/esp/esp-idf/export.sh >/dev/null
idf.py -C "$here" set-target esp32s3 >/dev/null
idf.py -C "$here" build
cp "$here/build/lonely.bin" "$here/lonely.bin"
ls -l "$here/lonely.bin"
echo "lonely.bin staged for //go:embed in lonely.go"