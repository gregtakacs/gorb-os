#!/usr/bin/env python3
"""Catch the orb's internal-RAM stranding as it happens, on one timeline.

The fault (bench, 2026-10-31): some time into a session the bound orb's internal
heap drops to ~4 KB free with a 1.5 KB largest block and stays there until a
reboot. Card reads then take 16 s and preview bodies come back empty. Neither
the serial log nor a later /status read can say what was happening at the
moment it dropped, so this tool records three things against one wall clock:

  timeline.txt  every serial line, every heap poll, every change in the host's
                TCP connections to the orb, merged in arrival order
  serial.txt    the raw serial capture (capf.py's stamped format)
  heap.tsv      the polls alone: epoch, free, largest, uptime, rst

Run the hub with -trace-orb, its log to a file, alongside; its "orb>" lines
share this clock. When the largest block stays under --low for three polls in a
row, the tool marks DROP, keeps recording --after seconds more, and exits.

The capture holds the cable's .press flag the whole time, so no hub sweep on
this box resets the board mid-run. Opening the port resets the board once at
the start (see capf.py): the run begins from a fresh boot.

Usage: heap_watch.py [--minutes 90] [--low 4096] [--after 120] [--poll 2] [--out DIR]
"""
import argparse, fcntl, json, os, subprocess, sys, threading, time, urllib.request

import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import orb_bind

ap = argparse.ArgumentParser()
ap.add_argument("--minutes", type=float, default=90.0, help="give up after this long without a drop")
ap.add_argument("--low", type=int, default=4096, help="largest-block threshold that counts as stranded")
ap.add_argument("--after", type=float, default=120.0, help="seconds to keep recording after the drop")
ap.add_argument("--poll", type=float, default=2.0, help="seconds between /status polls")
ap.add_argument("--out", default="", help="output directory (default /tmp/heapwatch-<time>)")
args = ap.parse_args()

out = args.out or time.strftime("/tmp/heapwatch-%Y%m%d-%H%M%S")
os.makedirs(out, exist_ok=True)

cfg = orb_bind.load()
port = orb_bind.resolve_port(cfg)
ip = orb_bind.gate_ip(cfg)

# The hub's own press flag (internal/orbbind press.go): every hub sweep and probe
# on this box declines a cable whose flag is held, so the capture cannot be
# answered with a reset. Same name the Go side derives from the resolved device.
press_path = "/tmp/orb-port-%s.press" % os.path.basename(os.path.realpath(port))
press = open(press_path, "w")
try:
    fcntl.flock(press, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    orb_bind._die("%s is pressed by another tool -- a flash or capture is running" % port)

orb_bind.gate_mac(cfg, port)   # the chip on the cable is the bound orb

lock = threading.Lock()
timeline = open(os.path.join(out, "timeline.txt"), "w")
stop = threading.Event()


def emit(kind, text):
    stamp = "%.3f %s %s" % (time.time(), kind, text)
    with lock:
        timeline.write(stamp + "\n")
        timeline.flush()
    if kind != "SER":
        print(time.strftime("%H:%M:%S ") + kind + " " + text, flush=True)


def read_serial():
    s = serial.Serial(port, 115200, timeout=0.4)
    with open(os.path.join(out, "serial.txt"), "w") as raw:
        while not stop.is_set():
            b = s.readline()
            if not b:
                continue
            line = b.decode("utf-8", "replace").rstrip("\r\n")
            raw.write("%.3f %s\n" % (time.time(), line))
            raw.flush()
            emit("SER", line)
    s.close()


def status():
    with urllib.request.urlopen("http://%s/api/v1/status" % ip, timeout=8) as r:
        return json.load(r)


def connections():
    # The host's view of who is talking to the orb: hub, browser relay, curl.
    r = subprocess.run(["ss", "-tnpH", "dst", ip], capture_output=True, text=True)
    rows = []
    for ln in r.stdout.splitlines():
        f = ln.split()
        if len(f) >= 4:
            proc = f[5].split('"')[1] if len(f) > 5 and '"' in f[5] else "?"
            rows.append("%s:%s:%s" % (f[0], f[3].rsplit(":", 1)[-1], proc))
    return sorted(rows)


def watch_conns():
    last = None
    while not stop.is_set():
        now = connections()
        if now != last:
            emit("TCP", " ".join(now) if now else "(none)")
            last = now
        time.sleep(0.1)


threading.Thread(target=read_serial, daemon=True).start()
threading.Thread(target=watch_conns, daemon=True).start()
emit("RUN", "capturing %s at %s into %s; drop = largest < %d for 3 polls" % (cfg["ORB_LABEL"], ip, out, args.low))

heap = open(os.path.join(out, "heap.tsv"), "w")
heap.write("epoch\tfree\tlargest\tuptime_s\trst\n")
deadline = time.time() + args.minutes * 60
low_run, dropped_at, booted, worst = 0, None, False, None
while time.time() < deadline and (dropped_at is None or time.time() - dropped_at < args.after):
    t = time.time()
    try:
        d = status()
        free, largest = d["heap_internal"], d["heap_internal_largest"]
        up = d.get("idle", {}).get("since_s", -1)
        heap.write("%.3f\t%d\t%d\t%d\t%s\n" % (t, free, largest, up, d.get("rst_name", "")))
        heap.flush()
        emit("HEAP", "free=%d largest=%d up=%ss" % (free, largest, up))
        if not booted and 0 <= up < 120:
            booted = True   # the capture's own reset has landed; count from here
        if booted:
            worst = largest if worst is None else min(worst, largest)
            low_run = low_run + 1 if largest < args.low else 0
            if low_run == 3 and dropped_at is None:
                dropped_at = time.time()
                emit("DROP", "largest block under %d for 3 polls; recording %ds more" % (args.low, args.after))
    except Exception as e:
        emit("HEAP", "no answer: %s" % e)
    time.sleep(max(0.0, args.poll - (time.time() - t)))

stop.set()
time.sleep(0.6)
emit("END", ("DROP caught" if dropped_at else "no drop in %.0f min" % args.minutes)
     + "; worst largest block %s; files in %s" % (worst, out))
sys.exit(0 if dropped_at else 3)
