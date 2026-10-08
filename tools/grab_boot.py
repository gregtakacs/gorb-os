#!/usr/bin/env python3
"""Grab ~14s of boot + steady-state serial from the orb after an RTS reset."""
import serial, time, sys

s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3)
s.dtr = False
s.rts = True          # EN asserted -> hold in reset
time.sleep(0.15)
s.rts = False         # release -> boots from scratch
t0 = time.time()
while time.time() - t0 < 14:
    line = s.readline()
    if line:
        sys.stdout.write(line.decode('utf-8', 'replace'))
        sys.stdout.flush()
s.close()
