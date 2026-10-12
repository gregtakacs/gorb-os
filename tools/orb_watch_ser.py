#!/usr/bin/env python3
"""Continuous serial capture for a scripted on-dial walk, in one reader thread.

The bench tools that read the port (cap.py, capf.py, grab_boot.py) each open the
port once and read it on the MAIN thread for a fixed window. That is right for a
boot log and wrong for watching a walk we drive from another shell: a capture
whose reader and its driver share one thread stops draining between the reads the
driver does, and the 6 fps clock paints fast enough that the OS serial buffer
overruns there and drops exactly the lines we fired the burst to see. This tool
keeps ONE daemon thread draining the byte stream for the whole run, so the only
thing that touches the port is the reader and nothing starves it.

It also survives a reboot. A staged theme restarts the device, and native
USB-Serial-JTAG re-enumerates on the way back: the by-id path disappears, the
open handle dies, and it returns seconds later. The reader thread catches the
death, waits for the path to reappear, and reopens — so one capture spans
"staged" through "install of ... ok" without a gap. Opening the port resets the
board (the DTR/RTS edge pyserial applies), which is why the run begins on a boot
and why a walk always starts from home.

The port and MAC gates are orb_bind's, exactly as every other device tool uses
them: this clone's .orb-target names the one orb, esptool read_mac confirms the
chip before the first open, and the per-orb /tmp flock makes a second writer
refuse. Nothing here touches the other orb.

Usage: orb_watch_ser.py <seconds> <outfile> [--stop-file PATH]
  seconds     hard wall-clock bound; the run ends by itself even if --stop-file
              is never written, so a forgotten capture cannot hold the port.
  outfile     every line, prefixed with a wall clock (%.3f) so a caller that
              fired a burst at a known instant can slice the log by that instant.
  --stop-file PATH
              poll this path; end the run as soon as it exists. Lets a driver
              say "I have the lines I need, release the port now".

Exit codes: 0 on a clean bounded/stop-file end, 2 on a gate refusal (orb_bind),
3 if the port never came back up (no re-enumeration to catch, or unplugged).
"""
import os, sys, time, threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial
import orb_bind

BAUD = 115200


def parse(argv):
    if len(argv) < 3:
        sys.exit("usage: orb_watch_ser.py <seconds> <outfile> [--stop-file PATH]")
    secs = float(argv[1])
    out = argv[2]
    stop = None
    if "--stop-file" in argv:
        i = argv.index("--stop-file")
        if i + 1 >= len(argv):
            sys.exit("--stop-file needs a path")
        stop = argv[i + 1]
    return secs, out, stop


def main():
    secs, out, stop = parse(sys.argv)

    cfg = orb_bind.load()                 # refuses if this clone is unbound
    port = orb_bind.resolve_port(cfg)     # refuses if the bound orb is absent
    orb_bind.gate_mac(cfg, port)          # takes the per-orb flock, proves the chip

    stop_ev = threading.Event()
    died_forever = threading.Event()
    count = {"lines": 0}

    def reader():
        t_end = time.time() + secs
        buf = b""
        f = open(out, "w")

        def wline(raw):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if line == "":
                return
            f.write("%9.3f %s\n" % (time.time(), line))
            f.flush()
            count["lines"] += 1

        while time.time() < t_end and not stop_ev.is_set():
            s = None
            # Open (retrying while the path is gone — the reboot window). Give up
            # only if it never returns within the run's own bound.
            while s is None and time.time() < t_end and not stop_ev.is_set():
                try:
                    s = serial.Serial(port, BAUD, timeout=0.2)
                except (serial.SerialException, OSError):
                    f.write("%9.3f -- waiting for %s --\n" % (time.time(), port))
                    f.flush()
                    time.sleep(0.2)
            if s is None:
                break
            # Drain the raw stream. in_waiting keeps us off the timeout path when
            # bytes are ready, so a fast paint burst is pulled in whole chunks.
            try:
                while time.time() < t_end and not stop_ev.is_set():
                    n = s.in_waiting
                    data = s.read(n if n else 1)
                    if not data:
                        continue
                    buf += data
                    while b"\n" in buf:
                        raw, _, buf = buf.partition(b"\n")
                        wline(raw)
            except (serial.SerialException, OSError):
                # The re-enumeration: the handle died mid-boot. Drop it, note the
                # gap so the log is honest about it, and loop back to reopen.
                f.write("%9.3f -- serial dropped (re-enumeration?) --\n" % time.time())
                f.flush()
            finally:
                try:
                    s.close()
                except Exception:
                    pass

        # Flush any trailing partial line that never saw its newline.
        if buf:
            wline(buf)
        f.close()

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    # The main thread only watches for the stop-file and the hard bound; it never
    # touches the port. This is the whole point of the dedicated reader.
    t_end = time.time() + secs
    while time.time() < t_end and not stop_ev.is_set():
        if stop and os.path.exists(stop):
            stop_ev.set()
            break
        time.sleep(0.15)
    stop_ev.set()
    th.join(timeout=3.0)

    print("captured %d line(s) -> %s" % (count["lines"], out), flush=True)
    if count["lines"] == 0:
        sys.exit(3)


if __name__ == "__main__":
    main()
