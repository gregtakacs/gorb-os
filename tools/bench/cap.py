#!/usr/bin/env python3
"""Capture N seconds of serial after an RTS reset. Usage: cap.py [secs] [out]."""
import sys, time
import serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 32.0
out = open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.dtr = False
s.rts = True
time.sleep(0.15)
s.rts = False
t0 = time.time()
while time.time() - t0 < secs:
    line = s.readline()
    if line:
        out.write(line.decode("utf-8", "replace"))
        out.flush()
s.close()
