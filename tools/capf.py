#!/usr/bin/env python3
"""Serial capture for the bench: hard-reset the board, read the port, write it out.

The port is never a positional guess. tools/orb_bind resolves it from the
clone's untracked .orb-target and esptool read_mac confirms the chip's own MAC
BEFORE the capture opens the port: opening the wrong board steals its console
AND resets it (the caveat below), so the wrong-orb gate is the reason this file
is tracked here instead of living in /tmp.

Nothing else may hold the port while tools/serflash.sh runs; start this only
after SERFLASH_DONE has landed. Lines are written as they arrive, so a caller
can stop the capture at any moment and still read everything up to there --
waiting out a fixed duration just holds the port hostage.

Caveat the bench had to learn: OPENING the port resets the board by itself. The
S3 USB-CDC driver acts on the DTR/RTS edges pyserial applies on open, which is
why a boot log read this way always begins with rst:0x15 (USB_UART_CHIP_RESET).
--no-reset only skips the extra deliberate pulse; it cannot un-reset the open.
Capture once per run and segment the log afterwards, not once per trial.

Usage: capf.py <seconds> <outfile> [--no-reset]
"""
import os, sys, time
import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orb_bind

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
out  = sys.argv[2] if len(sys.argv) > 2 else "/tmp/serial.txt"
do_reset = "--no-reset" not in sys.argv[3:]

cfg  = orb_bind.load()
port = orb_bind.resolve_port(cfg)
orb_bind.gate_mac(cfg, port)

BAUD = 115200
s = serial.Serial(port, BAUD, timeout=0.4)
if do_reset:
    s.dtr = False
    s.rts = True
    time.sleep(0.15)
    s.rts = False
t0 = time.time()
with open(out, "w") as f:
    while time.time() - t0 < secs:
        b = s.readline()
        if b:
            line = b.decode("utf-8", "replace").rstrip("\r\n")
            # Wall clock on every line, so a caller that fired a burst at a known
            # instant can slice the log by that instant instead of guessing trial
            # boundaries from gaps in the device's own ms clock.
            stamped = f"{time.time():.3f} {line}"
            f.write(stamped + "\n")
            f.flush()
            print(f"{time.time()-t0:6.2f} {line}", flush=True)
s.close()
