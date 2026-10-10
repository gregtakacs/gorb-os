#!/bin/bash
# One rows-capacity bench point. The clk_theme struct compiles INTO the app
# (assets/clock_assets.h is static const in the binary); only the pixel blob
# lives in the assets partition. So a theme-changing bake MUST rebuild and
# re-flash the app too -- assets-only flashing leaves the old theme running.
# $1 label $2 theme $3 frames|NONE $4 tourbillon|NONE $5 secs $6 bgfps $7 tickrate $8 maxrects $9 maxflush $10 tourb-fps
set -e
R=/home/greg/docker/Projects/gorb-os
say(){ echo "[$1] $*" >> /tmp/bench2/points.log; }
say "$1 start frames=$3 tourb=$4 bgfps=$6 tick=$7 mr=$8 mf=$9"
cd $R
A=($2 --bin assets/clock_assets.bin --header assets/clock_assets.h --dump-dir /tmp/bench2/dump_$1)
if [ "$3" != NONE ]; then A+=(--bg-frames $3 --bg-fps $6 --bg-max-rects ${8:-16} --bg-max-flush ${9:-0.62}); fi
if [ "$4" != NONE ]; then A+=(--tourbillon $4 --tourbillon-fps ${10:-$6}); fi
A+=(--tick-rate $7)
/tmp/bakevenv/bin/python tools/bake_clock.py "${A[@]}" > /tmp/bench2/$1.bake.log 2>&1 \
  || { say "$1 BAKE FAILED"; exit 1; }
grep -q '^wrote assets/clock_assets.bin' /tmp/bench2/$1.bake.log || { say "$1 BAKE NO BIN"; exit 1; }
say "$1 baked $(date +%T)"
. $HOME/esp/esp-idf/export.sh >/dev/null 2>&1
idf.py build > /tmp/bench2/$1.build.log 2>&1 || { say "$1 BUILD FAILED"; exit 1; }
/tmp/serflash.sh
grep -q 'EXIT=0' /tmp/serflash.log || { say "$1 APP FLASH FAILED"; exit 1; }
/tmp/serflash_assets.sh
grep -q 'EXIT=0' /tmp/serflash_assets.log || { say "$1 ASSETS FLASH FAILED"; exit 1; }
say "$1 flashed $(date +%T)"
sleep 8
/tmp/bakevenv/bin/python tools/bench/cap.py ${5:-30} /tmp/bench2/$1.log >/dev/null 2>&1 || say "$1 CAP FAILED"
say "$1 captured $(grep -c 'CAD f=' /tmp/bench2/$1.log) cad lines $(date +%T)"
python3 tools/bench/cadence.py /tmp/bench2/$1.log --csv $1 | tail -1 >> /tmp/bench2/results.csv || say "$1 CADENCE FAILED"
say "$1 done $(date +%T)"
