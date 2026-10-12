#!/usr/bin/env python3
"""Capture serial from the orb, with an OPTIONAL reset at the start.

tools/grab_boot.py always toggles RTS, which is right for a boot log and wrong
for watching anything time-based: the reset restarts the clock being measured.
Idle dim needs both modes — reset to start the ten-minute timer from a known
point, then a plain read window to catch the transition after the timer fires.

The port comes from this clone's untracked .orb-target behind the esptool
read_mac gate (tools/orb_bind): opening the wrong orb's port steals its console
and resets it, so there is no bare /dev/ttyACMx here anymore.

Usage: cap.py <seconds> [reset]
"""
import serial, sys, time, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orb_bind

secs = float(sys.argv[1])
reset = len(sys.argv) > 2 and sys.argv[2] == "reset"
cfg = orb_bind.load()
port = orb_bind.resolve_port(cfg)
orb_bind.gate_mac(cfg, port)
s = serial.Serial(port, 115200, timeout=0.3)
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
