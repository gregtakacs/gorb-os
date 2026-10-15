#!/bin/bash
# Host check of the lonely image's drawing code — no board, no ESP-IDF.
#
# The CO5300 takes a flush window that starts and spans even counts on both axes,
# big-endian pixels, one band buffer in flight at a time, a full-panel first
# paint before the glass lights up, the burst opened on the TE pulse, and the
# whole panel repainted every refresh at a fixed step. A build that misses any of those still
# compiles and still flashes; it just comes out green, smeared and unreadable.
# This runs the SHIPPED drawing functions against a fake panel and fails on the
# first violation.
#
# Run from anywhere:  bash lonely/test/run.sh   (needs a host C compiler)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../main/lonely_main.c"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Everything above the panel bring-up: the geometry, the palette and the drawing
# code, with the device includes dropped. Cut on the banner, not a line number.
cut=$(grep -n 'Panel bring-up' "$src" | cut -d: -f1)
[ -n "$cut" ] || { echo "lonely_main.c no longer carries the Panel bring-up banner" >&2; exit 1; }
sed "$((cut - 1))q" "$src" | sed '$d' | grep -v '^#include' > "$out/scene.inc"

# render_check.c declares the font table itself; the IDF surface it stubs.
gcc -O1 -w -std=gnu11 -I "$here/../main" -I "$out" \
    -o "$out/render_check" "$here/render_check.c" "$here/../main/font5x7.c" -lm
"$out/render_check"
echo "lonely render check: the six CO5300 rules hold"
