#!/usr/bin/env python3
"""Capture serial from the orb, with an OPTIONAL reset at the start.

tools/grab_boot.py always toggles RTS, which is right for a boot log and wrong
for watching anything time-based: the reset restarts the clock being measured.
Idle dim needs both modes — reset to start the ten-minute timer from a known
point, then a plain read window to catch the transition after the timer fires.

Usage: cap.py <seconds> [reset]
"""
import serial, sys, time

secs = float(sys.argv[1])
reset = len(sys.argv) > 2 and sys.argv[2] == "reset"
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3)
if reset:
    s.dtr = False
    s.rts = True            # EN asserted -> hold in reset
    time.sleep(0.15)
    s.rts = False           # release -> boots from scratch
t0 = time.time()
while time.time() - t0 < secs:
    line = s.readline()
    if line:
        sys.stdout.write(line.decode('utf-8', 'replace'))
        sys.stdout.flush()
s.close()
