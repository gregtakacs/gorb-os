#!/usr/bin/env python3
"""Capture N seconds of serial after an RTS reset. Usage: cap.py [secs] [out].

The port comes from this clone's .orb-target behind the read_mac gate -- see
tools/capf.py for the full why; no bare /dev/ttyACMx anymore.
"""
import sys, time, os
import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import orb_bind

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 32.0
out = open(sys.argv[2], "w") if len(sys.argv) > 2 else sys.stdout
cfg = orb_bind.load()
port = orb_bind.resolve_port(cfg)
orb_bind.gate_mac(cfg, port)
s = serial.Serial(port, 115200, timeout=0.3)
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
