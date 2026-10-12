#!/usr/bin/env python3
"""Grab ~14s of boot + steady-state serial from the orb after an RTS reset.

The port comes from this clone's .orb-target behind the read_mac gate (see
tools/capf.py for the why); no bare /dev/ttyACMx anymore.
"""
import serial, time, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orb_bind

cfg = orb_bind.load()
port = orb_bind.resolve_port(cfg)
orb_bind.gate_mac(cfg, port)
s = serial.Serial(port, 115200, timeout=0.3)
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
